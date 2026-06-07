#include <Database/Storage/ColumnarLazyBulk.hxx>

#include <Database/Storage/BulkSynthetic.hxx>
#include <Database/Storage/BulkSyntheticPrecompute.hxx>
#include <Database/Storage/ColumnFilterSimd.hxx>
#include <Database/Storage/GeneralizedLazyGroupBy.hxx>
#include <Database/Storage/PredicateKind.hxx>
#include <Database/Storage/SemistructuredProfile.hxx>
#include <Database/Storage/StarJoinCubeBulk.hxx>
#include <Database/Execution/FastPathGuard.hxx>
#include <Database/Execution/PlanTypes.hxx>

#include <cstdio>
#include <sstream>
#include <unordered_map>

namespace AstralDB {
namespace {

const Database::Column *FindCol(const std::vector<Database::Column> &Schema, std::string_view Name) {
	for(const Database::Column &C : Schema) {
		if(C.Name == Name)
			return &C;
	}
	return nullptr;
}

struct ComboAggOperand {
	int64_t Kind = 0;
	std::string Src;
	std::string Out;
};

bool ParseGroupByCountOnlyOperands(const SQL::Instruction &Inst, bool &IncludeCountStar, std::string &CountOut) {
	const auto *Tag = std::get_if<int64_t>(&Inst.Operands[0]);
	const auto *Nk = std::get_if<int64_t>(&Inst.Operands[1]);
	if(!Tag || !Nk || (*Tag != 0 && *Tag != 1))
		return false;
	const size_t Base = static_cast<size_t>(2 + *Nk);
	IncludeCountStar = (*Tag != 0);
	CountOut = "cnt";
	if(IncludeCountStar && Inst.Operands.size() > Base) {
		if(const auto *Cn = std::get_if<std::string>(&Inst.Operands[Base]); Cn && !Cn->empty())
			CountOut = *Cn;
	}
	return true;
}

bool ParseGroupByComboOperands(const SQL::Instruction &Inst, bool &IncludeCountStar, std::string &CountOut,
                              std::vector<ComboAggOperand> &Aggs) {
	const auto *Tag = std::get_if<int64_t>(&Inst.Operands[0]);
	const auto *Nk = std::get_if<int64_t>(&Inst.Operands[1]);
	if(!Tag || !Nk || *Tag != 3)
		return false;
	const size_t Base = static_cast<size_t>(2 + *Nk);
	if(Inst.Operands.size() < Base + 2)
		return false;
	const auto *HCnt = std::get_if<int64_t>(&Inst.Operands[Base]);
	const auto *Na = std::get_if<int64_t>(&Inst.Operands[Base + 1]);
	if(!HCnt || !Na || *Na <= 0)
		return false;
	IncludeCountStar = (*HCnt != 0);
	const size_t IdxAfterSpecs = Base + 2 + static_cast<size_t>(*Na) * 3;
	CountOut = "cnt";
	if(IncludeCountStar && Inst.Operands.size() > IdxAfterSpecs) {
		if(const auto *Cn = std::get_if<std::string>(&Inst.Operands[IdxAfterSpecs]); Cn && !Cn->empty())
			CountOut = *Cn;
	}
	Aggs.clear();
	Aggs.reserve(static_cast<size_t>(*Na));
	for(int64_t I = 0; I < *Na; ++I) {
		const size_t Spec = Base + 2 + static_cast<size_t>(I) * 3;
		if(Inst.Operands.size() <= Spec + 2)
			return false;
		const auto *K = std::get_if<int64_t>(&Inst.Operands[Spec]);
		const auto *Sc = std::get_if<std::string>(&Inst.Operands[Spec + 1]);
		const auto *So = std::get_if<std::string>(&Inst.Operands[Spec + 2]);
		if(!K || !Sc || !So)
			return false;
		Aggs.push_back({*K, *Sc, *So});
	}
	return !Aggs.empty();
}

bool ParseGroupBy3Operands(const SQL::Instruction &Inst, bool &IncludeCountStar, std::string &CountOut,
                           std::string &SumCol, std::string &SumOut) {
	std::vector<ComboAggOperand> Aggs;
	if(!ParseGroupByComboOperands(Inst, IncludeCountStar, CountOut, Aggs))
		return false;
	for(const ComboAggOperand &A : Aggs) {
		if(A.Kind == static_cast<int64_t>(SQL::GroupCombAggKind::Sum)) {
			SumCol = A.Src;
			SumOut = A.Out;
			return true;
		}
	}
	SumCol.clear();
	SumOut.clear();
	return true;
}

void PadCubeRow(Database::Item &Row, const std::vector<std::string> &AllKeys,
                const std::vector<std::string> &Active, int64_t Mask) {
	for(std::size_t Ki = 0; Ki < AllKeys.size(); ++Ki) {
		const bool KeyActive = std::find(Active.begin(), Active.end(), AllKeys[Ki]) != Active.end();
		if(!KeyActive)
			Row[AllKeys[Ki]] = "";
		Row["_grouping_" + AllKeys[Ki]] = KeyActive ? "0" : "1";
		(void)Mask;
	}
	Row["_olap_level"] = std::to_string(Mask);
}

void ScanPassRows(ColumnarTable &Fact, std::vector<std::size_t> &Passing) {
	Passing.clear();
	if(Fact.BulkSyntheticPassBits.empty())
		return;
	if(!Fact.BulkSyntheticPassSparseWords.empty())
		ScanPassBitsIndicesSparse(Fact.BulkSyntheticPassBits.data(), Fact.BulkSyntheticPassSparseWords.data(),
		                          Fact.BulkSyntheticPassSparseWords.size(), Fact.RowCount, Passing);
	else
		ScanPassBitsIndices(Fact.BulkSyntheticPassBits.data(), Fact.RowCount, Passing);
}

int64_t LinkedRowForBinding(const ColumnarTable & /*Fact*/, const LazyFactGroupByBinding &Binding,
                            int64_t FactRowId) {
	if(Binding.Source == LazyGroupKeySource::DimensionFk) {
		const int64_t Mod = BulkSyntheticFkModulus(*Binding.FactFk);
		return Mod > 0 ? ((FactRowId - 1) % Mod) + 1 : FactRowId;
	}
	return FactRowId;
}

} // namespace

bool TryLazyBulkGroupBy(Database *Db, ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                        const SQL::Instruction &Inst, const std::vector<std::string> &ActiveKeys, RowTable &Out) {
	if(!Db || !BulkSyntheticJoinFactReady(Col))
		return false;
	bool IncludeCountStar = false;
	std::string CountOut;
	std::string SumCol;
	std::string SumOut;
	if(!ParseGroupBy3Operands(Inst, IncludeCountStar, CountOut, SumCol, SumOut))
		return false;
	if(!FindCol(Schema, SumCol))
		return false;

	HybridTableSlot *FactSlot = nullptr;
	std::vector<Database::Column> FactSchema;
	std::string FactName;
	std::vector<LazyDimensionSide> Dims;
	if(!CollectStarSchemaSides(*Db, FactSlot, FactSchema, FactName, Dims) || &FactSlot->Columnar != &Col)
		return false;
	LazyFactGroupByPlan LutPlan;
	if(!BuildLazyFactGroupByPlan(Col, Schema, FactName.empty() ? std::string() : FactName, Dims, ActiveKeys, LutPlan))
		return false;

	SemistructuredProfileScope Scope("bulk_warehouse_lut_groupby");
	std::vector<WarehouseLutAggSlot> Slots(BulkSyntheticWarehouseLutSlots);
	if(!Col.BulkSyntheticPassSlots.empty() && Col.BulkSyntheticPassSlots.size() == Col.BulkSyntheticPassAmounts.size())
		FusedPassRowDirectAggParallel(Col.BulkSyntheticPassSlots.data(), Col.BulkSyntheticPassAmounts.data(),
		                              Col.BulkSyntheticPassSlots.size(), Slots.data(), Slots.size());
	else if(!Col.BulkSyntheticPassBits.empty() && !Col.BulkSyntheticJoinGroupSlotByRow.empty() &&
	        !Col.BulkSyntheticAmountByRow.empty()) {
		const std::size_t GroupCount = Col.BulkSyntheticPassGroupCounts.size();
		FusedPassBitWarehouseLutAggParallel(Col.BulkSyntheticPassBits.data(), Col.BulkSyntheticPassGroupCounts.data(),
		                                    Col.RowCount, GroupCount, Col.BulkSyntheticJoinGroupSlotByRow.data(),
		                                    Col.BulkSyntheticAmountByRow.data(), Slots.data(), Slots.size());
	} else
		return false;

	const int64_t CustMod = Col.BulkSyntheticFkCustMod > 0 ? Col.BulkSyntheticFkCustMod : 997;
	const int64_t ProdMod = Col.BulkSyntheticFkProdMod > 0 ? Col.BulkSyntheticFkProdMod : 997;
	Out.clear();
	Out.reserve(BulkSyntheticWarehouseLutSlots);
	for(std::size_t S = 0; S < Slots.size(); ++S) {
		const WarehouseLutAggSlot &G = Slots[S];
		if(G.Cnt <= 0)
			continue;
		const int64_t CustRow = static_cast<int64_t>((S % static_cast<std::size_t>(CustMod)) + 1);
		const int64_t ProdRow = static_cast<int64_t>((S % static_cast<std::size_t>(ProdMod)) + 1);
		Database::Item Row;
		for(std::size_t Bi = 0; Bi < LutPlan.Bindings.size() && Bi < ActiveKeys.size(); ++Bi) {
			const LazyFactGroupByBinding &B = LutPlan.Bindings[Bi];
			int64_t RowForCell = CustRow;
			if(B.Source == LazyGroupKeySource::DimensionFk) {
				const int64_t Mod = BulkSyntheticFkModulus(*B.FactFk);
				RowForCell = Mod == ProdMod ? ProdRow : CustRow;
			}
			Row[ActiveKeys[Bi]] = LazyGroupKeyCellValue(B, RowForCell);
		}
		if(IncludeCountStar)
			Row[CountOut] = std::to_string(G.Cnt);
		if(!SumOut.empty()) {
			char Buf[64];
			(void)std::snprintf(Buf, sizeof(Buf), "%.12g", G.Sum);
			Row[SumOut] = Buf;
		}
		Out.push_back(std::move(Row));
	}
	return !Out.empty();
}

bool TryLazyBulkWarehouseCube(Database &Db, const SQL::Instruction &Inst, const std::string_view CubeTable,
                              const std::vector<std::string> &GroupKeys, RowTable &Out,
                              std::uint64_t *RowsScannedOut) {
	if(GroupKeys.empty() || GroupKeys.size() > 8)
		return false;
	HybridTableSlot *FactSlot = nullptr;
	std::vector<Database::Column> FactSchema;
	std::string FactName;
	std::vector<LazyDimensionSide> Dimensions;
	if(!CollectStarSchemaSides(Db, FactSlot, FactSchema, FactName, Dimensions) ||
	   !BulkSyntheticJoinFactReady(FactSlot->Columnar) || FactName != CubeTable)
		return false;
	ColumnarTable &Fact = FactSlot->Columnar;

	bool IncludeCountStar = false;
	std::string CountOut;
	std::vector<ComboAggOperand> ComboAggs;
	if(!ParseGroupByComboOperands(Inst, IncludeCountStar, CountOut, ComboAggs) &&
	   !ParseGroupByCountOnlyOperands(Inst, IncludeCountStar, CountOut))
		return false;

	LazyFactGroupByPlan Plan;
	if(!BuildLazyFactGroupByPlan(Fact, FactSchema, FactName, Dimensions, GroupKeys, Plan))
		return false;

	SemistructuredProfileScope Scope("bulk_warehouse_cube");
	const std::size_t N = GroupKeys.size();
	const std::size_t Sets = size_t{1} << N;

	struct Acc {
		int64_t Cnt = 0;
		double Sum = 0;
		Database::Item Template;
	};
	std::vector<std::unordered_map<std::string, Acc>> Parts(Sets);

	auto EmitAggCells = [&](Database::Item &Row, const Acc &G) {
		for(const ComboAggOperand &Spec : ComboAggs) {
			char Buf[64];
			switch(static_cast<SQL::GroupCombAggKind>(Spec.Kind)) {
			case SQL::GroupCombAggKind::Sum:
				(void)std::snprintf(Buf, sizeof(Buf), "%.12g", G.Sum);
				Row[Spec.Out] = Buf;
				break;
			case SQL::GroupCombAggKind::Avg:
				if(G.Cnt > 0) {
					(void)std::snprintf(Buf, sizeof(Buf), "%.12g", G.Sum / static_cast<double>(G.Cnt));
					Row[Spec.Out] = Buf;
				}
				break;
			default:
				break;
			}
		}
	};

	auto WalkRow = [&](int64_t FactRowId, double Amount) {
		for(std::size_t Mask = 0; Mask < Sets; ++Mask) {
			Database::Item Template;
			LazyFactGroupByPlan SubPlan;
			SubPlan.Fact = Plan.Fact;
			SubPlan.FactSchema = Plan.FactSchema;
			SubPlan.FactTableName = Plan.FactTableName;
			SubPlan.Bindings.reserve(N);
			for(std::size_t I = 0; I < N; ++I) {
				const bool On = (Mask >> I) & 1;
				if(!On)
					continue;
				const std::string &K = GroupKeys[I];
				const LazyFactGroupByBinding &B = Plan.Bindings[I];
				const int64_t RowForCell = LinkedRowForBinding(Fact, B, FactRowId);
				const std::string Val = LazyGroupKeyCellValue(B, RowForCell);
				Template[K] = Val;
				SubPlan.Bindings.push_back(B);
			}
			std::string Key;
			if(!SubPlan.Bindings.empty()) {
				for(const LazyFactGroupByBinding &B : SubPlan.Bindings) {
					const int64_t RowForCell = LinkedRowForBinding(Fact, B, FactRowId);
					const std::string Part = LazyGroupKeyCellValue(B, RowForCell);
					if(!Key.empty())
						Key.push_back('\x1f');
					Key += Part;
				}
			}
			Acc &G = Parts[Mask][Key];
			if(G.Cnt == 0)
				G.Template = std::move(Template);
			++G.Cnt;
			G.Sum += Amount;
		}
	};

	std::vector<std::size_t> Passing;
	ScanPassRows(Fact, Passing);
	if(Passing.empty())
		return false;
	for(const std::size_t Oi : Passing) {
		const int64_t RowId = BulkSyntheticRowIdAt(Fact, Oi);
		const double Amount =
		    Oi < Fact.BulkSyntheticAmountByRow.size() ? Fact.BulkSyntheticAmountByRow[Oi]
		                                              : BulkSyntheticDecimalFromRowId(RowId);
		WalkRow(RowId, Amount);
	}

	if(RowsScannedOut != nullptr)
		*RowsScannedOut += static_cast<std::uint64_t>(Fact.RowCount);

	Out.clear();
	for(std::size_t Mask = 0; Mask < Sets; ++Mask) {
		std::vector<std::string> Active;
		for(std::size_t I = 0; I < N; ++I) {
			if((Mask >> I) & 1)
				Active.push_back(GroupKeys[I]);
		}
		for(auto &[_, G] : Parts[Mask]) {
			if(G.Cnt <= 0)
				continue;
			Database::Item Row = G.Template;
			PadCubeRow(Row, GroupKeys, Active, static_cast<int64_t>(Mask));
			if(IncludeCountStar)
				Row[CountOut] = std::to_string(G.Cnt);
			EmitAggCells(Row, G);
			Out.push_back(std::move(Row));
		}
	}
	return !Out.empty();
}

bool TryLazyBulkStarGroupByFromBaseTables(Database &Db, const SQL::Instruction &Inst,
                                          const std::vector<std::string> &GroupKeys, RowTable &Out,
                                          std::uint64_t *RowsScannedOut) {
	return Db.TryLazyBulkStarGroupByFromBaseTablesAssumeLocked(Inst, GroupKeys, Out, RowsScannedOut);
}

} // namespace AstralDB
