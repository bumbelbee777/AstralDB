#include <SQL/Fusion/FusedOps.hxx>

#include <Database/Storage/AggEngine.hxx>
#include <Database/Storage/BulkSyntheticSemistructured.hxx>
#include <Database/Storage/ColumnarLazyBulk.hxx>
#include <Database/Storage/ColumnarScanFilter.hxx>
#include <SQL/Bulk/BulkOps.hxx>
#include <Database/Storage/ColumnFilterSimd.hxx>
#include <SQL/Bytecode/BytecodeInterpreter.hxx>
#include <SQL/Bytecode/Bytecode.hxx>

#include <cstdlib>

namespace AstralDB {
namespace SQL {

namespace {

using DnfBranch = std::vector<std::tuple<std::string, std::string, std::string>>;
using Dnf = std::vector<DnfBranch>;

HybridTableSlot *TableSlot(Database &Db, const std::string &Name) {
	return Db.FindTableSlotAssumeDbMutexHeld(Name);
}

Dnf FiltersToDnf(const std::vector<FilterTriple> &Filters) {
	DnfBranch Branch;
	for(const auto &F : Filters)
		Branch.emplace_back(F.Column, F.Op, F.Literal);
	return {Branch};
}

FilterCompareOp OpFromString(const std::string &Op) {
	if(Op == "!=" || Op == "<>")
		return FilterCompareOp::Ne;
	if(Op == ">")
		return FilterCompareOp::Gt;
	if(Op == ">=")
		return FilterCompareOp::Ge;
	if(Op == "<")
		return FilterCompareOp::Lt;
	if(Op == "<=")
		return FilterCompareOp::Le;
	return FilterCompareOp::Eq;
}

bool RowMatchesFilter(const Database::Item &Row, const FilterTriple &F) {
	const auto It = Row.find(F.Column);
	const std::string Val = It == Row.end() ? "" : It->second;
	const FilterCompareOp Op = OpFromString(F.Op);
	const int64_t Lit = std::strtoll(F.Literal.c_str(), nullptr, 10);
	const int64_t Cell = std::strtoll(Val.c_str(), nullptr, 10);
	switch(Op) {
	case FilterCompareOp::Eq:
		return Cell == Lit;
	case FilterCompareOp::Ne:
		return Cell != Lit;
	case FilterCompareOp::Gt:
		return Cell > Lit;
	case FilterCompareOp::Ge:
		return Cell >= Lit;
	case FilterCompareOp::Lt:
		return Cell < Lit;
	case FilterCompareOp::Le:
		return Cell <= Lit;
	}
	return false;
}

bool FilterRowStore(const RowTable &Rows, const std::vector<FilterTriple> &Filters, RowTable &Out) {
	Out.clear();
	Out.reserve(Rows.size());
	for(const Database::Item &Row : Rows) {
		bool Pass = true;
		for(const FilterTriple &F : Filters) {
			if(!RowMatchesFilter(Row, F)) {
				Pass = false;
				break;
			}
		}
		if(Pass)
			Out.push_back(Row);
	}
	return true;
}

Instruction MakeGroupByInst(const std::vector<std::string> &Keys, const std::string &SumCol,
                          const std::string &SumOut) {
	std::vector<Value> Ops;
	Ops.push_back(static_cast<int64_t>(2));
	Ops.push_back(static_cast<int64_t>(Keys.size()));
	for(const auto &K : Keys)
		Ops.push_back(K);
	Ops.push_back(static_cast<int64_t>(1));
	Ops.push_back(static_cast<int64_t>(GroupCombAggKind::Sum));
	Ops.push_back(SumCol);
	Ops.push_back(SumOut);
	Instruction Inst;
	Inst.Opcode_ = Opcode::GROUP_BY;
	Inst.Operands = std::move(Ops);
	return Inst;
}

void ApplyLimitOffset(RowTable &Rows, int64_t Limit, int64_t Offset) {
	if(Offset > 0 && static_cast<std::size_t>(Offset) < Rows.size())
		Rows.erase(Rows.begin(), Rows.begin() + static_cast<std::size_t>(Offset));
	else if(Offset >= static_cast<int64_t>(Rows.size()))
		Rows.clear();
	if(Limit >= 0 && static_cast<std::size_t>(Limit) < Rows.size())
		Rows.resize(static_cast<std::size_t>(Limit));
}

} // namespace

bool ExecuteFusedScanFilter(Database &Db, const FusedScanFilterParams &Params, RowTable &Out) {
	HybridTableSlot *Slot = TableSlot(Db, Params.Table);
	if(!Slot)
		return false;
	Slot->SyncColumnarAfterRowMutation();
	ColumnarTable &Col = Slot->Columnar;
	const auto Schema = Db.TableSchemaAssumeDbMutexHeld(Params.Table);
	if(!Schema)
		return false;

	if(!Slot->ColumnarSynced)
		return FilterRowStore(Slot->RowStore, Params.Filters, Out);

	if(Col.BulkSyntheticLazy) {
		const Dnf Branches = FiltersToDnf(Params.Filters);
		std::uint64_t Scanned = 0;
		return TryColumnarFilterDnfLazy(Col, *Schema, Branches, &Db, Params.Table, Out, &Scanned);
	}

	if(Params.Filters.size() == 1) {
		const auto &F = Params.Filters[0];
		const auto Hits = FilterColumnarRows(Col, F.Column, OpFromString(F.Op), F.Literal);
		Out.clear();
		Out.reserve(Hits.size());
		const RowTable All = Col.MaterializeAllRows();
		for(const std::size_t I : Hits) {
			if(I < All.size())
				Out.push_back(All[I]);
		}
		return true;
	}

	return TryFilterColumnarDnf(Col, *Schema, FiltersToDnf(Params.Filters), Out);
}

bool ExecuteFusedScanFilterAggSum(Database &Db, const FusedScanFilterAggSumParams &Params, RowTable &Out,
                                  std::uint64_t *RowsScannedOut) {
	HybridTableSlot *Slot = TableSlot(Db, Params.Table);
	if(!Slot)
		return false;
	Slot->SyncColumnarAfterRowMutation();
	ColumnarTable &Col = Slot->Columnar;
	const auto Schema = Db.TableSchemaAssumeDbMutexHeld(Params.Table);
	if(!Schema)
		return false;

	if(Col.BulkSyntheticLazy) {
		const Dnf Branches = FiltersToDnf(Params.Filters);
		RowTable Filtered;
		std::uint64_t Scanned = 0;
		if(!TryColumnarFilterDnfLazy(Col, *Schema, Branches, &Db, Params.Table, Filtered, &Scanned))
			return false;
		if(RowsScannedOut)
			*RowsScannedOut = Scanned;
		Slot->RowStore = std::move(Filtered);
		const Instruction Inst = MakeGroupByInst(Params.GroupKeys, Params.SumColumn, Params.SumOutputColumn);
		if(TryLazyBulkGroupBy(&Db, Col, *Schema, Inst, Params.GroupKeys, Out))
			return true;
		return AggEngine::TryRun(Out, Inst, Params.GroupKeys);
	}

	RowTable Filtered;
	if(!ExecuteFusedScanFilter(Db, {Params.Table, Params.Filters}, Filtered))
		return false;
	const Instruction Inst = MakeGroupByInst(Params.GroupKeys, Params.SumColumn, Params.SumOutputColumn);
	if(!AggEngine::TryRun(Filtered, Inst, Params.GroupKeys))
		return false;
	Out = std::move(Filtered);
	return true;
}

bool ExecuteFusedScanFilterAggLimit(Database &Db, const FusedScanFilterAggLimitParams &Params, RowTable &Out,
                                    std::uint64_t *RowsScannedOut) {
	if(!ExecuteFusedScanFilterAggSum(Db, Params, Out, RowsScannedOut))
		return false;
	ApplyLimitOffset(Out, Params.Limit, Params.Offset);
	return true;
}

bool ExecuteFusedJoinFilter(Database &Db, const FusedJoinFilterParams &Params, RowTable &Out,
                            std::uint64_t *RowsScannedOut) {
	HybridTableSlot *Left = TableSlot(Db, Params.LeftTable);
	HybridTableSlot *Right = TableSlot(Db, Params.RightTable);
	if(!Left || !Right)
		return false;
	Left->SyncColumnarAfterRowMutation();
	Right->SyncColumnarAfterRowMutation();

	const auto LSchema = Db.TableSchemaAssumeDbMutexHeld(Params.LeftTable);
	const auto RSchema = Db.TableSchemaAssumeDbMutexHeld(Params.RightTable);
	if(!LSchema || !RSchema)
		return false;

	RowTable Joined;
	std::uint64_t Scanned = 0;
	if(Left->Columnar.BulkSyntheticLazy && Right->Columnar.BulkSyntheticLazy) {
		if(!TryColumnarInnerJoinEqualityLazy(Left->Columnar, Right->Columnar, *LSchema, *RSchema,
		                                     Params.LeftCol, Params.RightCol, Joined, &Db, Params.LeftTable,
		                                     Params.RightTable, &Scanned))
			return false;
	} else if(!TryColumnarInnerJoinEquality(Left->Columnar, Right->Columnar, Params.LeftCol, Params.RightCol, Joined)) {
		return false;
	}

	if(RowsScannedOut)
		*RowsScannedOut = Scanned;

	if(Params.Filters.empty()) {
		Out = std::move(Joined);
		return true;
	}

	Out.clear();
	for(const auto &Row : Joined) {
		bool Pass = true;
		for(const auto &F : Params.Filters) {
			const auto It = Row.find(F.Column);
			const std::string Val = It == Row.end() ? "" : It->second;
			const FilterCompareOp Op = OpFromString(F.Op);
			const int64_t Lit = std::strtoll(F.Literal.c_str(), nullptr, 10);
			const int64_t Cell = std::strtoll(Val.c_str(), nullptr, 10);
			switch(Op) {
			case FilterCompareOp::Eq:
				Pass = Cell == Lit;
				break;
			case FilterCompareOp::Ne:
				Pass = Cell != Lit;
				break;
			case FilterCompareOp::Gt:
				Pass = Cell > Lit;
				break;
			case FilterCompareOp::Ge:
				Pass = Cell >= Lit;
				break;
			case FilterCompareOp::Lt:
				Pass = Cell < Lit;
				break;
			case FilterCompareOp::Le:
				Pass = Cell <= Lit;
				break;
			}
			if(!Pass)
				break;
		}
		if(Pass)
			Out.push_back(Row);
	}
	return true;
}

bool ExecuteFusedFilterMerge(Database &Db, const std::string &Table, const std::vector<FilterTriple> &Filters,
                             RowTable &Out) {
	if(Filters.size() < 2)
		return false;
	HybridTableSlot *Slot = TableSlot(Db, Table);
	if(!Slot || !Slot->Columnar.BulkSyntheticLazy)
		return false;
	const auto Schema = Db.TableSchemaAssumeDbMutexHeld(Table);
	if(!Schema)
		return false;

	BulkWhereDnfBranch Branch;
	for(const auto &F : Filters)
		Branch.emplace_back(F.Column, F.Op, F.Literal);
	Slot->Columnar.BulkSyntheticWhereDnfs.push_back({Branch});
	std::uint64_t Scanned = 0;
	const BulkWhereDnf Empty{};
	return TryColumnarFilterDnfLazy(Slot->Columnar, *Schema, Empty, &Db, Table, Out, &Scanned);
}

bool ParseFilterOperands(const Instruction &Inst, std::size_t &Off, std::vector<FilterTriple> &Out) {
	if(Off >= Inst.Operands.size())
		return false;
	const auto *Cnt = std::get_if<int64_t>(&Inst.Operands[Off++]);
	if(!Cnt || *Cnt < 0)
		return false;
	for(int64_t I = 0; I < *Cnt; ++I) {
		if(Off + 2 >= Inst.Operands.size())
			return false;
		const auto *Col = std::get_if<std::string>(&Inst.Operands[Off++]);
		const auto *Op = std::get_if<std::string>(&Inst.Operands[Off++]);
		const auto *Lit = std::get_if<std::string>(&Inst.Operands[Off++]);
		if(!Col || !Op || !Lit)
			return false;
		Out.push_back({*Col, *Op, *Lit});
	}
	return true;
}

bool HandleFusedOpcode(BytecodeInterpreter &Vm, const Instruction &Inst) {
	Vm.EnsurePrimaryDatabaseOpened();
	Database *Db = Vm.PrimaryDatabase();
	if(!Db)
		return false;
	const auto *Dest = Inst.Operands.empty() ? nullptr : std::get_if<std::string>(&Inst.Operands[0]);
	if(!Dest || Dest->empty())
		return false;
	RowTable Out;
	std::size_t Off = 2;
	std::vector<FilterTriple> Filters;
	if(!ParseFilterOperands(Inst, Off, Filters))
		return false;
	std::uint64_t Scanned = 0;
	bool Ok = false;
	switch(Inst.Opcode_) {
	case Opcode::FUSED_SCAN_FILTER:
		Ok = ExecuteFusedScanFilter(*Db, {Inst.Operands.size() > 1 ? std::get<std::string>(Inst.Operands[1]) : *Dest,
		                                  Filters},
		                            Out);
		break;
	case Opcode::FUSED_SCAN_FILTER_AGG: {
		FusedScanFilterAggSumParams P;
		P.Table = std::get<std::string>(Inst.Operands[1]);
		P.Filters = Filters;
		if(Off < Inst.Operands.size()) {
			const auto *Gk = std::get_if<int64_t>(&Inst.Operands[Off++]);
			if(Gk)
				for(int64_t I = 0; I < *Gk && Off < Inst.Operands.size(); ++I)
					if(const auto *K = std::get_if<std::string>(&Inst.Operands[Off++]))
						P.GroupKeys.push_back(*K);
		}
		if(Off + 1 < Inst.Operands.size()) {
			if(const auto *S = std::get_if<std::string>(&Inst.Operands[Off++]))
				P.SumColumn = *S;
			if(const auto *O = std::get_if<std::string>(&Inst.Operands[Off++]))
				P.SumOutputColumn = *O;
		}
		Ok = ExecuteFusedScanFilterAggSum(*Db, P, Out, &Scanned);
		break;
	}
	case Opcode::FUSED_SCAN_PROJECT_LIMIT: {
		Instruction BulkInst = Inst;
		BulkInst.Opcode_ = Opcode::SEMISTRUCTURED_TOPK_BULK;
		if(BulkInst.Operands.size() > 1) {
			if(const auto *T = std::get_if<std::string>(&BulkInst.Operands[1]))
				BulkInst.Operands[0] = *T;
		} else if(!BulkInst.Operands.empty()) {
			BulkInst.Operands[0] = *Dest;
		}
		return RunSemistructuredTopk(*Db, Vm, BulkInst);
	}
	default:
		return false;
	}
	if(!Ok)
		return false;
	Vm.MutableTimeSqlStats().RowsScanned += Scanned;
	Vm.MutableTimeSqlStats().ResultRows = Out.size();
	Db->ReplaceTableContents(*Dest, Db->TableSchemaAssumeDbMutexHeld(*Dest).value_or(Database::Schema{}),
	                         std::move(Out));
	return true;
}

} // namespace SQL
} // namespace AstralDB
