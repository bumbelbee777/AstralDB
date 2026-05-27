#include <Database/ColumnarStorage.hxx>
#include <Database/HybridStorageScheduler.hxx>
#include <SQL/Bytecode.hxx>
#include <SQL/SQL.hxx>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace AstralDB {

void ColumnarTable::RebuildFromRows(const RowTable &Rows) {
	Columns.clear();
	RowCount = Rows.size();
	if(Rows.empty())
		return;
	std::unordered_map<std::string, std::size_t> Width;
	for(const auto &Row : Rows) {
		for(const auto &[Col, Val] : Row)
			Width[Col]++;
	}
	for(const auto &[Col, W] : Width)
		Columns[Col].assign(RowCount, "");
	for(size_t Ri = 0; Ri < Rows.size(); ++Ri) {
		for(const auto &[Col, Val] : Rows[Ri])
			Columns[Col][Ri] = Val;
	}
}

RowTable ColumnarTable::MaterializeAllRows() const {
	RowTable Out;
	if(RowCount == 0)
		return Out;
	Out.resize(RowCount);
	for(const auto &[Col, Vec] : Columns) {
		if(Vec.size() != RowCount)
			continue;
		for(size_t I = 0; I < RowCount; ++I)
			Out[I][Col] = Vec[I];
	}
	return Out;
}

namespace {

int CompareScalars(const std::string &A, const std::string &B) {
	size_t PosA = 0, PosB = 0;
	try {
		const long long Ia = std::stoll(A, &PosA);
		const long long Ib = std::stoll(B, &PosB);
		if(PosA == A.size() && PosB == B.size()) {
			if(Ia < Ib)
				return -1;
			if(Ia > Ib)
				return 1;
			return 0;
		}
	} catch(...) {
	}
	if(A < B)
		return -1;
	if(A > B)
		return 1;
	return 0;
}

std::string GroupKeySignature(const RowItem &Row, const std::vector<std::string> &Keys) {
	RowItem Sub;
	for(const auto &K : Keys) {
		auto It = Row.find(K);
		Sub[K] = It == Row.end() ? "" : It->second;
	}
	std::vector<std::pair<std::string, std::string>> Pairs;
	Pairs.reserve(Sub.size());
	for(const auto &[K, V] : Sub)
		Pairs.emplace_back(K, V);
	std::sort(Pairs.begin(), Pairs.end());
	std::ostringstream O;
	for(const auto &[K, V] : Pairs)
		O << '\0' << K << '\x01' << V;
	return std::move(O).str();
}

size_t GroupByInstPayloadEnd(const SQL::Instruction &Inst) {
	size_t End = Inst.Operands.size();
	while(End > 0) {
		const auto *Tag = std::get_if<int64_t>(&Inst.Operands[End - 1]);
		if(Tag != nullptr)
			break;
		--End;
	}
	return End;
}

} // namespace

bool ColumnarGroupBy::TryRun(RowTable &Tbl, const SQL::Instruction &Inst,
                             const std::vector<std::string> &ActiveKeys) {
	const auto *Tag = std::get_if<int64_t>(&Inst.Operands[0]);
	const auto *Nk = std::get_if<int64_t>(&Inst.Operands[1]);
	if(!Tag || !Nk || *Tag != 3)
		return false;
	const size_t Base = static_cast<size_t>(2 + *Nk);
	if(Inst.Operands.size() < Base + 2)
		return false;
	const auto *HCnt = std::get_if<int64_t>(&Inst.Operands[Base]);
	const auto *Na = std::get_if<int64_t>(&Inst.Operands[Base + 1]);
	if(!HCnt || !Na || *Na < 0 || *Na > 32 ||
	   Inst.Operands.size() < Base + 2 + static_cast<size_t>(*Na) * 3)
		return false;
	const bool IncludeCountStar = (*HCnt != 0);
	const size_t IdxAfterSpecs = Base + 2 + static_cast<size_t>(*Na) * 3;
	const size_t PayloadEnd = GroupByInstPayloadEnd(Inst);
	if(PayloadEnd < IdxAfterSpecs)
		return false;
	std::string CountStarCol = "cnt";
	if(IncludeCountStar) {
		if(PayloadEnd == IdxAfterSpecs + 1) {
			const auto *Cn = std::get_if<std::string>(&Inst.Operands[IdxAfterSpecs]);
			if(!Cn || Cn->empty())
				return false;
			CountStarCol = *Cn;
		} else if(PayloadEnd != IdxAfterSpecs)
			return false;
	} else if(PayloadEnd != IdxAfterSpecs)
		return false;

	struct AggSpecVm {
		int Kind = 0;
		std::string SrcCol;
		std::string OutCol;
	};
	std::vector<AggSpecVm> Specs;
	Specs.reserve(static_cast<size_t>(*Na));
	size_t Idx = Base + 2;
	std::size_t NumNumericAggs = 0;
	for(int64_t A = 0; A < *Na; ++A) {
		const auto *Knd = std::get_if<int64_t>(&Inst.Operands[Idx++]);
		const auto *Sc = std::get_if<std::string>(&Inst.Operands[Idx++]);
		const auto *Ou = std::get_if<std::string>(&Inst.Operands[Idx++]);
		if(!Knd || !Sc || !Ou || Sc->empty() || Ou->empty())
			return false;
		Specs.push_back(AggSpecVm{static_cast<int>(*Knd), *Sc, *Ou});
		if(*Knd >= 0 && *Knd <= 3)
			++NumNumericAggs;
	}
	if(!HybridStorageScheduler::PreferColumnarGroupBy(Tbl.size(), *Tag, NumNumericAggs))
		return false;

	struct Accum {
		std::unordered_map<std::string, RowItem> KeyTemplate;
		std::unordered_map<std::string, int64_t> CntStar;
		std::unordered_map<std::string, std::vector<double>> Sum;
		std::unordered_map<std::string, std::vector<int64_t>> AvgN;
		std::unordered_map<std::string, std::vector<bool>> HaveMinMax;
		std::unordered_map<std::string, std::vector<std::string>> CurMin;
		std::unordered_map<std::string, std::vector<std::string>> CurMax;
	} Acc;
	Acc.KeyTemplate.reserve(Tbl.size());
	std::vector<std::string> KeySigs;
	KeySigs.reserve(Tbl.size());
	for(const auto &Row : Tbl)
		KeySigs.push_back(GroupKeySignature(Row, ActiveKeys));
	std::vector<std::vector<std::string>> AggColumns(Specs.size());
	for(size_t Si = 0; Si < Specs.size(); ++Si) {
		AggColumns[Si].reserve(Tbl.size());
		const auto &Sp = Specs[Si];
		for(const auto &Row : Tbl) {
			auto It = Row.find(Sp.SrcCol);
			AggColumns[Si].push_back(It == Row.end() ? "" : It->second);
		}
	}
	for(size_t Ri = 0; Ri < Tbl.size(); ++Ri) {
		const std::string &Sig = KeySigs[Ri];
		const auto &Row = Tbl[Ri];
		if(IncludeCountStar)
			Acc.CntStar[Sig]++;
		if(Acc.KeyTemplate.find(Sig) == Acc.KeyTemplate.end()) {
			RowItem R;
			for(const auto &K : ActiveKeys) {
				auto It = Row.find(K);
				R[K] = It == Row.end() ? "" : It->second;
			}
			Acc.KeyTemplate.emplace(Sig, std::move(R));
		}
		if(Acc.Sum.find(Sig) == Acc.Sum.end()) {
			Acc.Sum[Sig] = std::vector<double>(Specs.size(), 0.0);
			Acc.AvgN[Sig] = std::vector<int64_t>(Specs.size(), 0);
			Acc.HaveMinMax[Sig] = std::vector<bool>(Specs.size(), false);
			Acc.CurMin[Sig] = std::vector<std::string>(Specs.size());
			Acc.CurMax[Sig] = std::vector<std::string>(Specs.size());
		}
		auto &Sv = Acc.Sum[Sig];
		auto &Nv = Acc.AvgN[Sig];
		auto &Hm = Acc.HaveMinMax[Sig];
		auto &Cmin = Acc.CurMin[Sig];
		auto &Cmax = Acc.CurMax[Sig];
		for(size_t Si = 0; Si < Specs.size(); ++Si) {
			const auto &Sp = Specs[Si];
			const std::string &Cell = AggColumns[Si][Ri];
			switch(Sp.Kind) {
			case static_cast<int>(SQL::GroupCombAggKind::Sum):
			case static_cast<int>(SQL::GroupCombAggKind::Avg): {
				double X = 0;
				bool Ok = false;
				try {
					X = std::stod(Cell);
					Ok = true;
				} catch(...) {
				}
				if(Ok) {
					Sv[Si] += X;
					if(Sp.Kind == static_cast<int>(SQL::GroupCombAggKind::Avg))
						Nv[Si]++;
				}
			} break;
			case static_cast<int>(SQL::GroupCombAggKind::Min):
				if(!Hm[Si]) {
					Hm[Si] = true;
					Cmin[Si] = Cell;
				} else if(CompareScalars(Cell, Cmin[Si]) < 0)
					Cmin[Si] = Cell;
				break;
			case static_cast<int>(SQL::GroupCombAggKind::Max):
				if(!Hm[Si]) {
					Hm[Si] = true;
					Cmax[Si] = Cell;
				} else if(CompareScalars(Cell, Cmax[Si]) > 0)
					Cmax[Si] = Cell;
				break;
			default:
				break;
			}
		}
	}
	RowTable OutTbl;
	OutTbl.reserve(Acc.KeyTemplate.size());
	for(auto &Ky : Acc.KeyTemplate) {
		const std::string &Sig = Ky.first;
		RowItem R = Ky.second;
		if(IncludeCountStar) {
			const auto ItCnt = Acc.CntStar.find(Sig);
			R[CountStarCol] = ItCnt == Acc.CntStar.end() ? "0" : std::to_string(ItCnt->second);
		}
		const auto &Sv = Acc.Sum.at(Sig);
		const auto &Nv = Acc.AvgN.at(Sig);
		const auto &Hm = Acc.HaveMinMax.at(Sig);
		const auto &Mn = Acc.CurMin.at(Sig);
		const auto &Mx = Acc.CurMax.at(Sig);
		for(size_t Si = 0; Si < Specs.size(); ++Si) {
			const auto &Sp = Specs[Si];
			switch(Sp.Kind) {
			case static_cast<int>(SQL::GroupCombAggKind::Sum):
				R[Sp.OutCol] = std::to_string(static_cast<long long>(std::llround(Sv[Si])));
				break;
			case static_cast<int>(SQL::GroupCombAggKind::Avg): {
				if(Nv[Si] > 0) {
					std::ostringstream O;
					O << (Sv[Si] / static_cast<double>(Nv[Si]));
					R[Sp.OutCol] = O.str();
				} else
					R[Sp.OutCol] = "0";
			} break;
			case static_cast<int>(SQL::GroupCombAggKind::Min):
				R[Sp.OutCol] = Hm[Si] ? Mn[Si] : "";
				break;
			case static_cast<int>(SQL::GroupCombAggKind::Max):
				R[Sp.OutCol] = Hm[Si] ? Mx[Si] : "";
				break;
			default:
				R[Sp.OutCol] = "";
				break;
			}
		}
		OutTbl.push_back(std::move(R));
	}
	Tbl = std::move(OutTbl);
	std::sort(Tbl.begin(), Tbl.end(), [&](const RowItem &A, const RowItem &B) {
		return GroupKeySignature(A, ActiveKeys) < GroupKeySignature(B, ActiveKeys);
	});
	return true;
}

namespace {

std::string UpperCol(std::string S) {
	for(char &C : S)
		C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
	return S;
}

enum class BulkSyntheticKind : uint8_t {
	Id,
	Acct,
	Amount,
	Ts,
	TextB,
	TextC,
	TextD,
	Offset
};

BulkSyntheticKind ClassifyBulkSyntheticKind(const std::string &Col, std::size_t ColIndex, std::size_t ColCount) {
	const std::string Key = UpperCol(Col);
	if(Key == "ID")
		return BulkSyntheticKind::Id;
	if(Key == "ACCT" || Key == "A")
		return BulkSyntheticKind::Acct;
	if(Key == "AMOUNT")
		return BulkSyntheticKind::Amount;
	if(Key == "TS" || Key == "TIMESTAMP")
		return BulkSyntheticKind::Ts;
	if(Key == "B" || (ColCount == 5 && ColIndex == 2))
		return BulkSyntheticKind::TextB;
	if(Key == "C" || (ColCount == 5 && ColIndex == 3))
		return BulkSyntheticKind::TextC;
	if(Key == "D" || (ColCount == 5 && ColIndex == 4))
		return BulkSyntheticKind::TextD;
	return BulkSyntheticKind::Offset;
}

std::string BulkCellValue(BulkSyntheticKind Kind, int64_t RowId, std::size_t ColIndex) {
	switch(Kind) {
	case BulkSyntheticKind::Id:
		return std::to_string(RowId);
	case BulkSyntheticKind::Acct:
		return std::to_string(RowId % 997);
	case BulkSyntheticKind::Amount:
		return std::to_string(RowId % 10000) + "." + std::to_string((RowId / 100) % 100);
	case BulkSyntheticKind::Ts:
		return std::to_string(1'704'067'200LL + RowId);
	case BulkSyntheticKind::TextB:
		return std::string("txt_") + std::to_string(RowId);
	case BulkSyntheticKind::TextC:
		return std::string("chk_") + std::to_string(RowId % 71);
	case BulkSyntheticKind::TextD:
		return std::string("e_") + std::to_string(RowId / 17);
	case BulkSyntheticKind::Offset:
	default:
		return std::to_string(RowId + static_cast<int64_t>(ColIndex));
	}
}

std::optional<double> ParseNum(const std::string &S) {
	try {
		size_t Pos = 0;
		const double D = std::stod(S, &Pos);
		if(Pos > 0)
			return D;
	} catch(...) {
	}
	return std::nullopt;
}

} // namespace

void AppendBulkSyntheticColumnar(ColumnarTable &Col, const std::vector<std::string> &ColNames, int64_t Count,
                                 int64_t StartId, int64_t Step) {
	const std::size_t ColCount = ColNames.size();
	const std::size_t Base = Col.RowCount;
	const std::size_t NewTotal = Base + static_cast<std::size_t>(Count);
	for(const auto &Cn : ColNames) {
		auto &Vec = Col.Columns[Cn];
		if(Vec.size() < Base)
			Vec.assign(Base, "");
		Vec.reserve(NewTotal);
	}
	std::vector<BulkSyntheticKind> Kinds;
	Kinds.reserve(ColCount);
	for(std::size_t Ci = 0; Ci < ColCount; ++Ci)
		Kinds.push_back(ClassifyBulkSyntheticKind(ColNames[Ci], Ci, ColCount));
	for(int64_t K = 0; K < Count; ++K) {
		const int64_t RowId = StartId + K * Step;
		for(std::size_t Ci = 0; Ci < ColCount; ++Ci)
			Col.Columns[ColNames[Ci]].push_back(BulkCellValue(Kinds[Ci], RowId, Ci));
	}
	Col.RowCount = NewTotal;
}

bool TrySlidingSumRowsFrame(ColumnarTable &Col, const std::string &PartCol, const std::string &OrderCol,
                            const std::string &SrcCol, const std::string &OutCol, std::size_t PrecedingRows,
                            bool OrderAscending) {
	if(Col.RowCount == 0)
		return false;
	auto PartIt = Col.Columns.find(PartCol);
	auto OrdIt = Col.Columns.find(OrderCol);
	auto SrcIt = Col.Columns.find(SrcCol);
	if(PartIt == Col.Columns.end() || OrdIt == Col.Columns.end() || SrcIt == Col.Columns.end())
		return false;
	if(PartIt->second.size() != Col.RowCount || OrdIt->second.size() != Col.RowCount ||
	   SrcIt->second.size() != Col.RowCount)
		return false;
	const std::vector<std::string> &Part = PartIt->second;
	const std::vector<std::string> &Ord = OrdIt->second;
	const std::vector<std::string> &Src = SrcIt->second;
	std::vector<std::size_t> Order(Col.RowCount);
	std::iota(Order.begin(), Order.end(), std::size_t{0});
	const auto Less = [&](std::size_t A, std::size_t B) {
		if(Part[A] != Part[B])
			return Part[A] < Part[B];
		const auto Na = ParseNum(Ord[A]);
		const auto Nb = ParseNum(Ord[B]);
		if(Na && Nb)
			return OrderAscending ? *Na < *Nb : *Na > *Nb;
		return OrderAscending ? Ord[A] < Ord[B] : Ord[A] > Ord[B];
	};
	std::stable_sort(Order.begin(), Order.end(), Less);
	auto &Out = Col.Columns[OutCol];
	Out.assign(Col.RowCount, "");
	const std::size_t Width = PrecedingRows + 1;
	std::size_t PartStart = 0;
	for(std::size_t R = 0; R <= Col.RowCount; ++R) {
		const bool Boundary = R == Col.RowCount || (R > 0 && Part[Order[R]] != Part[Order[R - 1]]);
		if(!Boundary)
			continue;
		if(R > PartStart) {
			double Ring[32]{};
			std::size_t RingLen = 0;
			std::size_t RingPos = 0;
			double Sum = 0;
			for(std::size_t K = PartStart; K < R; ++K) {
				const std::size_t Ix = Order[K];
				const double V = ParseNum(Src[Ix]).value_or(0.);
				if(RingLen < Width) {
					Ring[RingLen++] = V;
					Sum += V;
				} else {
					Sum -= Ring[RingPos];
					Ring[RingPos] = V;
					Sum += V;
					RingPos = (RingPos + 1) % Width;
				}
				Out[Ix] = std::to_string(static_cast<long long>(std::llround(Sum)));
			}
		}
		PartStart = R;
	}
	return true;
}

} // namespace AstralDB
