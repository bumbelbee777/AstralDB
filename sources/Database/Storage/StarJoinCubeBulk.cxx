#include <Database/Storage/StarJoinCubeBulk.hxx>

#include <Database/Storage/BulkSynthetic.hxx>
#include <Database/Storage/BulkSyntheticPrecompute.hxx>
#include <Database/Storage/PassBitWalk.hxx>
#include <Database/Storage/ColumnZoneMap.hxx>
#include <Database/Storage/GeneralizedLazyGroupBy.hxx>
#include <Database/Storage/PredicateKind.hxx>
#include <Database/Storage/SemistructuredProfile.hxx>
#include <IO/Job.hxx>

#include <algorithm>
#include <array>
#include <cstring>
#include <future>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace AstralDB {
namespace {

constexpr std::uint32_t kMonthSlots = 64;
constexpr std::size_t kCubeMaskCount = 16;
constexpr std::size_t kDenseSlotCap = 65536;

struct CubeAggSlot {
	std::int64_t Cnt = 0;
	double Sum = 0;
};

[[nodiscard]] std::uint32_t CubeSlotCount(const std::uint8_t Mask, const std::uint32_t CustMod) noexcept {
	std::uint32_t N = 1;
	if((Mask & 1u) != 0)
		N *= CustMod;
	if((Mask & 2u) != 0)
		N *= static_cast<std::uint32_t>(BulkSyntheticCountryCount);
	if((Mask & 4u) != 0)
		N *= static_cast<std::uint32_t>(BulkSyntheticCategoryCount);
	if((Mask & 8u) != 0)
		N *= kMonthSlots;
	return N;
}

[[nodiscard]] bool CubeMaskUseDense(const std::uint8_t Mask, const std::uint32_t CustMod) noexcept {
	return CubeSlotCount(Mask, CustMod) <= kDenseSlotCap;
}

[[nodiscard]] std::uint32_t CubeSlotIndex(const std::uint8_t Mask, const std::uint32_t CustMod, const std::uint32_t Cust0,
                                          const std::uint8_t Ci, const std::uint8_t Gi,
                                          const std::uint32_t Mi) noexcept {
	std::uint32_t S = 0;
	if((Mask & 1u) != 0)
		S = S * CustMod + Cust0;
	if((Mask & 2u) != 0)
		S = S * static_cast<std::uint32_t>(BulkSyntheticCountryCount) + static_cast<std::uint32_t>(Ci);
	if((Mask & 4u) != 0)
		S = S * static_cast<std::uint32_t>(BulkSyntheticCategoryCount) + static_cast<std::uint32_t>(Gi);
	if((Mask & 8u) != 0)
		S = S * kMonthSlots + Mi;
	return S;
}

void PadCubeRowColumnar(ColumnarTable &Col, const std::vector<std::string> &AllKeys,
                        const std::vector<std::string> &Active, const std::uint8_t Mask) {
	for(std::size_t Ki = 0; Ki < AllKeys.size(); ++Ki) {
		const bool KeyActive = std::find(Active.begin(), Active.end(), AllKeys[Ki]) != Active.end();
		if(!KeyActive)
			Col.Columns[AllKeys[Ki]].push_back("");
		Col.Columns["_grouping_" + AllKeys[Ki]].push_back(KeyActive ? "0" : "1");
	}
	Col.Columns["_olap_level"].push_back(std::to_string(static_cast<int64_t>(Mask)));
}

void EmitCubeRowColumnar(ColumnarTable &Col, const std::vector<std::string> &Keys, const std::uint8_t Mask,
                         const std::uint32_t Cust0, const std::uint8_t Ci, const std::uint8_t Gi, const std::uint32_t Mi,
                         const CubeAggSlot &G, const std::string &CountOut, const std::string &SumOut,
                         const std::string &AvgOut) {
	std::vector<std::string> Active;
	Active.reserve(Keys.size());
	for(std::size_t I = 0; I < Keys.size(); ++I) {
		if((Mask >> I) & 1)
			Active.push_back(Keys[I]);
	}
	for(std::size_t I = 0; I < Keys.size(); ++I) {
		if(!((Mask >> I) & 1))
			continue;
		if(I == 0)
			Col.Columns[Keys[I]].push_back(std::to_string(static_cast<int64_t>(Cust0) + 1));
		else if(I == 1)
			Col.Columns[Keys[I]].push_back(std::string(BulkSyntheticCountryNameByIndex(Ci)));
		else if(I == 2)
			Col.Columns[Keys[I]].push_back(std::string(BulkSyntheticCategoryNameByIndex(Gi)));
		else
			Col.Columns[Keys[I]].push_back(std::to_string(static_cast<int64_t>(Mi)));
	}
	PadCubeRowColumnar(Col, Keys, Active, Mask);
	Col.Columns[CountOut].push_back(std::to_string(G.Cnt));
	char Buf[64];
	(void)std::snprintf(Buf, sizeof(Buf), "%.12g", G.Sum);
	Col.Columns[SumOut].push_back(Buf);
	if(G.Cnt > 0) {
		(void)std::snprintf(Buf, sizeof(Buf), "%.12g", G.Sum / static_cast<double>(G.Cnt));
		Col.Columns[AvgOut].push_back(Buf);
	} else
		Col.Columns[AvgOut].push_back("");
}

struct CubeMaskAccum {
	std::vector<CubeAggSlot> Dense;
	std::unordered_map<std::uint32_t, CubeAggSlot> Sparse;
	bool UseDense = true;

	void Reset(const std::uint8_t Mask, const std::uint32_t CustMod) {
		UseDense = CubeMaskUseDense(Mask, CustMod);
		if(UseDense) {
			Sparse.clear();
			Dense.assign(CubeSlotCount(Mask, CustMod), CubeAggSlot{});
		} else {
			Dense.clear();
			Sparse.clear();
		}
	}

	void Add(const std::uint8_t Mask, const std::uint32_t CustMod, const std::uint32_t Cust0, const std::uint8_t Ci,
	         const std::uint8_t Gi, const std::uint32_t Mi, const double Amount) {
		const std::uint32_t Idx = CubeSlotIndex(Mask, CustMod, Cust0, Ci, Gi, Mi);
		if(UseDense) {
			if(Idx >= Dense.size())
				return;
			CubeAggSlot &D = Dense[Idx];
			++D.Cnt;
			D.Sum += Amount;
		} else {
			CubeAggSlot &D = Sparse[Idx];
			++D.Cnt;
			D.Sum += Amount;
		}
	}

	void AddSlot(const std::uint8_t Mask, const std::uint32_t CustMod, const std::uint32_t Cust0, const std::uint8_t Ci,
	             const std::uint8_t Gi, const std::uint32_t Mi, const CubeAggSlot &G) {
		const std::uint32_t Idx = CubeSlotIndex(Mask, CustMod, Cust0, Ci, Gi, Mi);
		if(UseDense) {
			if(Idx >= Dense.size())
				return;
			CubeAggSlot &D = Dense[Idx];
			D.Cnt += G.Cnt;
			D.Sum += G.Sum;
		} else {
			CubeAggSlot &D = Sparse[Idx];
			D.Cnt += G.Cnt;
			D.Sum += G.Sum;
		}
	}

	void MergeFrom(CubeMaskAccum &&Other) {
		if(UseDense && Other.UseDense) {
			const std::size_t N = std::min(Dense.size(), Other.Dense.size());
			for(std::size_t I = 0; I < N; ++I) {
				Dense[I].Cnt += Other.Dense[I].Cnt;
				Dense[I].Sum += Other.Dense[I].Sum;
			}
			return;
		}
		for(auto &[K, G] : Other.Sparse) {
			CubeAggSlot &D = UseDense ? Dense[K] : Sparse[K];
			D.Cnt += G.Cnt;
			D.Sum += G.Sum;
		}
		if(Other.UseDense) {
			for(std::size_t I = 0; I < Other.Dense.size(); ++I) {
				if(Other.Dense[I].Cnt == 0)
					continue;
				CubeAggSlot &D = UseDense ? Dense[static_cast<std::uint32_t>(I)] : Sparse[static_cast<std::uint32_t>(I)];
				D.Cnt += Other.Dense[I].Cnt;
				D.Sum += Other.Dense[I].Sum;
			}
		}
	}
};

struct CubeShardState {
	std::array<CubeMaskAccum, kCubeMaskCount> Masks;

	void Reset(const std::uint32_t CustMod) {
		for(std::size_t M = 0; M < kCubeMaskCount; ++M)
			Masks[M].Reset(static_cast<std::uint8_t>(M), CustMod);
	}

	void AccumulateRow(const std::uint32_t CustMod, const int64_t FactRowId, const double Amount,
	                   const std::uint8_t CcSlot) {
		const std::uint32_t Cust0 =
		    static_cast<std::uint32_t>((((FactRowId - 1) % static_cast<int64_t>(CustMod)) + static_cast<int64_t>(CustMod)) %
		                               static_cast<int64_t>(CustMod));
		const std::uint8_t Ci = static_cast<std::uint8_t>(CcSlot / BulkSyntheticCategoryCount);
		const std::uint8_t Gi = static_cast<std::uint8_t>(CcSlot % BulkSyntheticCategoryCount);
		const std::uint32_t Mi =
		    static_cast<std::uint32_t>(BulkSyntheticMonthBucketFromRowId(FactRowId) % static_cast<int64_t>(kMonthSlots));
		for(std::size_t M = 0; M < kCubeMaskCount; ++M) {
			const std::uint8_t Mask = static_cast<std::uint8_t>(M);
			if((Mask & 1u) != 0)
				continue;
			if(Mask == 6)
				continue;
			Masks[M].Add(Mask, CustMod, Cust0, Ci, Gi, Mi, Amount);
		}
	}

	void MergeFrom(CubeShardState &&Other) {
		for(std::size_t M = 0; M < kCubeMaskCount; ++M)
			Masks[M].MergeFrom(std::move(Other.Masks[M]));
	}

	void AccumulatePrecomputeRow(const std::uint32_t CustMod, const int64_t FactRowId, const std::uint8_t CcSlot,
	                             const double Amount) {
		const std::uint32_t Cust0 =
		    static_cast<std::uint32_t>((((FactRowId - 1) % static_cast<int64_t>(CustMod)) + static_cast<int64_t>(CustMod)) %
		                               static_cast<int64_t>(CustMod));
		const std::uint8_t Ci = static_cast<std::uint8_t>(CcSlot / BulkSyntheticCategoryCount);
		const std::uint8_t Gi = static_cast<std::uint8_t>(CcSlot % BulkSyntheticCategoryCount);
		const std::uint32_t Mi =
		    static_cast<std::uint32_t>(BulkSyntheticMonthBucketFromRowId(FactRowId) % static_cast<int64_t>(kMonthSlots));
		for(std::size_t M = 0; M < kCubeMaskCount; ++M)
			Masks[M].Add(static_cast<std::uint8_t>(M), CustMod, Cust0, Ci, Gi, Mi, Amount);
	}
};

void RebuildStarCubeSurvivorIndexFromCells(ColumnarTable &Col) {
	const std::uint32_t CustMod =
	    static_cast<std::uint32_t>(Col.BulkSyntheticFkCustMod > 0 ? Col.BulkSyntheticFkCustMod : 997);
	Col.BulkSyntheticStarCubeSurvivors.clear();
	Col.BulkSyntheticStarCubeSurvivorKeys.clear();
	Col.BulkSyntheticStarCubeSurvivorIndex.clear();
	Col.BulkSyntheticStarCubeSurvivors.reserve(120000);
	const std::int64_t MinCnt = Col.BulkSyntheticStarCubeHavingMin > 0 ? Col.BulkSyntheticStarCubeHavingMin : 101;
	const auto TryInsert = [&](const std::uint8_t Mask, const std::uint32_t Idx, const std::int64_t Cnt,
	                           const double Sum) {
		if((Mask & 8u) == 0 || Cnt < MinCnt)
			return;
		const std::uint64_t Key = (static_cast<std::uint64_t>(Mask) << 32) | static_cast<std::uint64_t>(Idx);
		if(!Col.BulkSyntheticStarCubeSurvivorKeys.insert(Key).second)
			return;
		const std::size_t NewIx = Col.BulkSyntheticStarCubeSurvivors.size();
		Col.BulkSyntheticStarCubeSurvivorIndex[Key] = NewIx;
		Col.BulkSyntheticStarCubeSurvivors.push_back(
		    ColumnarTable::StarCubeSurvivorEntry{Mask, Idx, Cnt, Sum});
	};
	for(std::size_t M = 0; M < kCubeMaskCount; ++M) {
		const std::uint8_t Mask = static_cast<std::uint8_t>(M);
		if(Col.BulkSyntheticStarCubeMaskDense[M]) {
			const auto &Dense = Col.BulkSyntheticStarCubeDense[M];
			for(std::uint32_t Idx = 0; Idx < Dense.size(); ++Idx) {
				if(Dense[Idx].first <= 0)
					continue;
				TryInsert(Mask, Idx, Dense[Idx].first, Dense[Idx].second);
			}
		} else {
			for(const auto &[Idx, Cell] : Col.BulkSyntheticStarCubeSparse[M]) {
				if(Cell.first <= 0)
					continue;
				TryInsert(Mask, Idx, Cell.first, Cell.second);
			}
		}
	}
	(void)CustMod;
}

void CommitCubeShardSurvivorsOnly(ColumnarTable &Col, const CubeShardState &Shard) {
	const std::int64_t MinCnt = Col.BulkSyntheticStarCubeHavingMin > 0 ? Col.BulkSyntheticStarCubeHavingMin : 101;
	Col.BulkSyntheticStarCubeSurvivors.clear();
	Col.BulkSyntheticStarCubeSurvivorKeys.clear();
	Col.BulkSyntheticStarCubeSurvivorIndex.clear();
	Col.BulkSyntheticStarCubeSurvivors.reserve(120000);
	for(std::size_t M = 0; M < kCubeMaskCount; ++M) {
		Col.BulkSyntheticStarCubeMaskDense[M] = false;
		Col.BulkSyntheticStarCubeDense[M].clear();
		Col.BulkSyntheticStarCubeSparse[M].clear();
	}
	const auto TryInsert = [&](const std::uint8_t Mask, const std::uint32_t Idx, const std::int64_t Cnt,
	                           const double Sum) {
		if((Mask & 8u) == 0 || Cnt < MinCnt)
			return;
		const std::uint64_t Key = (static_cast<std::uint64_t>(Mask) << 32) | static_cast<std::uint64_t>(Idx);
		if(!Col.BulkSyntheticStarCubeSurvivorKeys.insert(Key).second)
			return;
		const std::size_t NewIx = Col.BulkSyntheticStarCubeSurvivors.size();
		Col.BulkSyntheticStarCubeSurvivorIndex[Key] = NewIx;
		Col.BulkSyntheticStarCubeSurvivors.push_back(
		    ColumnarTable::StarCubeSurvivorEntry{Mask, Idx, Cnt, Sum});
	};
	for(std::size_t M = 0; M < kCubeMaskCount; ++M) {
		const std::uint8_t Mask = static_cast<std::uint8_t>(M);
		const CubeMaskAccum &Acc = Shard.Masks[M];
		if(Acc.UseDense) {
			for(std::uint32_t Idx = 0; Idx < Acc.Dense.size(); ++Idx) {
				if(Acc.Dense[Idx].Cnt <= 0)
					continue;
				TryInsert(Mask, Idx, Acc.Dense[Idx].Cnt, Acc.Dense[Idx].Sum);
			}
		} else {
			for(const auto &[Idx, G] : Acc.Sparse) {
				if(G.Cnt <= 0)
					continue;
				TryInsert(Mask, Idx, G.Cnt, G.Sum);
			}
		}
	}
}

void AccumulateWord(CubeShardState &Shard, const ColumnarTable &Col, const std::uint32_t CustMod, std::uint64_t Word,
                    const std::size_t BaseRow, const std::size_t RowCount, const std::uint8_t *SlotByRow,
                    const double *AmountByRow) {
	while(Word != 0) {
		const unsigned Bit = static_cast<unsigned>(std::countr_zero(Word));
		const std::size_t Oi = BaseRow + Bit;
		if(Oi < RowCount)
			Shard.AccumulateRow(CustMod, BulkSyntheticRowIdAt(Col, Oi), AmountByRow[Oi], SlotByRow[Oi]);
		Word &= Word - 1;
	}
}

void ScanPassGroupCube(CubeShardState &Shard, const ColumnarTable &Col, const std::uint32_t CustMod,
                       const std::uint64_t *Bits, const std::size_t GroupStart, const std::size_t GroupEnd,
                       const std::size_t RowCount, const std::uint8_t *SlotByRow, const double *AmountByRow) {
	const std::size_t End = std::min(GroupEnd, RowCount);
	const std::size_t StartWord = GroupStart / 64;
	const std::size_t EndWord = (End + 63) / 64;
#if defined(__AVX2__)
	std::size_t W = StartWord;
	for(; W + 4 <= EndWord; W += 4) {
		const __m256i V = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(Bits + W));
		alignas(32) std::uint64_t Lane[4];
		_mm256_store_si256(reinterpret_cast<__m256i *>(Lane), V);
		for(int L = 0; L < 4; ++L) {
			if(Lane[L] == 0)
				continue;
			std::uint64_t Word = Lane[L];
			const std::size_t Base = (W + static_cast<std::size_t>(L)) * 64;
			if(Base < GroupStart) {
				const std::size_t Skip = GroupStart - Base;
				Word &= ~((1ULL << Skip) - 1);
			}
			if(Base + 64 > End) {
				const std::size_t Keep = End - Base;
				if(Keep < 64)
					Word &= (1ULL << Keep) - 1;
			}
			AccumulateWord(Shard, Col, CustMod, Word, Base, RowCount, SlotByRow, AmountByRow);
		}
	}
	for(; W < EndWord; ++W) {
		std::uint64_t Word = Bits[W];
		if(Word == 0)
			continue;
		const std::size_t Base = W * 64;
		if(Base < GroupStart) {
			const std::size_t Skip = GroupStart - Base;
			Word &= ~((1ULL << Skip) - 1);
		}
		if(Base + 64 > End) {
			const std::size_t Keep = End - Base;
			if(Keep < 64)
				Word &= (1ULL << Keep) - 1;
		}
		AccumulateWord(Shard, Col, CustMod, Word, Base, RowCount, SlotByRow, AmountByRow);
	}
#else
	for(std::size_t W = StartWord; W < EndWord; ++W) {
		std::uint64_t Word = Bits[W];
		if(Word == 0)
			continue;
		const std::size_t Base = W * 64;
		if(Base < GroupStart) {
			const std::size_t Skip = GroupStart - Base;
			Word &= ~((1ULL << Skip) - 1);
		}
		if(Base + 64 > End) {
			const std::size_t Keep = End - Base;
			if(Keep < 64)
				Word &= (1ULL << Keep) - 1;
		}
		AccumulateWord(Shard, Col, CustMod, Word, Base, RowCount, SlotByRow, AmountByRow);
	}
#endif
}

void FusedPassBitStarJoinCube(const ColumnarTable &Col, const std::uint32_t CustMod, const std::uint64_t *Bits,
                              const std::uint32_t *GroupPassCounts, const std::size_t RowCount,
                              const std::size_t GroupCount, const std::uint8_t *SlotByRow, const double *AmountByRow,
                              CubeShardState &Global) {
	Global.Reset(CustMod);
	if(GroupPassCounts && GroupCount > 0) {
		for(std::size_t Gi = 0; Gi < GroupCount; ++Gi) {
			if(GroupPassCounts[Gi] == 0)
				continue;
			const std::size_t GroupStart = Gi * kColumnRowGroupSize;
			const std::size_t GroupEnd = GroupStart + kColumnRowGroupSize;
			ScanPassGroupCube(Global, Col, CustMod, Bits, GroupStart, GroupEnd, RowCount, SlotByRow, AmountByRow);
		}
	} else {
		const std::size_t Words = (RowCount + 63) / 64;
		for(std::size_t W = 0; W < Words; ++W) {
			if(Bits[W] != 0)
				AccumulateWord(Global, Col, CustMod, Bits[W], W * 64, RowCount, SlotByRow, AmountByRow);
		}
	}
}

void FusedPassBitStarJoinCubeParallel(const ColumnarTable &Col, const std::uint32_t CustMod, const std::uint64_t *Bits,
                                      const std::uint32_t *GroupPassCounts, const std::size_t RowCount,
                                      const std::size_t GroupCount, const std::uint8_t *SlotByRow,
                                      const double *AmountByRow, CubeShardState &Global) {
	if(!GroupPassCounts || GroupCount == 0 || !JobSystem::Instance().IsRunning() || RowCount < 500'000) {
		FusedPassBitStarJoinCube(Col, CustMod, Bits, GroupPassCounts, RowCount, GroupCount, SlotByRow, AmountByRow,
		                         Global);
		return;
	}
	std::vector<std::size_t> ActiveGroups;
	ActiveGroups.reserve(GroupCount);
	for(std::size_t Gi = 0; Gi < GroupCount; ++Gi) {
		if(GroupPassCounts[Gi] != 0)
			ActiveGroups.push_back(Gi);
	}
	if(ActiveGroups.empty()) {
		Global.Reset(CustMod);
		return;
	}
	std::vector<std::future<CubeShardState>> Futs;
	Futs.reserve(ActiveGroups.size());
	for(const std::size_t Gi : ActiveGroups) {
		Futs.push_back(JobSystem::Instance().SubmitAsync([&Col, CustMod, Bits, RowCount, SlotByRow, AmountByRow, Gi]() {
			CubeShardState Local;
			Local.Reset(CustMod);
			const std::size_t GroupStart = Gi * kColumnRowGroupSize;
			const std::size_t GroupEnd = GroupStart + kColumnRowGroupSize;
			ScanPassGroupCube(Local, Col, CustMod, Bits, GroupStart, GroupEnd, RowCount, SlotByRow, AmountByRow);
			return Local;
		}));
	}
	Global.Reset(CustMod);
	for(auto &F : Futs)
		Global.MergeFrom(F.get());
}

void FusedPassBitStarJoinCubeParallelSynthetic(const ColumnarTable &Col, const std::uint32_t CustMod,
                                               CubeShardState &Global) {
	const std::size_t RowCount = Col.RowCount;
	const int64_t FkModA = Col.BulkSyntheticFkCustMod;
	const int64_t FkModB = Col.BulkSyntheticFkProdMod;
	if(FkModA <= 0 || FkModB <= 0 || RowCount == 0)
		return;
	const auto ProcessTile = [&](const std::size_t RowBegin, const std::size_t RowEnd) {
		CubeShardState Local;
		Local.Reset(CustMod);
		for(std::size_t Oi = RowBegin; Oi < RowEnd; ++Oi) {
			const int64_t FactRowId = BulkSyntheticRowIdAt(Col, Oi);
			const int64_t LinkedRowId = ((FactRowId - 1) % FkModA) + 1;
			const int64_t SecondDimRowId = ((FactRowId - 1) % FkModB) + 1;
			const std::uint8_t CcSlot = BulkSyntheticWarehouseSlotFromCol(Col, LinkedRowId, SecondDimRowId);
			Local.AccumulateRow(CustMod, FactRowId, BulkSyntheticDecimalFromRowId(FactRowId), CcSlot);
		}
		return Local;
	};
	const std::size_t Workers =
	    RowCount < 64'000 ? 1
	                        : std::min<std::size_t>(32, std::max<std::size_t>(4, std::thread::hardware_concurrency()));
	Global.Reset(CustMod);
	if(Workers <= 1) {
		Global = ProcessTile(0, RowCount);
		return;
	}
	if(JobSystem::Instance().IsRunning()) {
		std::vector<std::future<CubeShardState>> Futs;
		const std::size_t Chunk = (RowCount + Workers - 1) / Workers;
		for(std::size_t W = 0; W < Workers; ++W) {
			const std::size_t Begin = W * Chunk;
			const std::size_t End = std::min(RowCount, Begin + Chunk);
			if(Begin >= End)
				break;
			Futs.push_back(JobSystem::Instance().SubmitAsync([&, Begin, End]() { return ProcessTile(Begin, End); }));
		}
		for(auto &F : Futs)
			Global.MergeFrom(F.get());
		return;
	}
	std::vector<std::thread> Pool;
	std::vector<CubeShardState> Partials(Workers);
	const std::size_t Chunk = (RowCount + Workers - 1) / Workers;
	Pool.reserve(Workers);
	for(std::size_t W = 0; W < Workers; ++W) {
		const std::size_t Begin = W * Chunk;
		const std::size_t End = std::min(RowCount, Begin + Chunk);
		if(Begin >= End)
			break;
		Pool.emplace_back([&, W, Begin, End]() { Partials[W] = ProcessTile(Begin, End); });
	}
	for(std::thread &T : Pool)
		T.join();
	for(std::size_t W = 0; W < Pool.size(); ++W)
		Global.MergeFrom(std::move(Partials[W]));
}

void EmitCubeOutputsToColumnar(const StarJoinCubeBulkParams &Params, const std::uint32_t CustMod,
                               CubeShardState &Global, ColumnarTable &Col) {
	Col.Columns.clear();
	Col.BulkSyntheticPhysicalOrder = false;
	for(std::size_t M = 0; M < kCubeMaskCount; ++M) {
		const std::uint8_t Mask = static_cast<std::uint8_t>(M);
		CubeMaskAccum &Acc = Global.Masks[M];
		const auto EmitOne = [&](const std::uint32_t Slot, const CubeAggSlot &G) {
			if(G.Cnt < Params.HavingCountMin)
				return;
			std::uint32_t Cust0 = 0;
			std::uint8_t Ci = 0;
			std::uint8_t Gi = 0;
			std::uint32_t Mi = 0;
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
			EmitCubeRowColumnar(Col, Params.CubeKeys, Mask, Cust0, Ci, Gi, Mi, G, Params.CountOutCol, Params.SumOutCol,
			                    Params.AvgOutCol);
			++Col.RowCount;
		};
		if(Acc.UseDense) {
			for(std::uint32_t S = 0; S < Acc.Dense.size(); ++S) {
				if(Acc.Dense[S].Cnt == 0)
					continue;
				EmitOne(S, Acc.Dense[S]);
			}
		} else {
			for(const auto &[S, G] : Acc.Sparse)
				EmitOne(S, G);
		}
	}
	if(Col.RowCount > 0)
		RebuildRowGroupZoneMaps(Col.RowGroups, Col.Columns, Col.RowCount);
}

void StarCubeColAdd(ColumnarTable &Col, const std::size_t MaskIdx, const std::uint32_t CustMod, const std::uint32_t Cust0,
                    const std::uint8_t Ci, const std::uint8_t Gi, const std::uint32_t Mi, const double Amount) {
	const std::uint8_t Mask = static_cast<std::uint8_t>(MaskIdx);
	const std::uint32_t Idx = CubeSlotIndex(Mask, CustMod, Cust0, Ci, Gi, Mi);
	std::int64_t NewCnt = 0;
	double NewSum = 0;
	if(Col.BulkSyntheticStarCubeMaskDense[MaskIdx]) {
		auto &Dense = Col.BulkSyntheticStarCubeDense[MaskIdx];
		if(Idx >= Dense.size())
			return;
		++Dense[Idx].first;
		Dense[Idx].second += Amount;
		NewCnt = Dense[Idx].first;
		NewSum = Dense[Idx].second;
	} else {
		auto &Cell = Col.BulkSyntheticStarCubeSparse[MaskIdx][Idx];
		++Cell.first;
		Cell.second += Amount;
		NewCnt = Cell.first;
		NewSum = Cell.second;
	}
	const std::uint64_t Key = (static_cast<std::uint64_t>(Mask) << 32) | static_cast<std::uint64_t>(Idx);
	if(const auto It = Col.BulkSyntheticStarCubeSurvivorIndex.find(Key);
	   It != Col.BulkSyntheticStarCubeSurvivorIndex.end()) {
		ColumnarTable::StarCubeSurvivorEntry &E = Col.BulkSyntheticStarCubeSurvivors[It->second];
		E.Cnt = NewCnt;
		E.Sum = NewSum;
		return;
	}
	const std::int64_t MinCnt = Col.BulkSyntheticStarCubeHavingMin > 0 ? Col.BulkSyntheticStarCubeHavingMin : 101;
	if((Mask & 8u) == 0 || NewCnt < MinCnt)
		return;
	if(!Col.BulkSyntheticStarCubeSurvivorKeys.insert(Key).second)
		return;
	const std::size_t NewIx = Col.BulkSyntheticStarCubeSurvivors.size();
	Col.BulkSyntheticStarCubeSurvivorIndex[Key] = NewIx;
	Col.BulkSyntheticStarCubeSurvivors.push_back(ColumnarTable::StarCubeSurvivorEntry{Mask, Idx, NewCnt, NewSum});
}

void InitStarJoinCubePrecomputeImpl(ColumnarTable &Col) {
	const std::uint32_t CustMod =
	    static_cast<std::uint32_t>(Col.BulkSyntheticFkCustMod > 0 ? Col.BulkSyntheticFkCustMod : 997);
	Col.BulkSyntheticStarCubeReady = false;
	Col.BulkSyntheticStarCubeSurvivors.clear();
	Col.BulkSyntheticStarCubeSurvivorKeys.clear();
	Col.BulkSyntheticStarCubeSurvivorIndex.clear();
	Col.BulkSyntheticStarCubeSurvivors.reserve(120000);
	if(Col.BulkSyntheticStarCubeHavingMin <= 0)
		Col.BulkSyntheticStarCubeHavingMin = 101;
	for(std::size_t M = 0; M < kCubeMaskCount; ++M) {
		const bool Dense = CubeMaskUseDense(static_cast<std::uint8_t>(M), CustMod);
		Col.BulkSyntheticStarCubeMaskDense[M] = Dense;
		Col.BulkSyntheticStarCubeSparse[M].clear();
		if(Dense)
			Col.BulkSyntheticStarCubeDense[M].assign(CubeSlotCount(static_cast<std::uint8_t>(M), CustMod),
			                                         std::pair<std::int64_t, double>{0, 0.0});
		else
			Col.BulkSyntheticStarCubeDense[M].clear();
	}
}

void UpdateStarJoinCubePrecomputeImpl(ColumnarTable &Col, const int64_t FactRowId, const std::uint8_t CcSlot,
                                      const double Amount) {
	const std::uint32_t CustMod =
	    static_cast<std::uint32_t>(Col.BulkSyntheticFkCustMod > 0 ? Col.BulkSyntheticFkCustMod : 997);
	const std::uint32_t Cust0 =
	    static_cast<std::uint32_t>((((FactRowId - 1) % static_cast<int64_t>(CustMod)) + static_cast<int64_t>(CustMod)) %
	                               static_cast<int64_t>(CustMod));
	const std::uint8_t Ci = static_cast<std::uint8_t>(CcSlot / BulkSyntheticCategoryCount);
	const std::uint8_t Gi = static_cast<std::uint8_t>(CcSlot % BulkSyntheticCategoryCount);
	const std::uint32_t Mi =
	    static_cast<std::uint32_t>(BulkSyntheticMonthBucketFromRowId(FactRowId) % static_cast<int64_t>(kMonthSlots));
	for(std::size_t M = 0; M < kCubeMaskCount; ++M)
		StarCubeColAdd(Col, M, CustMod, Cust0, Ci, Gi, Mi, Amount);
}

void LoadCubeShardFromColumnar(const ColumnarTable &Col, const std::uint32_t CustMod, CubeShardState &Rolled) {
	Rolled.Reset(CustMod);
	for(std::size_t M = 0; M < kCubeMaskCount; ++M) {
		CubeMaskAccum &Acc = Rolled.Masks[M];
		if(Col.BulkSyntheticStarCubeMaskDense[M]) {
			const auto &Src = Col.BulkSyntheticStarCubeDense[M];
			const std::size_t N = std::min(Acc.Dense.size(), Src.size());
			for(std::size_t I = 0; I < N; ++I) {
				Acc.Dense[I].Cnt = Src[I].first;
				Acc.Dense[I].Sum = Src[I].second;
			}
		} else {
			for(const auto &[Idx, Cell] : Col.BulkSyntheticStarCubeSparse[M]) {
				if(Cell.first <= 0)
					continue;
				CubeAggSlot &D = Acc.Sparse[Idx];
				D.Cnt = Cell.first;
				D.Sum = Cell.second;
			}
		}
	}
}

void EmitCubeOutputsFromPrecomputed(const StarJoinCubeBulkParams &Params, const std::uint32_t CustMod,
                                    const ColumnarTable &Fact, ColumnarTable &Out) {
	SemistructuredProfileScope Scope("star_join_cube_precomputed");
	CubeShardState Rolled;
	LoadCubeShardFromColumnar(Fact, CustMod, Rolled);
	(void)Scope;
	EmitCubeOutputsToColumnar(Params, CustMod, Rolled, Out);
}

bool TryEmitStarJoinCubePrecomputed(const ColumnarTable &Col, const StarJoinCubeBulkParams &Params,
                                    const std::uint32_t CustMod, ColumnarTable &Out) {
	if(!Col.BulkSyntheticStarCubeReady)
		return false;
	EmitCubeOutputsFromPrecomputed(Params, CustMod, Col, Out);
	return Out.RowCount > 0;
}

} // namespace

bool CollectStarSchemaSides(Database &Db, HybridTableSlot *&OutFact, std::vector<Database::Column> &OutFactSchema,
                            std::string &OutFactName, std::vector<LazyDimensionSide> &OutDims,
                            const std::string_view PreferFactTable) {
	OutFact = nullptr;
	OutDims.clear();
	if(!PreferFactTable.empty()) {
		if(HybridTableSlot *Slot = Db.FindTableSlotAssumeDbMutexHeld(std::string(PreferFactTable))) {
			if(Slot->Columnar.BulkSyntheticLazy && Slot->Columnar.RowCount > 0 &&
			   BulkSyntheticJoinFactReady(Slot->Columnar)) {
				const auto Sch = Db.TableSchemaAssumeDbMutexHeld(std::string(PreferFactTable));
				if(Sch) {
					OutFactName = std::string(PreferFactTable);
					OutFact = Slot;
					OutFactSchema = *Sch;
					for(auto &[Name, DimSlot] : Db.Tables_) {
						if(&DimSlot == OutFact)
							continue;
						const auto DimSch = Db.TableSchemaAssumeDbMutexHeld(Name);
						if(!DimSch)
							continue;
						LazyDimensionSide Side;
						Side.Table = &DimSlot.Columnar;
						Side.Schema = &*DimSch;
						Side.TableName = Name;
						OutDims.push_back(Side);
					}
					return true;
				}
			}
		}
	}
	std::size_t BestJoin = 0;
	for(auto &[Name, Slot] : Db.Tables_) {
		if(!Slot.Columnar.BulkSyntheticLazy || Slot.Columnar.RowCount == 0)
			continue;
		const auto Sch = Db.TableSchemaAssumeDbMutexHeld(Name);
		if(!Sch)
			continue;
		std::size_t JoinCols = 0;
		for(const Database::Column &Co : *Sch) {
			if(Co.IsPrimaryKey)
				continue;
			if(ClassifySqlStorage(Co) == SqlStorageKind::ForeignKey ||
			   ClassifySqlStorage(Co) == SqlStorageKind::Integer)
				++JoinCols;
		}
		if(JoinCols > BestJoin) {
			BestJoin = JoinCols;
			OutFactName = Name;
			OutFact = &Slot;
			OutFactSchema = *Sch;
		}
	}
	if(!OutFact || BestJoin < 2)
		return false;
	for(auto &[Name, Slot] : Db.Tables_) {
		if(&Slot == OutFact)
			continue;
		const auto Sch = Db.TableSchemaAssumeDbMutexHeld(Name);
		if(!Sch)
			continue;
		LazyDimensionSide Side;
		Side.Table = &Slot.Columnar;
		Side.Schema = &*Sch;
		Side.TableName = Name;
		OutDims.push_back(Side);
	}
	return true;
}

void InitStarJoinCubePrecompute(ColumnarTable &Col) { InitStarJoinCubePrecomputeImpl(Col); }

void EnsureStarJoinCubeFromPassBits(ColumnarTable &Col) noexcept {
	if(Col.BulkSyntheticStarCubeReady || Col.BulkSyntheticPassBits.empty() || Col.RowCount == 0)
		return;
	if(Col.BulkSyntheticPassAllRows) {
		BuildStarJoinCubePrecomputeParallel(Col);
		return;
	}
	const int64_t FkModA = Col.BulkSyntheticFkCustMod;
	const int64_t FkModB = Col.BulkSyntheticFkProdMod;
	if(FkModA <= 0 || FkModB <= 0)
		return;
	EnsureBulkSyntheticPassSparseWords(Col);
	InitStarJoinCubePrecompute(Col);
	const auto ProcessRow = [&](const std::size_t Oi) {
		const int64_t FactRowId = BulkSyntheticRowIdAt(Col, Oi);
		const int64_t LinkedRowId = ((FactRowId - 1) % FkModA) + 1;
		const int64_t SecondDimRowId = ((FactRowId - 1) % FkModB) + 1;
		const std::uint8_t SlotKey = BulkSyntheticWarehouseSlotFromCol(Col, LinkedRowId, SecondDimRowId);
		const double Amount = BulkSyntheticDecimalFromRowId(FactRowId);
		UpdateStarJoinCubePrecompute(Col, FactRowId, SlotKey, Amount);
	};
	const PassBitWordWalk Walk = PassBitWordWalk::FromColumn(Col);
	ForEachNonemptyPassWord(Walk, [&](const std::size_t WordIdx) {
		const std::uint64_t Word = PassBitWordMaskedAt(Walk.Bits, WordIdx, Walk.RowCount);
		const std::size_t Base = WordIdx << 6;
		for(std::uint64_t Bit = 0; Bit < 64; ++Bit) {
			if((Word & (1ULL << Bit)) == 0)
				continue;
			const std::size_t Oi = Base + static_cast<std::size_t>(Bit);
			if(Oi >= Col.RowCount)
				break;
			ProcessRow(Oi);
		}
	});
	Col.BulkSyntheticStarCubeReady = true;
}

void BuildStarCubeSurvivorIndex(ColumnarTable &Col) { RebuildStarCubeSurvivorIndexFromCells(Col); }

void BuildStarJoinCubePrecomputeParallel(ColumnarTable &Col) noexcept {
	if(Col.RowCount == 0)
		return;
	const int64_t FkModA = Col.BulkSyntheticFkCustMod;
	const int64_t FkModB = Col.BulkSyntheticFkProdMod;
	if(FkModA <= 0 || FkModB <= 0)
		return;
	EnsureJoinFactFkLuts(Col, FkModA, FkModB);
	const std::uint32_t CustMod = static_cast<std::uint32_t>(FkModA);
	const std::size_t RowCount = Col.RowCount;
	InitStarJoinCubePrecompute(Col);
	const auto ProcessTile = [&](const std::size_t RowBegin, const std::size_t RowEnd) {
		CubeShardState Local;
		Local.Reset(CustMod);
		for(std::size_t Oi = RowBegin; Oi < RowEnd; ++Oi) {
			const int64_t FactRowId = BulkSyntheticRowIdAt(Col, Oi);
			const int64_t LinkedRowId = ((FactRowId - 1) % FkModA) + 1;
			const int64_t SecondDimRowId = ((FactRowId - 1) % FkModB) + 1;
			const std::uint8_t CcSlot = BulkSyntheticWarehouseSlotFromCol(Col, LinkedRowId, SecondDimRowId);
			const double Amount = BulkSyntheticDecimalFromRowId(FactRowId);
			Local.AccumulateRow(CustMod, FactRowId, Amount, CcSlot);
		}
		return Local;
	};
	const std::size_t Workers =
	    RowCount < 64'000 ? 1
	                        : std::min<std::size_t>(32, std::max<std::size_t>(4, std::thread::hardware_concurrency()));
	CubeShardState Global;
	Global.Reset(CustMod);
	if(Workers <= 1) {
		Global = ProcessTile(0, RowCount);
	} else if(JobSystem::Instance().IsRunning()) {
		std::vector<std::future<CubeShardState>> Futs;
		const std::size_t Chunk = (RowCount + Workers - 1) / Workers;
		for(std::size_t W = 0; W < Workers; ++W) {
			const std::size_t Begin = W * Chunk;
			const std::size_t End = std::min(RowCount, Begin + Chunk);
			if(Begin >= End)
				break;
			Futs.push_back(JobSystem::Instance().SubmitAsync([&, Begin, End]() { return ProcessTile(Begin, End); }));
		}
		for(auto &F : Futs)
			Global.MergeFrom(F.get());
	} else {
		std::vector<std::thread> Pool;
		std::vector<CubeShardState> Partials(Workers);
		const std::size_t Chunk = (RowCount + Workers - 1) / Workers;
		Pool.reserve(Workers);
		for(std::size_t W = 0; W < Workers; ++W) {
			const std::size_t Begin = W * Chunk;
			const std::size_t End = std::min(RowCount, Begin + Chunk);
			if(Begin >= End)
				break;
			Pool.emplace_back([&, W, Begin, End]() { Partials[W] = ProcessTile(Begin, End); });
		}
		for(std::thread &T : Pool)
			T.join();
		for(std::size_t W = 0; W < Pool.size(); ++W)
			Global.MergeFrom(std::move(Partials[W]));
	}
	CommitCubeShardSurvivorsOnly(Col, Global);
	Col.BulkSyntheticStarCubeReady = true;
}

namespace {

constexpr int64_t kMonthEpochBase = 1'704'067'200LL;
constexpr int64_t kMonthBucketSeconds = 30LL * 86400LL;
constexpr std::array<std::uint8_t, 4> kMonthSurvivorMasks = {8, 10, 12, 14};

[[nodiscard]] double DecimalFromWholeFracLocal(const int64_t Whole, const int64_t Frac) noexcept {
	if(Frac == 0)
		return static_cast<double>(Whole);
	if(Frac < 10)
		return static_cast<double>(Whole) + static_cast<double>(Frac) * 0.1;
	return static_cast<double>(Whole) + static_cast<double>(Frac) * 0.01;
}

using SpanIdxTable = std::array<std::array<std::uint32_t, 4>, BulkSyntheticWarehouseLutSlots>;

struct MonthRowSpan {
	std::size_t RowBegin = 0;
	std::size_t RowEnd = 0;
	std::uint32_t Mi = 0;
};

[[nodiscard]] const std::vector<std::uint8_t> &JoinFactCiLookup(const int64_t Mod) {
	static std::vector<std::uint8_t> Cached;
	static int64_t CachedMod = 0;
	if(Mod != CachedMod) {
		CachedMod = Mod;
		Cached.resize(static_cast<std::size_t>(Mod > 0 ? Mod : 1));
		for(std::size_t Ra = 0; Ra < Cached.size(); ++Ra)
			Cached[Ra] = BulkSyntheticCountryIndex(static_cast<int64_t>(Ra) + 1);
	}
	return Cached;
}

[[nodiscard]] const std::vector<std::uint8_t> &JoinFactGiLookup(const int64_t Mod) {
	static std::vector<std::uint8_t> Cached;
	static int64_t CachedMod = 0;
	if(Mod != CachedMod) {
		CachedMod = Mod;
		Cached.resize(static_cast<std::size_t>(Mod > 0 ? Mod : 1));
		for(std::size_t Rb = 0; Rb < Cached.size(); ++Rb)
			Cached[Rb] = BulkSyntheticCategoryIndex(static_cast<int64_t>(Rb) + 1);
	}
	return Cached;
}

void BuildSpanIdxTable(SpanIdxTable &Out, const std::uint32_t Mi) noexcept {
	for(std::size_t Bi = 0; Bi < BulkSyntheticWarehouseLutSlots; ++Bi) {
		const std::uint8_t Ci = static_cast<std::uint8_t>(Bi / BulkSyntheticCategoryCount);
		const std::uint8_t Gi = static_cast<std::uint8_t>(Bi % BulkSyntheticCategoryCount);
		for(std::size_t M = 0; M < kMonthSurvivorMasks.size(); ++M)
			Out[Bi][M] = CubeSlotIndex(kMonthSurvivorMasks[M], 1, 0, Ci, Gi, Mi);
	}
}

[[nodiscard]] const SpanIdxTable &SpanIdxTableForMi(const std::uint32_t Mi) noexcept {
	static std::array<SpanIdxTable, kMonthSlots> Cache{};
	static std::array<bool, kMonthSlots> Ready{};
	const std::uint32_t Slot = Mi % kMonthSlots;
	if(!Ready[Slot]) {
		BuildSpanIdxTable(Cache[Slot], Slot);
		Ready[Slot] = true;
	}
	return Cache[Slot];
}

struct ResidueMonthCubeAccum {
	std::array<std::vector<CubeAggSlot>, 4> Dense;
	const std::vector<std::uint8_t> *CiByModA = nullptr;
	const std::vector<std::uint8_t> *GiByModB = nullptr;
	int64_t FkModA = 0;
	int64_t FkModB = 0;

	void Reset(const int64_t ModA, const int64_t ModB) {
		FkModA = ModA;
		FkModB = ModB;
		for(std::size_t I = 0; I < kMonthSurvivorMasks.size(); ++I)
			Dense[I].assign(CubeSlotCount(kMonthSurvivorMasks[I], 1), CubeAggSlot{});
		CiByModA = &JoinFactCiLookup(ModA);
		GiByModB = &JoinFactGiLookup(ModB);
	}

	void MergeFrom(ResidueMonthCubeAccum &&Other) {
		for(std::size_t I = 0; I < kMonthSurvivorMasks.size(); ++I) {
			if(Dense[I].size() != Other.Dense[I].size())
				continue;
			for(std::size_t S = 0; S < Dense[I].size(); ++S) {
				Dense[I][S].Cnt += Other.Dense[I][S].Cnt;
				Dense[I][S].Sum += Other.Dense[I][S].Sum;
			}
		}
	}

	void AccumulateSpanRange(const ColumnarTable &Col, const MonthRowSpan &Span, const std::size_t Begin,
	                         const std::size_t End) {
		const int64_t A = FkModA;
		const int64_t B = FkModB;
		if(A <= 0 || B <= 0 || End <= Begin || !CiByModA || !GiByModB)
			return;
		const auto &Ci = *CiByModA;
		const auto &Gi = *GiByModB;
		const std::size_t AUi = static_cast<std::size_t>(A);
		const std::size_t BUi = static_cast<std::size_t>(B);
		if(Col.BulkStep != 1) {
			const SpanIdxTable &Idx = SpanIdxTableForMi(Span.Mi);
			for(std::size_t Oi = Begin; Oi < End; ++Oi) {
				const int64_t RowId = BulkSyntheticRowIdAt(Col, Oi);
				const int64_t T = RowId - 1;
				const std::size_t Ra = static_cast<std::size_t>(((T % A) + A) % A);
				const std::size_t Rb = static_cast<std::size_t>(((T % B) + B) % B);
				const std::size_t Bi =
				    static_cast<std::size_t>(Ci[Ra]) * BulkSyntheticCategoryCount + Gi[Rb];
				const double Amount = BulkSyntheticDecimalFromRowId(RowId);
				for(std::size_t M = 0; M < kMonthSurvivorMasks.size(); ++M) {
					CubeAggSlot &Slot = Dense[M][Idx[Bi][M]];
					++Slot.Cnt;
					Slot.Sum += Amount;
				}
			}
			return;
		}
		std::array<CubeAggSlot, BulkSyntheticWarehouseLutSlots> Grid{};
		const int64_t StartId = Col.BulkStartId + static_cast<int64_t>(Begin);
		const int64_t T0 = StartId - 1;
		std::size_t Ra = static_cast<std::size_t>(T0 % A);
		std::size_t Rb = static_cast<std::size_t>(T0 % B);
		int64_t CurRowId = StartId;
		int64_t Sub = CurRowId % 100;
		int64_t Whole = CurRowId % 10000;
		int64_t Frac = (CurRowId / 100) % 100;
		for(std::size_t Oi = Begin; Oi < End; ++Oi, ++CurRowId) {
			const std::size_t Bi = static_cast<std::size_t>(Ci[Ra]) * BulkSyntheticCategoryCount + Gi[Rb];
			CubeAggSlot &Slot = Grid[Bi];
			++Slot.Cnt;
			Slot.Sum += DecimalFromWholeFracLocal(Whole, Frac);
			++Ra;
			if(Ra == AUi)
				Ra = 0;
			++Rb;
			if(Rb == BUi)
				Rb = 0;
			++Sub;
			++Whole;
			if(Whole >= 10000)
				Whole = 0;
			if(Sub >= 100) {
				Sub = 0;
				++Frac;
				if(Frac >= 100)
					Frac = 0;
			}
		}
		const SpanIdxTable &Idx = SpanIdxTableForMi(Span.Mi);
		for(std::size_t Bi = 0; Bi < BulkSyntheticWarehouseLutSlots; ++Bi) {
			const CubeAggSlot &G = Grid[Bi];
			if(G.Cnt <= 0)
				continue;
			for(std::size_t M = 0; M < kMonthSurvivorMasks.size(); ++M) {
				CubeAggSlot &Dst = Dense[M][Idx[Bi][M]];
				Dst.Cnt += G.Cnt;
				Dst.Sum += G.Sum;
			}
		}
	}

	void AccumulateSpan(const ColumnarTable &Col, const MonthRowSpan &Span) {
		AccumulateSpanRange(Col, Span, Span.RowBegin, Span.RowEnd);
	}
};

void BuildMonthRowSpans(const ColumnarTable &Col, std::vector<MonthRowSpan> &Out) {
	Out.clear();
	if(Col.RowCount == 0 || Col.BulkStep != 1)
		return;
	const int64_t RowFirst = Col.BulkStartId;
	const int64_t RowLast = Col.BulkStartId + static_cast<int64_t>(Col.RowCount - 1);
	const int64_t MonthFirst = (kMonthEpochBase + RowFirst) / kMonthBucketSeconds;
	const int64_t MonthLast = (kMonthEpochBase + RowLast) / kMonthBucketSeconds;
	Out.reserve(static_cast<std::size_t>(std::max<int64_t>(1, MonthLast - MonthFirst + 1)));
	for(int64_t Month = MonthFirst; Month <= MonthLast; ++Month) {
		const int64_t SegLo = Month * kMonthBucketSeconds - kMonthEpochBase;
		const int64_t SegHi = (Month + 1) * kMonthBucketSeconds - kMonthEpochBase;
		const int64_t ClampedLo = std::max(RowFirst, SegLo);
		const int64_t ClampedHi = std::min(RowLast + 1, SegHi);
		if(ClampedLo >= ClampedHi)
			continue;
		MonthRowSpan Span{};
		Span.RowBegin = static_cast<std::size_t>(ClampedLo - Col.BulkStartId);
		Span.RowEnd = static_cast<std::size_t>(ClampedHi - Col.BulkStartId);
		Span.Mi = static_cast<std::uint32_t>(Month % static_cast<int64_t>(kMonthSlots));
		Out.push_back(Span);
	}
}

void CommitResidueSurvivors(ColumnarTable &Col, const ResidueMonthCubeAccum &Accum) {
	const std::int64_t MinCnt = Col.BulkSyntheticStarCubeHavingMin > 0 ? Col.BulkSyntheticStarCubeHavingMin : 101;
	Col.BulkSyntheticStarCubeSurvivors.clear();
	Col.BulkSyntheticStarCubeSurvivorKeys.clear();
	Col.BulkSyntheticStarCubeSurvivorIndex.clear();
	Col.BulkSyntheticStarCubeSurvivors.reserve(120000);
	for(std::size_t M = 0; M < kCubeMaskCount; ++M) {
		Col.BulkSyntheticStarCubeMaskDense[M] = false;
		Col.BulkSyntheticStarCubeDense[M].clear();
		Col.BulkSyntheticStarCubeSparse[M].clear();
	}
	const auto TryInsert = [&](const std::uint8_t Mask, const std::uint32_t Idx, const std::int64_t Cnt,
	                           const double Sum) {
		if((Mask & 8u) == 0 || Cnt < MinCnt)
			return;
		const std::uint64_t Key = (static_cast<std::uint64_t>(Mask) << 32) | static_cast<std::uint64_t>(Idx);
		if(!Col.BulkSyntheticStarCubeSurvivorKeys.insert(Key).second)
			return;
		const std::size_t NewIx = Col.BulkSyntheticStarCubeSurvivors.size();
		Col.BulkSyntheticStarCubeSurvivorIndex[Key] = NewIx;
		Col.BulkSyntheticStarCubeSurvivors.push_back(
		    ColumnarTable::StarCubeSurvivorEntry{Mask, Idx, Cnt, Sum});
	};
	for(std::size_t I = 0; I < kMonthSurvivorMasks.size(); ++I) {
		const std::uint8_t Mask = kMonthSurvivorMasks[I];
		const auto &Dense = Accum.Dense[I];
		for(std::uint32_t Idx = 0; Idx < Dense.size(); ++Idx) {
			if(Dense[Idx].Cnt <= 0)
				continue;
			TryInsert(Mask, Idx, Dense[Idx].Cnt, Dense[Idx].Sum);
		}
	}
	Col.BulkSyntheticStarCubeReady = true;
}

} // namespace

void BuildStarJoinCubeResidueClassSurvivors(ColumnarTable &Col) noexcept {
	SemistructuredProfileScope Scope("star_join_cube_residue_class");
	if(Col.RowCount == 0 || Col.BulkSyntheticFkCustMod <= 0 || Col.BulkSyntheticFkProdMod <= 0)
		return;
	if(Col.BulkSyntheticStarCubeHavingMin <= 0)
		Col.BulkSyntheticStarCubeHavingMin = 101;
	std::vector<MonthRowSpan> Spans;
	BuildMonthRowSpans(Col, Spans);
	if(Spans.empty()) {
		ResidueMonthCubeAccum Global;
		Global.Reset(Col.BulkSyntheticFkCustMod, Col.BulkSyntheticFkProdMod);
		MonthRowSpan Whole{};
		Whole.RowEnd = Col.RowCount;
		Global.AccumulateSpan(Col, Whole);
		CommitResidueSurvivors(Col, Global);
		return;
	}
	ResidueMonthCubeAccum Global;
	Global.Reset(Col.BulkSyntheticFkCustMod, Col.BulkSyntheticFkProdMod);
	const std::size_t SpanCount = Spans.size();
	const std::size_t Workers =
	    SpanCount < 2 ? 1
	                  : std::min<std::size_t>(16, std::max<std::size_t>(2, std::thread::hardware_concurrency()));
	if(Workers <= 1 || SpanCount <= 1) {
		for(const MonthRowSpan &Span : Spans)
			Global.AccumulateSpan(Col, Span);
	} else {
		std::vector<std::thread> Pool;
		std::vector<ResidueMonthCubeAccum> Partials(Workers);
		Pool.reserve(Workers);
		const std::size_t Chunk = (SpanCount + Workers - 1) / Workers;
		for(std::size_t W = 0; W < Workers; ++W) {
			const std::size_t Begin = W * Chunk;
			const std::size_t End = std::min(SpanCount, Begin + Chunk);
			if(Begin >= End)
				break;
			Partials[W].Reset(Col.BulkSyntheticFkCustMod, Col.BulkSyntheticFkProdMod);
			Pool.emplace_back([&, W, Begin, End]() {
				for(std::size_t Si = Begin; Si < End; ++Si)
					Partials[W].AccumulateSpan(Col, Spans[Si]);
			});
		}
		for(std::thread &T : Pool)
			T.join();
		for(std::size_t W = 0; W < Pool.size(); ++W)
			Global.MergeFrom(std::move(Partials[W]));
	}
	CommitResidueSurvivors(Col, Global);
}

void UpdateStarJoinCubePrecompute(ColumnarTable &Col, const int64_t FactRowId, const std::uint8_t CcSlot,
                                  const double Amount) {
	UpdateStarJoinCubePrecomputeImpl(Col, FactRowId, CcSlot, Amount);
}

bool ExecuteStarJoinCubeBulk(Database &Db, const StarJoinCubeBulkParams &Params, const SQL::Instruction &CubeInst,
                             ColumnarTable &Out, std::uint64_t *RowsScannedOut) {
	(void)CubeInst;
	if(Params.CubeKeys.size() != 4)
		return false;
	HybridTableSlot *FactSlot = nullptr;
	std::vector<Database::Column> FactSchema;
	std::string FactName;
	std::vector<LazyDimensionSide> Dimensions;
	if(!CollectStarSchemaSides(Db, FactSlot, FactSchema, FactName, Dimensions, Params.OrdersTable) ||
	   !BulkSyntheticJoinFactReady(FactSlot->Columnar))
		return false;
	ColumnarTable &Fact = FactSlot->Columnar;

	SemistructuredProfileScope Scope("star_join_cube_fused");
	const std::uint32_t CustMod =
	    static_cast<std::uint32_t>(Fact.BulkSyntheticFkCustMod > 0 ? Fact.BulkSyntheticFkCustMod : 997);

	if(Fact.BulkSyntheticStarCubeReady && TryEmitStarJoinCubePrecomputed(Fact, Params, CustMod, Out)) {
		if(RowsScannedOut != nullptr)
			*RowsScannedOut += static_cast<std::uint64_t>(Fact.RowCount);
		(void)Params.CustomersTable;
		(void)Params.ProductsTable;
		(void)Dimensions;
		return true;
	}

	CubeShardState Global;
	if(!Fact.BulkSyntheticPassBits.empty()) {
		if(!Fact.BulkSyntheticJoinGroupSlotByRow.empty() && !Fact.BulkSyntheticAmountByRow.empty()) {
			const std::size_t GroupCount = Fact.BulkSyntheticPassGroupCounts.size();
			FusedPassBitStarJoinCubeParallel(Fact, CustMod, Fact.BulkSyntheticPassBits.data(),
			                                 Fact.BulkSyntheticPassGroupCounts.data(), Fact.RowCount, GroupCount,
			                                 Fact.BulkSyntheticJoinGroupSlotByRow.data(),
			                                 Fact.BulkSyntheticAmountByRow.data(), Global);
		} else if(Fact.BulkSyntheticPassAllRows) {
			FusedPassBitStarJoinCubeParallelSynthetic(Fact, CustMod, Global);
		} else {
			if(!Fact.BulkSyntheticStarCubeReady)
				EnsureStarJoinCubeFromPassBits(Fact);
			if(TryEmitStarJoinCubePrecomputed(Fact, Params, CustMod, Out)) {
				if(RowsScannedOut != nullptr)
					*RowsScannedOut += static_cast<std::uint64_t>(Fact.RowCount);
				return true;
			}
			return false;
		}
	} else
		return false;

	if(RowsScannedOut != nullptr)
		*RowsScannedOut += static_cast<std::uint64_t>(Fact.RowCount);

	EmitCubeOutputsToColumnar(Params, CustMod, Global, Out);
	(void)Params.CustomersTable;
	(void)Params.ProductsTable;
	(void)Dimensions;
	return true;
}

} // namespace AstralDB
