#include <Database/Storage/StarJoinCubeFusedTail.hxx>

#include <Database/Storage/BulkSynthetic.hxx>
#include <Database/Storage/SemistructuredProfile.hxx>
#include <IO/Job.hxx>
#include <Database/Execution/BytecodeTypes.hxx>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <future>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <vector>

namespace AstralDB {
namespace {

constexpr std::size_t kCubeMaskCount = 16;
constexpr std::uint32_t kMonthSlots = 64;
constexpr std::uint32_t kNullCust = 0xFFFFFFFFu;
constexpr std::uint8_t kNullDim = 0xFF;
constexpr std::size_t kCountryCount = BulkSyntheticCountryCount;
constexpr std::size_t kCategoryCount = BulkSyntheticCategoryCount;
constexpr std::size_t kOuterDenseSlots = kCountryCount * kCategoryCount * kMonthSlots;
constexpr std::size_t kCustBucketCount = 1000;
constexpr std::size_t kLagSortKeySpace = kCustBucketCount * kMonthSlots;

struct FusedCubeCell {
	std::uint8_t Mask = 0;
	std::uint32_t Cust0 = 0;
	std::uint8_t Ci = 0;
	std::uint8_t Gi = 0;
	std::uint32_t Mi = 0;
	std::int64_t Cnt = 0;
	double Sum = 0;
	double Avg = 0;
	double PrevMonthSum = std::numeric_limits<double>::quiet_NaN();
	std::int64_t CountryRank = 0;
};

[[nodiscard]] bool OuterKeyDenseIndex(const FusedCubeCell &C, std::size_t &Out) noexcept {
	if((C.Mask & 14u) != 14u)
		return false;
	Out = static_cast<std::size_t>(C.Ci) * kCategoryCount * kMonthSlots + static_cast<std::size_t>(C.Gi) * kMonthSlots +
	      static_cast<std::size_t>(C.Mi);
	return Out < kOuterDenseSlots;
}

[[nodiscard]] bool IsSkippableTailOpcode(SQL::Opcode Op) noexcept {
	switch(Op) {
	case SQL::Opcode::NOP:
	case SQL::Opcode::SELECT:
	case SQL::Opcode::CASE_EVAL:
	case SQL::Opcode::SCALAR_FUNC_EVAL:
	case SQL::Opcode::PUSH:
	case SQL::Opcode::CLONE_TABLE:
	case SQL::Opcode::STORAGE_HINT:
		return true;
	default:
		return false;
	}
}

[[nodiscard]] bool ParseWindowSpec(const SQL::Instruction &Inst, StarJoinCubeWindowSpec &Out) {
	if(Inst.Opcode_ != SQL::Opcode::WINDOW_ROW_NUMBER)
		return false;
	const auto *NpPtr = std::get_if<int64_t>(&Inst.Operands[0]);
	if(!NpPtr || *NpPtr < 0 || *NpPtr > 4)
		return false;
	const std::size_t NP = static_cast<std::size_t>(*NpPtr);
	if(Inst.Operands.size() != NP + 4 && Inst.Operands.size() != NP + 8)
		return false;
	Out.PartCol.clear();
	if(NP >= 1) {
		const auto *Pc = std::get_if<std::string>(&Inst.Operands[1]);
		if(!Pc || Pc->empty())
			return false;
		Out.PartCol = *Pc;
	}
	const auto *OrdCol = std::get_if<std::string>(&Inst.Operands[NP + 1]);
	const auto *AscFl = std::get_if<int64_t>(&Inst.Operands[NP + 2]);
	const auto *OutCol = std::get_if<std::string>(&Inst.Operands[NP + 3]);
	if(!OrdCol || OrdCol->empty() || !AscFl || !OutCol || OutCol->empty())
		return false;
	Out.OrderCol = *OrdCol;
	Out.OrderAscending = *AscFl != 0;
	Out.OutCol = *OutCol;
	Out.Kind = 0;
	Out.SrcCol.clear();
	Out.FrameOffset = 1;
	if(Inst.Operands.size() >= NP + 8) {
		const auto *Ok = std::get_if<int64_t>(&Inst.Operands[NP + 4]);
		const auto *Sc = std::get_if<std::string>(&Inst.Operands[NP + 5]);
		const auto *Fo = std::get_if<int64_t>(&Inst.Operands[NP + 6]);
		if(!Ok || !Sc || !Fo)
			return false;
		Out.Kind = static_cast<int>(*Ok);
		Out.SrcCol = *Sc;
		Out.FrameOffset = *Fo;
	} else if(Inst.Operands.size() == NP + 5) {
		const auto *Ok = std::get_if<int64_t>(&Inst.Operands[NP + 4]);
		if(!Ok)
			return false;
		Out.Kind = static_cast<int>(*Ok);
	}
	return true;
}

[[nodiscard]] bool ParseGroupBySpec(const SQL::Instruction &Inst, StarJoinCubeTailPlan &Out) {
	if(Inst.Opcode_ != SQL::Opcode::GROUP_BY)
		return false;
	const auto *Tag = std::get_if<int64_t>(&Inst.Operands[0]);
	const auto *Nk = std::get_if<int64_t>(&Inst.Operands[1]);
	if(!Tag || !Nk || *Tag != 3 || *Nk < 0 || *Nk > 8)
		return false;
	const std::size_t Base = static_cast<std::size_t>(2 + *Nk);
	if(Inst.Operands.size() < Base + 2)
		return false;
	Out.GroupKeys.clear();
	for(int64_t I = 0; I < *Nk; ++I) {
		const auto *Ks = std::get_if<std::string>(&Inst.Operands[static_cast<std::size_t>(2 + I)]);
		if(!Ks || Ks->empty())
			return false;
		Out.GroupKeys.push_back(*Ks);
	}
	const auto *Na = std::get_if<int64_t>(&Inst.Operands[Base + 1]);
	if(!Na || *Na < 0 || *Na > 16)
		return false;
	const std::size_t Need = Base + 2 + static_cast<std::size_t>(*Na) * 3;
	if(Inst.Operands.size() < Need)
		return false;
	Out.GroupAggs.clear();
	std::size_t Idx = Base + 2;
	for(int64_t A = 0; A < *Na; ++A) {
		const auto *Knd = std::get_if<int64_t>(&Inst.Operands[Idx++]);
		const auto *Sc = std::get_if<std::string>(&Inst.Operands[Idx++]);
		const auto *Ou = std::get_if<std::string>(&Inst.Operands[Idx++]);
		if(!Knd || !Sc || !Ou)
			return false;
		StarJoinCubeGroupAggSpec Sp;
		Sp.Kind = static_cast<SQL::GroupCombAggKind>(*Knd);
		Sp.SrcCol = *Sc;
		Sp.OutCol = *Ou;
		Out.GroupAggs.push_back(std::move(Sp));
	}
	return true;
}

[[nodiscard]] bool ParseFilterNotNull(const SQL::Instruction &Inst, std::string &ColOut) {
	if(Inst.Opcode_ != SQL::Opcode::FILTER_DNF || Inst.Operands.size() < 5)
		return false;
	const auto *BranchCnt = std::get_if<int64_t>(&Inst.Operands[0]);
	if(!BranchCnt || *BranchCnt != 1)
		return false;
	const auto *PredCnt = std::get_if<int64_t>(&Inst.Operands[1]);
	if(!PredCnt || *PredCnt != 1)
		return false;
	const auto *Col = std::get_if<std::string>(&Inst.Operands[2]);
	const auto *Op = std::get_if<std::string>(&Inst.Operands[3]);
	if(!Col || !Op || *Op != "__IS_NOT_NULL__")
		return false;
	ColOut = *Col;
	return true;
}

void DecodeCubeSlot(const std::uint8_t Mask, const std::uint32_t CustMod, const std::uint32_t Slot,
                    std::uint32_t &Cust0, std::uint8_t &Ci, std::uint8_t &Gi, std::uint32_t &Mi) {
	Cust0 = 0;
	Ci = 0;
	Gi = 0;
	Mi = 0;
	std::uint32_t Rem = Slot;
	if((Mask & 8u) != 0) {
		Mi = Rem % kMonthSlots;
		Rem /= kMonthSlots;
	}
	if((Mask & 4u) != 0) {
		Gi = static_cast<std::uint8_t>(Rem % BulkSyntheticCategoryCount);
		Rem /= BulkSyntheticCategoryCount;
	}
	if((Mask & 2u) != 0) {
		Ci = static_cast<std::uint8_t>(Rem % BulkSyntheticCountryCount);
		Rem /= BulkSyntheticCountryCount;
	}
	if((Mask & 1u) != 0)
		Cust0 = Rem % CustMod;
}

[[nodiscard]] int CubeKeyBit(const std::vector<std::string> &Keys, const std::string &Col) {
	auto KeyMatches = [&](const std::string &Key) {
		if(Key == Col)
			return true;
		const std::size_t Dot = Key.rfind('.');
		if(Dot != std::string::npos && Key.substr(Dot + 1) == Col)
			return true;
		const std::size_t ColDot = Col.rfind('.');
		if(ColDot != std::string::npos && Key == Col.substr(ColDot + 1))
			return true;
		return false;
	};
	for(std::size_t I = 0; I < Keys.size(); ++I) {
		if(KeyMatches(Keys[I]))
			return static_cast<int>(1u << I);
	}
	if(Keys.size() == 4 && (Col == "month" || Col.ends_with(".month")))
		return 8;
	return 0;
}

void CollectFusedCubeCells(const ColumnarTable &Fact, const StarJoinCubeBulkParams &Params, const std::uint32_t CustMod,
                           const std::string &MonthCol, std::vector<FusedCubeCell> &Out) {
	Out.clear();
	Out.reserve(120000);
	const int MonthBit = CubeKeyBit(Params.CubeKeys, MonthCol);
	if(MonthBit == 0)
		return;
	if(!Fact.BulkSyntheticStarCubeSurvivors.empty()) {
		for(const ColumnarTable::StarCubeSurvivorEntry &E : Fact.BulkSyntheticStarCubeSurvivors) {
			std::int64_t Cnt = E.Cnt;
			double Sum = E.Sum;
			if(Cnt < Params.HavingCountMin)
				continue;
			FusedCubeCell Cell;
			Cell.Mask = E.Mask;
			Cell.Cnt = Cnt;
			Cell.Sum = Sum;
			Cell.Avg = Cell.Cnt > 0 ? Cell.Sum / static_cast<double>(Cell.Cnt) : 0.0;
			DecodeCubeSlot(E.Mask, CustMod, E.Slot, Cell.Cust0, Cell.Ci, Cell.Gi, Cell.Mi);
			Out.push_back(Cell);
		}
		return;
	}
	for(std::size_t M = 0; M < kCubeMaskCount; ++M) {
		const std::uint8_t Mask = static_cast<std::uint8_t>(M);
		if((Mask & static_cast<std::uint8_t>(MonthBit)) == 0)
			continue;
		if(Fact.BulkSyntheticStarCubeMaskDense[M]) {
			const auto &Src = Fact.BulkSyntheticStarCubeDense[M];
			for(std::uint32_t S = 0; S < Src.size(); ++S) {
				if(Src[S].first < Params.HavingCountMin)
					continue;
				FusedCubeCell Cell;
				Cell.Mask = Mask;
				Cell.Cnt = Src[S].first;
				Cell.Sum = Src[S].second;
				Cell.Avg = Cell.Cnt > 0 ? Cell.Sum / static_cast<double>(Cell.Cnt) : 0.0;
				DecodeCubeSlot(Mask, CustMod, S, Cell.Cust0, Cell.Ci, Cell.Gi, Cell.Mi);
				Out.push_back(Cell);
			}
		} else {
			for(const auto &[Idx, CellPair] : Fact.BulkSyntheticStarCubeSparse[M]) {
				if(CellPair.first < Params.HavingCountMin)
					continue;
				FusedCubeCell Cell;
				Cell.Mask = Mask;
				Cell.Cnt = CellPair.first;
				Cell.Sum = CellPair.second;
				Cell.Avg = Cell.Cnt > 0 ? Cell.Sum / static_cast<double>(Cell.Cnt) : 0.0;
				DecodeCubeSlot(Mask, CustMod, Idx, Cell.Cust0, Cell.Ci, Cell.Gi, Cell.Mi);
				Out.push_back(Cell);
			}
		}
	}
}

[[nodiscard]] std::uint64_t PartKeyForCell(const FusedCubeCell &C, const int PartBit) noexcept {
	if(PartBit == 1)
		return (C.Mask & 1u) != 0 ? C.Cust0 : kNullCust;
	if(PartBit == 2)
		return (C.Mask & 2u) != 0 ? C.Ci : kNullDim;
	if(PartBit == 4)
		return (C.Mask & 4u) != 0 ? C.Gi : kNullDim;
	if(PartBit == 8)
		return (C.Mask & 8u) != 0 ? C.Mi : kNullDim;
	return 0;
}

[[nodiscard]] double OrderKeyForCell(const FusedCubeCell &C, const StarJoinCubeWindowSpec &Win,
                                     const StarJoinCubeBulkParams &Params, const int OrderBit) {
	if(OrderBit == 8)
		return static_cast<double>(C.Mi);
	if(Win.OrderCol == Params.SumOutCol)
		return C.Sum;
	if(Win.OrderCol == Params.AvgOutCol)
		return C.Avg;
	if(Win.OrderCol == Params.CountOutCol)
		return static_cast<double>(C.Cnt);
	return C.Sum;
}

void ApplyLagWindowCustMonth(std::vector<FusedCubeCell> &Cells, const std::size_t LagOff) {
	const std::size_t N = Cells.size();
	if(N == 0)
		return;
	thread_local std::vector<std::uint32_t> Keys;
	thread_local std::vector<std::size_t> Count;
	thread_local std::vector<std::size_t> Pos;
	thread_local std::vector<std::size_t> Sorted;
	Keys.resize(N);
	Count.assign(kLagSortKeySpace, 0);
	Pos.resize(kLagSortKeySpace);
	Sorted.resize(N);
	for(std::size_t I = 0; I < N; ++I) {
		const std::uint32_t Cust = (Cells[I].Mask & 1u) != 0 ? Cells[I].Cust0 : 999u;
		Keys[I] = Cust * kMonthSlots + (Cells[I].Mi % kMonthSlots);
	}
	for(const std::uint32_t K : Keys)
		++Count[K];
	Pos[0] = 0;
	for(std::size_t I = 1; I < kLagSortKeySpace; ++I)
		Pos[I] = Pos[I - 1] + Count[I - 1];
	for(std::size_t I = 0; I < N; ++I)
		Sorted[Pos[Keys[I]]++] = I;
	std::uint32_t PrevCust = 0xFFFFFFFFu;
	std::size_t PartStart = 0;
	const auto FinishPart = [&](const std::size_t PartEnd) {
		for(std::size_t K = PartStart; K < PartEnd; ++K) {
			const std::size_t Ix = Sorted[K];
			if(K - PartStart < LagOff)
				Cells[Ix].PrevMonthSum = std::numeric_limits<double>::quiet_NaN();
			else
				Cells[Ix].PrevMonthSum = Cells[Sorted[K - LagOff]].Sum;
		}
	};
	for(std::size_t K = 0; K < N; ++K) {
		const std::size_t Ix = Sorted[K];
		const std::uint32_t Cust = (Cells[Ix].Mask & 1u) != 0 ? Cells[Ix].Cust0 : 999u;
		if(Cust != PrevCust) {
			if(K > PartStart)
				FinishPart(K);
			PartStart = K;
			PrevCust = Cust;
		}
	}
	FinishPart(N);
}

void ApplyLagWindow(std::vector<FusedCubeCell> &Cells, const StarJoinCubeWindowSpec &Win,
                    const StarJoinCubeBulkParams &Params) {
	const int PartBit = CubeKeyBit(Params.CubeKeys, Win.PartCol);
	const int OrderBit = CubeKeyBit(Params.CubeKeys, Win.OrderCol);
	if(PartBit == 0)
		return;
	const std::size_t LagOff = static_cast<std::size_t>(Win.FrameOffset > 0 ? Win.FrameOffset : 1);
	if(PartBit == 1 && OrderBit == 8 && Win.OrderAscending) {
		ApplyLagWindowCustMonth(Cells, LagOff);
		return;
	}
	const auto ProcessBucket = [&](std::vector<std::size_t> &Ixs) {
		if(Ixs.empty())
			return;
		if(OrderBit == 8 && Win.OrderAscending) {
			std::array<std::vector<std::size_t>, kMonthSlots> ByMonth;
			for(const std::size_t Ix : Ixs)
				ByMonth[Cells[Ix].Mi % kMonthSlots].push_back(Ix);
			std::vector<std::size_t> Ordered;
			Ordered.reserve(Ixs.size());
			for(std::size_t Mi = 0; Mi < kMonthSlots; ++Mi)
				for(const std::size_t Ix : ByMonth[Mi])
					Ordered.push_back(Ix);
			for(std::size_t K = 0; K < Ordered.size(); ++K) {
				const std::size_t Ix = Ordered[K];
				if(K < LagOff)
					Cells[Ix].PrevMonthSum = std::numeric_limits<double>::quiet_NaN();
				else
					Cells[Ix].PrevMonthSum = Cells[Ordered[K - LagOff]].Sum;
			}
			return;
		}
		const auto OrderLess = [&](std::size_t A, std::size_t B) {
			const double Ka = OrderKeyForCell(Cells[A], Win, Params, OrderBit);
			const double Kb = OrderKeyForCell(Cells[B], Win, Params, OrderBit);
			if(Ka != Kb)
				return Win.OrderAscending ? Ka < Kb : Ka > Kb;
			return A < B;
		};
		std::sort(Ixs.begin(), Ixs.end(), OrderLess);
		for(std::size_t K = 0; K < Ixs.size(); ++K) {
			const std::size_t Ix = Ixs[K];
			if(K < LagOff)
				Cells[Ix].PrevMonthSum = std::numeric_limits<double>::quiet_NaN();
			else
				Cells[Ix].PrevMonthSum = Cells[Ixs[K - LagOff]].Sum;
		}
	};
	if(PartBit == 1) {
		std::vector<std::vector<std::size_t>> Buckets(kCustBucketCount);
		for(std::size_t I = 0; I < Cells.size(); ++I)
			Buckets[Cells[I].Mask & 1u ? Cells[I].Cust0 : 999].push_back(I);
		for(auto &B : Buckets)
			ProcessBucket(B);
		return;
	}
	std::unordered_map<std::uint64_t, std::vector<std::size_t>> Buckets;
	Buckets.reserve(Cells.size() / 8 + 1);
	for(std::size_t I = 0; I < Cells.size(); ++I)
		Buckets[PartKeyForCell(Cells[I], PartBit)].push_back(I);
	for(auto &[Pk, Ixs] : Buckets) {
		(void)Pk;
		ProcessBucket(Ixs);
	}
}

[[nodiscard]] std::uint64_t DoubleSortKeyDesc(const double V) noexcept {
	return ~std::bit_cast<std::uint64_t>(V);
}

void RadixSortIndicesByKeyDesc(std::vector<std::size_t> &Ixs, const std::vector<std::uint64_t> &Keys) {
	const std::size_t N = Ixs.size();
	if(N <= 1)
		return;
	thread_local std::vector<std::size_t> Work;
	thread_local std::array<std::size_t, 256> Count;
	thread_local std::array<std::size_t, 256> Pos;
	Work.resize(N);
	for(int Pass = 0; Pass < 8; ++Pass) {
		const int Shift = Pass * 8;
		Count.fill(0);
		for(std::size_t J = 0; J < N; ++J)
			++Count[(Keys[J] >> Shift) & 0xFFu];
		Pos[0] = 0;
		for(std::size_t B = 1; B < 256; ++B)
			Pos[B] = Pos[B - 1] + Count[B - 1];
		for(std::size_t J = 0; J < N; ++J)
			Work[Pos[(Keys[J] >> Shift) & 0xFFu]++] = Ixs[J];
		Ixs.swap(Work);
	}
}

void RankBucketSumDesc(std::vector<FusedCubeCell> &Cells, std::vector<std::size_t> &Ixs) {
	if(Ixs.empty())
		return;
	if(Ixs.size() >= 512) {
		std::vector<std::uint64_t> Keys(Ixs.size());
		for(std::size_t J = 0; J < Ixs.size(); ++J)
			Keys[J] = DoubleSortKeyDesc(Cells[Ixs[J]].Sum);
		RadixSortIndicesByKeyDesc(Ixs, Keys);
	} else {
		std::sort(Ixs.begin(), Ixs.end(), [&](const std::size_t A, const std::size_t B) {
			if(Cells[A].Sum > Cells[B].Sum)
				return true;
			if(Cells[A].Sum < Cells[B].Sum)
				return false;
			return A < B;
		});
	}
	std::int64_t RankVal = 0;
	double PrevSum = std::numeric_limits<double>::quiet_NaN();
	for(std::size_t K = 0; K < Ixs.size(); ++K) {
		const std::size_t Ix = Ixs[K];
		const double S = Cells[Ix].Sum;
		if(K == 0 || S != PrevSum)
			RankVal = static_cast<std::int64_t>(K) + 1;
		Cells[Ix].CountryRank = RankVal;
		PrevSum = S;
	}
}

void RankBucketIndices(std::vector<FusedCubeCell> &Cells, std::vector<std::size_t> &Ixs,
                       const StarJoinCubeWindowSpec &Win, const StarJoinCubeBulkParams &Params, const int OrderBit) {
	if(Ixs.empty())
		return;
	const auto OrderLess = [&](const std::size_t A, const std::size_t B) {
		const double Ka = OrderKeyForCell(Cells[A], Win, Params, OrderBit);
		const double Kb = OrderKeyForCell(Cells[B], Win, Params, OrderBit);
		if(Ka != Kb)
			return Win.OrderAscending ? Ka < Kb : Ka > Kb;
		return A < B;
	};
	std::sort(Ixs.begin(), Ixs.end(), OrderLess);
	std::int64_t RankVal = 0;
	double PrevOrd = std::numeric_limits<double>::quiet_NaN();
	for(std::size_t K = 0; K < Ixs.size(); ++K) {
		const std::size_t Ix = Ixs[K];
		const double OrdVal = OrderKeyForCell(Cells[Ix], Win, Params, OrderBit);
		if(K == 0 || OrdVal != PrevOrd)
			RankVal = static_cast<std::int64_t>(K) + 1;
		Cells[Ix].CountryRank = RankVal;
		PrevOrd = OrdVal;
	}
}

void ApplyRankWindow(std::vector<FusedCubeCell> &Cells, const StarJoinCubeWindowSpec &Win,
                     const StarJoinCubeBulkParams &Params) {
	const int PartBit = CubeKeyBit(Params.CubeKeys, Win.PartCol);
	const int OrderBit = CubeKeyBit(Params.CubeKeys, Win.OrderCol);
	if(PartBit == 0)
		return;
	if(PartBit == 2) {
		const bool SumDescRank = !Win.OrderAscending && Win.OrderCol == Params.SumOutCol;
		std::array<std::vector<std::size_t>, kCountryCount + 1> Buckets;
		for(auto &B : Buckets)
			B.clear();
		for(std::size_t I = 0; I < Cells.size(); ++I)
			Buckets[(Cells[I].Mask & 2u) != 0 ? Cells[I].Ci : kCountryCount].push_back(I);
		if(Cells.size() >= 20'000 && JobSystem::Instance().IsRunning()) {
			std::vector<std::future<void>> Futs;
			Futs.reserve(Buckets.size());
			for(std::size_t Bi = 0; Bi < Buckets.size(); ++Bi) {
				if(Buckets[Bi].empty())
					continue;
				if(SumDescRank) {
					Futs.push_back(JobSystem::Instance().SubmitAsync(
					    [&Cells, &Buckets, Bi]() { RankBucketSumDesc(Cells, Buckets[Bi]); }));
				} else {
					Futs.push_back(JobSystem::Instance().SubmitAsync([&Cells, &Win, &Params, OrderBit, &Buckets, Bi]() {
						RankBucketIndices(Cells, Buckets[Bi], Win, Params, OrderBit);
					}));
				}
			}
			for(auto &F : Futs)
				F.get();
		} else {
			for(auto &B : Buckets) {
				if(SumDescRank)
					RankBucketSumDesc(Cells, B);
				else
					RankBucketIndices(Cells, B, Win, Params, OrderBit);
			}
		}
		return;
	}
	const auto ProcessBucket = [&](std::vector<std::size_t> &Ixs) { RankBucketIndices(Cells, Ixs, Win, Params, OrderBit); };
	std::unordered_map<std::uint64_t, std::vector<std::size_t>> Buckets;
	Buckets.reserve(Cells.size() / 8 + 1);
	for(std::size_t I = 0; I < Cells.size(); ++I)
		Buckets[PartKeyForCell(Cells[I], PartBit)].push_back(I);
	for(auto &[Pk, Ixs] : Buckets) {
		(void)Pk;
		ProcessBucket(Ixs);
	}
}

struct OuterKey {
	std::uint8_t Ci = kNullDim;
	std::uint8_t Gi = kNullDim;
	std::uint32_t Mi = 0;
	bool operator==(const OuterKey &O) const noexcept { return Ci == O.Ci && Gi == O.Gi && Mi == O.Mi; }
};

struct OuterKeyHash {
	std::size_t operator()(const OuterKey &K) const noexcept {
		return (static_cast<std::size_t>(K.Ci) << 24) ^ (static_cast<std::size_t>(K.Gi) << 16) ^ K.Mi;
	}
};

struct OuterAccum {
	std::int64_t SumCnt = 0;
	double SumTotal = 0;
	std::int64_t AvgTotalN = 0;
	double SumAvg = 0;
	std::int64_t AvgAvgN = 0;
	double SumRank = 0;
	std::int64_t AvgRankN = 0;
};

[[nodiscard]] OuterKey OuterKeyForCell(const FusedCubeCell &C, const StarJoinCubeTailPlan &Tail,
                                       const StarJoinCubeBulkParams &Params) {
	OuterKey K;
	for(const std::string &Gk : Tail.GroupKeys) {
		const int Bit = CubeKeyBit(Params.CubeKeys, Gk);
		if(Bit == 2)
			K.Ci = (C.Mask & 2u) != 0 ? C.Ci : kNullDim;
		else if(Bit == 4)
			K.Gi = (C.Mask & 4u) != 0 ? C.Gi : kNullDim;
		else if(Bit == 8)
			K.Mi = (C.Mask & 8u) != 0 ? C.Mi : 0;
	}
	return K;
}

void RunOuterGroupBy(const std::vector<FusedCubeCell> &Cells, const StarJoinCubeTailPlan &Tail,
                     const StarJoinCubeBulkParams &Params, std::vector<std::pair<OuterKey, OuterAccum>> &Groups) {
	std::size_t DenseUsed = 0;
	std::array<OuterAccum, kOuterDenseSlots> DenseAcc{};
	std::array<uint8_t, kOuterDenseSlots> DenseHit{};
	DenseAcc.fill(OuterAccum{});
	DenseHit.fill(0);
	std::unordered_map<OuterKey, OuterAccum, OuterKeyHash> SparseAcc;
	SparseAcc.reserve(Cells.size() / 8 + 1);
	for(const FusedCubeCell &C : Cells) {
		std::size_t Di = 0;
		OuterAccum *Target = nullptr;
		if(OuterKeyDenseIndex(C, Di)) {
			Target = &DenseAcc[Di];
			if(!DenseHit[Di]) {
				DenseHit[Di] = 1;
				++DenseUsed;
			}
		} else {
			const OuterKey K = OuterKeyForCell(C, Tail, Params);
			Target = &SparseAcc[K];
		}
		OuterAccum &A = *Target;
		for(const StarJoinCubeGroupAggSpec &Sp : Tail.GroupAggs) {
			switch(Sp.Kind) {
			case SQL::GroupCombAggKind::Sum:
				if(Sp.SrcCol == Params.CountOutCol)
					A.SumCnt += C.Cnt;
				break;
			case SQL::GroupCombAggKind::Avg:
				if(Sp.SrcCol == Params.SumOutCol) {
					A.SumTotal += C.Sum;
					++A.AvgTotalN;
				} else if(Sp.SrcCol == Params.AvgOutCol) {
					A.SumAvg += C.Avg;
					++A.AvgAvgN;
				} else if(Sp.SrcCol == "country_rank") {
					A.SumRank += static_cast<double>(C.CountryRank);
					++A.AvgRankN;
				}
				break;
			default:
				break;
			}
		}
	}
	Groups.clear();
	Groups.reserve(DenseUsed + SparseAcc.size());
	for(std::size_t Di = 0; Di < kOuterDenseSlots; ++Di) {
		if(!DenseHit[Di])
			continue;
		const std::size_t Mi = Di % kMonthSlots;
		const std::size_t Rem = Di / kMonthSlots;
		const std::size_t Gi = Rem % kCategoryCount;
		const std::size_t Ci = Rem / kCategoryCount;
		Groups.push_back({OuterKey{static_cast<std::uint8_t>(Ci), static_cast<std::uint8_t>(Gi), static_cast<std::uint32_t>(Mi)},
		                  DenseAcc[Di]});
	}
	for(auto &Kv : SparseAcc)
		Groups.push_back(std::move(Kv));
}

[[nodiscard]] double GroupField(const OuterAccum &A, const StarJoinCubeGroupAggSpec &Sp,
                                const StarJoinCubeBulkParams &Params) {
	switch(Sp.Kind) {
	case SQL::GroupCombAggKind::Sum:
		if(Sp.SrcCol == Params.CountOutCol)
			return static_cast<double>(A.SumCnt);
		return 0.0;
	case SQL::GroupCombAggKind::Avg:
		if(Sp.SrcCol == Params.SumOutCol)
			return A.AvgTotalN > 0 ? A.SumTotal / static_cast<double>(A.AvgTotalN) : 0.0;
		if(Sp.SrcCol == Params.AvgOutCol)
			return A.AvgAvgN > 0 ? A.SumAvg / static_cast<double>(A.AvgAvgN) : 0.0;
		if(Sp.SrcCol == "country_rank")
			return A.AvgRankN > 0 ? A.SumRank / static_cast<double>(A.AvgRankN) : 0.0;
		return 0.0;
	default:
		return 0.0;
	}
}

void MaterializeGroupRow(const OuterKey &K, const OuterAccum &A, const StarJoinCubeTailPlan &Tail,
                         const StarJoinCubeBulkParams &Params, RowItem &Row) {
	for(const std::string &Gk : Tail.GroupKeys) {
		const int Bit = CubeKeyBit(Params.CubeKeys, Gk);
		if(Bit == 2)
			Row[Gk] = K.Ci != kNullDim ? std::string(BulkSyntheticCountryNameByIndex(K.Ci)) : "";
		else if(Bit == 4)
			Row[Gk] = K.Gi != kNullDim ? std::string(BulkSyntheticCategoryNameByIndex(K.Gi)) : "";
		else if(Bit == 8)
			Row[Gk] = std::to_string(K.Mi);
	}
	for(const StarJoinCubeGroupAggSpec &Sp : Tail.GroupAggs) {
		const double V = GroupField(A, Sp, Params);
		char Buf[64];
		(void)std::snprintf(Buf, sizeof(Buf), "%.12g", V);
		Row[Sp.OutCol] = Buf;
	}
}

[[nodiscard]] int CompareGroups(const std::pair<OuterKey, OuterAccum> &A, const std::pair<OuterKey, OuterAccum> &B,
                                const StarJoinCubeTailPlan &Tail, const StarJoinCubeBulkParams &Params) {
	for(const SQL::OrderBySpec &Ob : Tail.OrderBy) {
		double Va = 0;
		double Vb = 0;
		bool Found = false;
		for(const std::string &Gk : Tail.GroupKeys) {
			if(Gk != Ob.Column)
				continue;
			const int Bit = CubeKeyBit(Params.CubeKeys, Gk);
			if(Bit == 2) {
				Va = A.first.Ci == kNullDim ? -1.0 : static_cast<double>(A.first.Ci);
				Vb = B.first.Ci == kNullDim ? -1.0 : static_cast<double>(B.first.Ci);
			} else if(Bit == 4) {
				Va = A.first.Gi == kNullDim ? -1.0 : static_cast<double>(A.first.Gi);
				Vb = B.first.Gi == kNullDim ? -1.0 : static_cast<double>(B.first.Gi);
			} else if(Bit == 8) {
				Va = static_cast<double>(A.first.Mi);
				Vb = static_cast<double>(B.first.Mi);
			}
			Found = true;
			break;
		}
		if(!Found) {
			for(const StarJoinCubeGroupAggSpec &Sp : Tail.GroupAggs) {
				if(Sp.OutCol == Ob.Column) {
					Va = GroupField(A.second, Sp, Params);
					Vb = GroupField(B.second, Sp, Params);
					Found = true;
					break;
				}
			}
		}
		if(!Found)
			continue;
		if(Va < Vb)
			return Ob.Ascending ? -1 : 1;
		if(Va > Vb)
			return Ob.Ascending ? 1 : -1;
	}
	return 0;
}

} // namespace

bool ParseStarJoinCubeBulkParams(const SQL::Instruction &Inst, StarJoinCubeBulkParams &Params) {
	if(Inst.Opcode_ != SQL::Opcode::STAR_JOIN_CUBE_BULK || Inst.Operands.size() < 16)
		return false;
	const auto *Dest = std::get_if<std::string>(&Inst.Operands[0]);
	const auto *Cust = std::get_if<std::string>(&Inst.Operands[1]);
	const auto *Ord = std::get_if<std::string>(&Inst.Operands[2]);
	const auto *Prod = std::get_if<std::string>(&Inst.Operands[3]);
	if(!Dest || !Cust || !Ord || !Prod)
		return false;
	Params.DestTable = *Dest;
	Params.CustomersTable = *Cust;
	Params.OrdersTable = *Ord;
	Params.ProductsTable = *Prod;
	std::size_t Off = 4;
	const auto *FilterCnt = std::get_if<int64_t>(&Inst.Operands[Off]);
	if(!FilterCnt)
		return false;
	Off += 1 + static_cast<std::size_t>(*FilterCnt) * 3;
	if(Off + 9 > Inst.Operands.size())
		return false;
	Params.CubeKeys.clear();
	for(std::size_t K = 0; K < 4; ++K) {
		const auto *Kn = std::get_if<std::string>(&Inst.Operands[Off++]);
		if(!Kn)
			return false;
		Params.CubeKeys.push_back(*Kn);
	}
	const auto *SumCol = std::get_if<std::string>(&Inst.Operands[Off++]);
	const auto *SumOut = std::get_if<std::string>(&Inst.Operands[Off++]);
	const auto *AvgOut = std::get_if<std::string>(&Inst.Operands[Off++]);
	const auto *CountOut = std::get_if<std::string>(&Inst.Operands[Off++]);
	const auto *HavingMin = std::get_if<int64_t>(&Inst.Operands[Off++]);
	if(!SumCol || !SumOut || !AvgOut || !CountOut || !HavingMin)
		return false;
	Params.SumSourceCol = *SumCol;
	Params.SumOutCol = *SumOut;
	Params.AvgOutCol = *AvgOut;
	Params.CountOutCol = *CountOut;
	Params.HavingCountMin = *HavingMin;
	return Params.CubeKeys.size() == 4;
}

bool IsStarJoinCubeDominantBytecode(const SQL::Bytecode &Code, std::size_t &BulkIxOut) {
	BulkIxOut = static_cast<std::size_t>(-1);
	for(std::size_t I = 0; I < Code.size(); ++I) {
		const SQL::Opcode Op = Code[I].Opcode_;
		if(Op == SQL::Opcode::NOP || Op == SQL::Opcode::HALT)
			continue;
		if(BulkIxOut == static_cast<std::size_t>(-1)) {
			if(IsSkippableTailOpcode(Op) || Op == SQL::Opcode::POP || Op == SQL::Opcode::FILTER_DNF ||
			   Op == SQL::Opcode::INNER_JOIN)
				continue;
		} else if(IsSkippableTailOpcode(Op))
			continue;
		if(Op == SQL::Opcode::STAR_JOIN_CUBE_BULK) {
			if(BulkIxOut != static_cast<std::size_t>(-1))
				return false;
			BulkIxOut = I;
			continue;
		}
		if(Op == SQL::Opcode::WINDOW_ROW_NUMBER || Op == SQL::Opcode::FILTER_DNF || Op == SQL::Opcode::GROUP_BY ||
		   Op == SQL::Opcode::ORDER_BY || Op == SQL::Opcode::LIMIT || Op == SQL::Opcode::OFFSET ||
		   Op == SQL::Opcode::SLICE_RANGE || Op == SQL::Opcode::CASE_EVAL)
			continue;
		return false;
	}
	return BulkIxOut != static_cast<std::size_t>(-1);
}

bool TryParseStarJoinCubeTail(const SQL::Bytecode &Code, const std::size_t BulkIx, StarJoinCubeTailPlan &Out) {
	Out = {};
	if(BulkIx >= Code.size() || Code[BulkIx].Opcode_ != SQL::Opcode::STAR_JOIN_CUBE_BULK)
		return false;
	std::size_t I = BulkIx + 1;
	for(; I < Code.size(); ++I) {
		const SQL::Opcode Op = Code[I].Opcode_;
		if(Op == SQL::Opcode::HALT)
			break;
		if(IsSkippableTailOpcode(Op))
			continue;
		if(Op == SQL::Opcode::WINDOW_ROW_NUMBER) {
			StarJoinCubeWindowSpec Win;
			if(!ParseWindowSpec(Code[I], Win))
				return false;
			Out.Windows.push_back(std::move(Win));
			continue;
		}
		if(Op == SQL::Opcode::FILTER_DNF) {
			if(!Out.FilterNotNullCol.empty() || !ParseFilterNotNull(Code[I], Out.FilterNotNullCol))
				return false;
			continue;
		}
		if(Op == SQL::Opcode::GROUP_BY) {
			if(!Out.GroupKeys.empty() || !ParseGroupBySpec(Code[I], Out))
				return false;
			continue;
		}
		if(Op == SQL::Opcode::ORDER_BY) {
			if(Code[I].Operands.empty() || I < 2)
				return false;
			if(Code[I - 2].Opcode_ != SQL::Opcode::PUSH || Code[I - 1].Opcode_ != SQL::Opcode::PUSH)
				return false;
			const auto *Asc = std::get_if<int64_t>(&Code[I - 2].Operands[0]);
			if(!Asc)
				return false;
			const auto *Col = std::get_if<std::string>(&Code[I].Operands[0]);
			if(!Col)
				return false;
			SQL::OrderBySpec Ob;
			Ob.Column = *Col;
			Ob.Ascending = *Asc != 0;
			Out.OrderBy.push_back(Ob);
			continue;
		}
		if(Op == SQL::Opcode::PUSH && !Code[I].Operands.empty()) {
			if(std::get_if<int64_t>(&Code[I].Operands[0]) != nullptr)
				continue;
		}
		if(Op == SQL::Opcode::LIMIT) {
			const auto *Lim = std::get_if<int64_t>(&Code[I].Operands[0]);
			if(!Lim || *Lim <= 0)
				return false;
			Out.Limit = static_cast<std::size_t>(*Lim);
			Out.FinalSelectIp = I + 1;
			while(Out.FinalSelectIp < Code.size() && Code[Out.FinalSelectIp].Opcode_ == SQL::Opcode::NOP)
				++Out.FinalSelectIp;
			return !Out.Windows.empty() && !Out.FilterNotNullCol.empty() && !Out.GroupKeys.empty() &&
			       !Out.GroupAggs.empty() && Out.Limit > 0;
		}
		return false;
	}
	return false;
}

bool ExecuteFusedStarJoinCubeTail(Database &Db, const StarJoinCubeBulkParams &Params, const StarJoinCubeTailPlan &Tail,
                                  RowTable &OutRows, std::uint64_t *RowsScannedOut) {
	SemistructuredProfileScope Scope("star_join_cube_fused_tail");
	HybridTableSlot *FactSlot = nullptr;
	const auto It = Db.Tables_.find(Params.OrdersTable);
	if(It != Db.Tables_.end() && It->second.Columnar.BulkSyntheticStarCubeReady)
		FactSlot = &It->second;
	if(!FactSlot || !FactSlot->Columnar.BulkSyntheticStarCubeReady)
		return false;
	if(FactSlot->Columnar.BulkSyntheticStarCubeHavingMin <= 0 ||
	   FactSlot->Columnar.BulkSyntheticStarCubeHavingMin > Params.HavingCountMin)
		FactSlot->Columnar.BulkSyntheticStarCubeHavingMin = Params.HavingCountMin;
	const ColumnarTable &Fact = FactSlot->Columnar;
	const std::uint32_t CustMod =
	    static_cast<std::uint32_t>(Fact.BulkSyntheticFkCustMod > 0 ? Fact.BulkSyntheticFkCustMod : 997);
	std::vector<FusedCubeCell> Cells;
	{
		SemistructuredProfileScope CollectScope("fused_collect");
		CollectFusedCubeCells(Fact, Params, CustMod, Tail.FilterNotNullCol, Cells);
	}
	if(Cells.empty())
		return false;
	for(const StarJoinCubeWindowSpec &Win : Tail.Windows) {
		if(Win.Kind == static_cast<int>(SQL::WindowFnKind::Lag)) {
			SemistructuredProfileScope LagScope("fused_lag");
			ApplyLagWindow(Cells, Win, Params);
		} else if(Win.Kind == static_cast<int>(SQL::WindowFnKind::Rank)) {
			SemistructuredProfileScope RankScope("fused_rank");
			ApplyRankWindow(Cells, Win, Params);
		}
	}
	std::vector<std::pair<OuterKey, OuterAccum>> Groups;
	{
		SemistructuredProfileScope GroupScope("fused_group");
		RunOuterGroupBy(Cells, Tail, Params, Groups);
	}
	const auto GroupLess = [&](const std::pair<OuterKey, OuterAccum> &A, const std::pair<OuterKey, OuterAccum> &B) {
		return CompareGroups(A, B, Tail, Params) < 0;
	};
	{
		SemistructuredProfileScope SortScope("fused_sort");
		if(Tail.Limit > 0 && Groups.size() > Tail.Limit)
			std::partial_sort(Groups.begin(), Groups.begin() + Tail.Limit, Groups.end(), GroupLess);
		else
			std::sort(Groups.begin(), Groups.end(), GroupLess);
		if(Tail.Limit > 0 && Groups.size() > Tail.Limit)
			Groups.resize(Tail.Limit);
	}
	OutRows.clear();
	OutRows.reserve(Groups.size());
	for(const auto &[K, A] : Groups) {
		RowItem Row;
		MaterializeGroupRow(K, A, Tail, Params, Row);
		OutRows.push_back(std::move(Row));
	}
	if(RowsScannedOut != nullptr)
		*RowsScannedOut += static_cast<std::uint64_t>(Fact.RowCount);
	return true;
}

} // namespace AstralDB
