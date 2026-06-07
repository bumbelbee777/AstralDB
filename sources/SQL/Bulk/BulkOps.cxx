#include <SQL/Bulk/BulkOps.hxx>

#include <Database/Storage/BulkSyntheticPrecompute.hxx>
#include <Database/Storage/BulkShapePlan.hxx>
#include <Database/Storage/BulkSyntheticPrecomputeColumns.hxx>
#include <Database/Storage/HybridTable.hxx>
#include <Database/Storage/PredicateKind.hxx>
#include <Database/Storage/AggEngine.hxx>
#include <Database/Storage/BulkSyntheticSemistructured.hxx>
#include <Database/Storage/SemistructuredProfile.hxx>
#include <Database/Storage/SemistructuredResultStrips.hxx>
#include <SQL/Bulk/SqlBytecodeTail.hxx>
#include <Database/Storage/ColumnarLazyBulk.hxx>
#include <Database/Storage/GeneralizedLazyGroupBy.hxx>
#include <Database/Storage/LazyStarJoinSelect.hxx>
#include <Database/Storage/StarJoinCubeBulk.hxx>
#include <Database/Storage/StarJoinCubeFusedTail.hxx>
#include <Database/Storage/StarJoinCubeAnalytic.hxx>
#include <SQL/Bytecode/BytecodeInterpreter.hxx>
#include <SQL/Bytecode/Bytecode.hxx>
#include <SQL/Bytecode/FastPathGuard.hxx>
#include <SQL/Fusion/FusionPlanTypes.hxx>
#include <SQL/Profiler/QueryProfiler.hxx>
#include <SQL/SemistructuredPlanner.hxx>
#include <SQL/SemistructuredVM.hxx>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <utility>

namespace AstralDB {
namespace SQL {

namespace {

using DnfBranch = std::vector<std::tuple<std::string, std::string, std::string>>;

HybridTableSlot *TableSlot(Database &Db, const std::string &Name) {
	return Db.FindTableSlotAssumeDbMutexHeld(Name);
}

Database::Schema SingleColumnSchema(const std::string &ColName) {
	Database::Schema Sch;
	Database::Column C;
	C.Name = ColName;
	C.DefaultValue = "INTEGER";
	Sch.push_back(std::move(C));
	return Sch;
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

BulkWhereDnf FiltersToBulkDnf(const std::vector<FilterTriple> &Filters) {
	BulkWhereDnf Dnf;
	if(!Filters.empty()) {
		DnfBranch Branch;
		for(const auto &F : Filters)
			Branch.emplace_back(F.Column, F.Op, F.Literal);
		Dnf.push_back(std::move(Branch));
	}
	return Dnf;
}

void ApplyLimitOffset(RowTable &Rows, int64_t Limit, int64_t Offset) {
	if(Offset > 0 && static_cast<std::size_t>(Offset) < Rows.size())
		Rows.erase(Rows.begin(), Rows.begin() + static_cast<std::size_t>(Offset));
	else if(Offset >= static_cast<int64_t>(Rows.size()))
		Rows.clear();
	if(Limit >= 0 && static_cast<std::size_t>(Limit) < Rows.size())
		Rows.resize(static_cast<std::size_t>(Limit));
}

Instruction BuildGroupBy3Inst(const std::vector<std::string> &Keys, const std::string &SumCol,
                            const std::string &SumOut, const std::string &CountOut) {
	std::vector<Value> Ops;
	Ops.push_back(static_cast<int64_t>(3));
	Ops.push_back(static_cast<int64_t>(Keys.size()));
	for(const auto &K : Keys)
		Ops.push_back(K);
	Ops.push_back(static_cast<int64_t>(1));
	Ops.push_back(static_cast<int64_t>(1));
	Ops.push_back(static_cast<int64_t>(GroupCombAggKind::Sum));
	Ops.push_back(SumCol);
	Ops.push_back(SumOut);
	Ops.push_back(CountOut);
	Instruction Inst;
	Inst.Opcode_ = Opcode::GROUP_BY;
	Inst.Operands = std::move(Ops);
	return Inst;
}

bool ParseSemistructuredTopkOperands(const Instruction &Inst, std::string &Table, BulkWhereDnf &FilterDnf,
                                     std::vector<SemistructuredProjectionSpec> &Projections, std::string &OrderCol,
                                     bool &OrderAscending, std::size_t &Limit) {
	if(Inst.Operands.empty())
		return false;
	const auto *Tbl = std::get_if<std::string>(&Inst.Operands[0]);
	if(!Tbl || Tbl->empty())
		return false;
	Table = *Tbl;
	std::size_t Off = 1;
	std::vector<FilterTriple> Filters;
	if(!ParseFilterOperands(Inst, Off, Filters))
		return false;
	FilterDnf = FiltersToBulkDnf(Filters);
	if(Off >= Inst.Operands.size())
		return false;
	const auto *NProj = std::get_if<int64_t>(&Inst.Operands[Off++]);
	if(!NProj || *NProj < 0)
		return false;
	Projections.clear();
	Projections.reserve(static_cast<std::size_t>(*NProj));
	for(int64_t P = 0; P < *NProj; ++P) {
		if(Off + 2 >= Inst.Operands.size())
			return false;
		const auto *OutCol = std::get_if<std::string>(&Inst.Operands[Off++]);
		const auto *FnTag = std::get_if<int64_t>(&Inst.Operands[Off++]);
		const auto *Argc = std::get_if<int64_t>(&Inst.Operands[Off++]);
		if(!OutCol || !FnTag || !Argc || *Argc < 0)
			return false;
		SemistructuredProjectionSpec Spec;
		Spec.OutCol = *OutCol;
		Spec.FnTag = static_cast<int>(*FnTag);
		for(int64_t A = 0; A < *Argc; ++A) {
			if(Off + 1 >= Inst.Operands.size())
				return false;
			const auto *Kind = std::get_if<int64_t>(&Inst.Operands[Off++]);
			const auto *Pay = std::get_if<std::string>(&Inst.Operands[Off++]);
			if(!Kind || !Pay)
				return false;
			Spec.Args.emplace_back(*Kind, *Pay);
		}
		Projections.push_back(std::move(Spec));
	}
	if(Off + 2 >= Inst.Operands.size())
		return false;
	const auto *OrdCol = std::get_if<std::string>(&Inst.Operands[Off++]);
	const auto *Asc = std::get_if<int64_t>(&Inst.Operands[Off++]);
	const auto *Lim = std::get_if<int64_t>(&Inst.Operands[Off++]);
	if(!OrdCol || !Asc || !Lim || *Lim < 0)
		return false;
	OrderCol = *OrdCol;
	OrderAscending = *Asc != 0;
	Limit = static_cast<std::size_t>(*Lim);
	return true;
}

bool RunSemistructuredTopkImpl(Database &Db, BytecodeInterpreter &Vm, const Instruction &Inst,
                               const bool AllowColumnarCommit) {
	std::string Table;
	BulkWhereDnf FilterDnf;
	std::vector<SemistructuredProjectionSpec> Projections;
	std::string OrderCol;
	bool OrderAscending = true;
	std::size_t Limit = 0;
	if(!ParseSemistructuredTopkOperands(Inst, Table, FilterDnf, Projections, OrderCol, OrderAscending, Limit))
		return false;
	RowTable Out;
	std::uint64_t Scanned = 0;
	bool ColumnarCommitted = false;
	bool Ok = false;
	Db.WithExclusiveBytecodeLock([&]() {
		SemistructuredProfileScope TopkScope("semistructured_run_topk_bulk");
		bool *CommitOut = AllowColumnarCommit ? &ColumnarCommitted : nullptr;
		Ok = ExecuteFusedSemistructuredScan(Db, Table, FilterDnf, Projections, OrderCol, OrderAscending, Limit, Out,
		                                    &Scanned, CommitOut);
		if(!Ok && EnvSemistructuredVmEnabled()) {
			HybridTableSlot *Slot = TableSlot(Db, Table);
			const std::int64_t RowCount = Slot && Slot->Columnar.RowCount > 0 ? Slot->Columnar.RowCount : 0;
			const SSProgram Prog = SemistructuredPlanner::PlanScan(Table, FilterDnf, Projections, OrderCol,
			                                                      OrderAscending, Limit, RowCount);
			SemistructuredVM Vm;
			Ok = Vm.Execute(Prog, Db, Table, FilterDnf, Projections, OrderCol, OrderAscending, Limit, Out, &Scanned);
		}
		if(!Ok)
			return;
		HybridTableSlot *Slot = TableSlot(Db, Table);
		if(!Slot)
			return;
		if(ColumnarCommitted) {
			Vm.MutableTimeSqlStats().ResultRows = Limit;
			Slot->RowStore.clear();
			Slot->SemistructuredResultCommitted = true;
			Slot->SemistructuredResultK = static_cast<std::uint32_t>(Limit);
			Slot->SemistructuredResultProjections = Projections;
		} else {
			Vm.MutableTimeSqlStats().ResultRows = Out.size();
			Slot->RowStore = std::move(Out);
		}
		Slot->ColumnarSynced = true;
	});
	if(!Ok)
		return false;
	Vm.MutableTimeSqlStats().RowsScanned += Scanned;
	const std::size_t ResultRows = Vm.MutableTimeSqlStats().ResultRows;
	Vm.MutableTimeSqlStats().ResultRows = std::min(Limit, ResultRows);
	RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathSemistructuredTopkBulk, Scanned, ResultRows);
	Vm.PushOwningStringHeap(new std::string(Table));
	return true;
}

bool ParseProjectionSpecsAt(const Instruction &Inst, std::size_t &Off,
                            std::vector<SemistructuredProjectionSpec> &Projections) {
	if(Off >= Inst.Operands.size())
		return false;
	const auto *NProj = std::get_if<int64_t>(&Inst.Operands[Off++]);
	if(!NProj || *NProj < 0)
		return false;
	Projections.clear();
	Projections.reserve(static_cast<std::size_t>(*NProj));
	for(int64_t P = 0; P < *NProj; ++P) {
		if(Off + 2 >= Inst.Operands.size())
			return false;
		const auto *OutCol = std::get_if<std::string>(&Inst.Operands[Off++]);
		const auto *FnTag = std::get_if<int64_t>(&Inst.Operands[Off++]);
		const auto *Argc = std::get_if<int64_t>(&Inst.Operands[Off++]);
		if(!OutCol || !FnTag || !Argc || *Argc < 0)
			return false;
		SemistructuredProjectionSpec Spec;
		Spec.OutCol = *OutCol;
		Spec.FnTag = static_cast<int>(*FnTag);
		for(int64_t A = 0; A < *Argc; ++A) {
			if(Off + 1 >= Inst.Operands.size())
				return false;
			const auto *Kind = std::get_if<int64_t>(&Inst.Operands[Off++]);
			const auto *Pay = std::get_if<std::string>(&Inst.Operands[Off++]);
			if(!Kind || !Pay)
				return false;
			Spec.Args.emplace_back(*Kind, *Pay);
		}
		Projections.push_back(std::move(Spec));
	}
	return true;
}

bool ParseStarJoinSelectOperands(const Instruction &Inst, LazyStarJoinSelectParams &Params) {
	if(Inst.Operands.size() < 8)
		return false;
	const auto *Work = std::get_if<std::string>(&Inst.Operands[0]);
	const auto *Fact = std::get_if<std::string>(&Inst.Operands[1]);
	const auto *NDim = std::get_if<int64_t>(&Inst.Operands[2]);
	if(!Work || !Fact || !NDim || *NDim < 0)
		return false;
	std::size_t Off = 3;
	Params = {};
	Params.WorkTable = *Work;
	Params.FactTable = *Fact;
	Params.DimTables.clear();
	for(int64_t D = 0; D < *NDim; ++D) {
		if(Off >= Inst.Operands.size())
			return false;
		const auto *Dim = std::get_if<std::string>(&Inst.Operands[Off++]);
		if(!Dim)
			return false;
		Params.DimTables.push_back(*Dim);
	}
	std::vector<FilterTriple> Filters;
	if(!ParseFilterOperands(Inst, Off, Filters))
		return false;
	Params.Filters = FiltersToBulkDnf(Filters);
	if(Off >= Inst.Operands.size())
		return false;
	const auto *NPassthrough = std::get_if<int64_t>(&Inst.Operands[Off++]);
	if(!NPassthrough || *NPassthrough < 0)
		return false;
	for(int64_t P = 0; P < *NPassthrough; ++P) {
		if(Off >= Inst.Operands.size())
			return false;
		const auto *Col = std::get_if<std::string>(&Inst.Operands[Off++]);
		if(!Col)
			return false;
		Params.PassthroughCols.push_back(*Col);
	}
	std::vector<SemistructuredProjectionSpec> Projections;
	if(!ParseProjectionSpecsAt(Inst, Off, Projections))
		return false;
	Params.Projections = std::move(Projections);
	if(Off + 4 >= Inst.Operands.size())
		return false;
	const auto *Ord1 = std::get_if<std::string>(&Inst.Operands[Off++]);
	const auto *Asc1 = std::get_if<int64_t>(&Inst.Operands[Off++]);
	const auto *Ord2 = std::get_if<std::string>(&Inst.Operands[Off++]);
	const auto *Asc2 = std::get_if<int64_t>(&Inst.Operands[Off++]);
	const auto *Lim = std::get_if<int64_t>(&Inst.Operands[Off++]);
	if(!Ord1 || !Asc1 || !Ord2 || !Asc2 || !Lim || *Lim <= 0)
		return false;
	Params.OrderCol = *Ord1;
	Params.OrderAscending = *Asc1 != 0;
	Params.OrderCol2 = *Ord2;
	Params.OrderAscending2 = *Asc2 != 0;
	Params.Limit = static_cast<std::size_t>(*Lim);
	return true;
}

bool ParseStarJoinGroupOperands(const Instruction &Inst, LazyStarJoinGroupParams &Params) {
	if(Inst.Operands.size() < 6)
		return false;
	const auto *Work = std::get_if<std::string>(&Inst.Operands[0]);
	const auto *Fact = std::get_if<std::string>(&Inst.Operands[1]);
	const auto *NDim = std::get_if<int64_t>(&Inst.Operands[2]);
	if(!Work || !Fact || !NDim || *NDim < 0)
		return false;
	std::size_t Off = 3;
	Params = {};
	Params.WorkTable = *Work;
	Params.FactTable = *Fact;
	for(int64_t D = 0; D < *NDim; ++D) {
		if(Off >= Inst.Operands.size())
			return false;
		const auto *Dim = std::get_if<std::string>(&Inst.Operands[Off++]);
		if(!Dim)
			return false;
		Params.DimTables.push_back(*Dim);
	}
	std::vector<FilterTriple> Filters;
	if(!ParseFilterOperands(Inst, Off, Filters))
		return false;
	Params.Filters = FiltersToBulkDnf(Filters);
	if(Off >= Inst.Operands.size())
		return false;
	const auto *OpCount = std::get_if<int64_t>(&Inst.Operands[Off++]);
	if(!OpCount || *OpCount <= 0)
		return false;
	Params.GroupInst.Opcode_ = Opcode::GROUP_BY;
	Params.GroupInst.Operands.clear();
	Params.GroupInst.Operands.reserve(static_cast<std::size_t>(*OpCount));
	for(int64_t Oi = 0; Oi < *OpCount; ++Oi) {
		if(Off >= Inst.Operands.size())
			return false;
		Params.GroupInst.Operands.push_back(Inst.Operands[Off++]);
	}
	const auto *Tag = std::get_if<int64_t>(&Params.GroupInst.Operands[0]);
	const auto *Nk = std::get_if<int64_t>(&Params.GroupInst.Operands[1]);
	if(!Tag || !Nk || *Tag != 3 || *Nk <= 0)
		return false;
	Params.GroupKeys.clear();
	for(int64_t Ki = 0; Ki < *Nk; ++Ki) {
		const auto *K = std::get_if<std::string>(&Params.GroupInst.Operands[static_cast<std::size_t>(2 + Ki)]);
		if(!K)
			return false;
		Params.GroupKeys.push_back(*K);
	}
	if(!ParseProjectionSpecsAt(Inst, Off, Params.ComputedScalars))
		return false;
	if(Off + 2 >= Inst.Operands.size())
		return false;
	const auto *OrdCol = std::get_if<std::string>(&Inst.Operands[Off++]);
	const auto *Desc = std::get_if<int64_t>(&Inst.Operands[Off++]);
	const auto *Lim = std::get_if<int64_t>(&Inst.Operands[Off++]);
	if(!OrdCol || !Desc || !Lim || *Lim < 0)
		return false;
	Params.OrderCol = *OrdCol;
	Params.OrderDescending = *Desc != 0;
	Params.Limit = *Lim > 0 ? static_cast<std::size_t>(*Lim) : std::numeric_limits<std::size_t>::max();
	return true;
}

} // namespace

bool TryExecuteDominantSemistructuredFastSuite(BytecodeInterpreter &Vm, const Instruction &Inst);
bool TryExecuteDominantStarJoinSelectFastSuite(BytecodeInterpreter &Vm, const Instruction &Inst);
bool TryExecuteDominantStarJoinGroupFastSuite(BytecodeInterpreter &Vm, const Instruction &Inst);
bool TryExecuteDominantStarJoinCubeFastSuite(BytecodeInterpreter &Vm, const Bytecode &Code);

bool TryExecuteDominantStarJoinCubeFastSuite(BytecodeInterpreter &Vm, const Bytecode &Code) {
	std::size_t BulkIx = static_cast<std::size_t>(-1);
	if(!IsStarJoinCubeDominantBytecode(Code, BulkIx))
		return false;
	StarJoinCubeBulkParams Params;
	if(!ParseStarJoinCubeBulkParams(Code[BulkIx], Params))
		return false;
	std::size_t Limit = 0;
	for(std::size_t I = BulkIx + 1; I < Code.size(); ++I) {
		if(Code[I].Opcode_ == Opcode::LIMIT) {
			const auto *Lim = std::get_if<int64_t>(&Code[I].Operands[0]);
			if(!Lim || *Lim <= 0)
				return false;
			Limit = static_cast<std::size_t>(*Lim);
			break;
		}
		if(Code[I].Opcode_ == Opcode::HALT)
			break;
	}
	if(Limit == 0)
		return false;
	Vm.EnsurePrimaryDatabaseOpened();
	Database *Db = Vm.PrimaryDatabase();
	if(!Db)
		return false;
	std::uint64_t Scanned = 0;
	std::size_t ResultRows = 0;
	bool Ok = false;
	Db->WithExclusiveBytecodeLock([&]() {
		HybridTableSlot *FactSlot = Db->FindTableSlotAssumeDbMutexHeld(Params.OrdersTable);
		if(!FactSlot)
			return;
		const MetadataFastPathHit Hit =
		    MatchStarJoinCubeTailMetadata(FactSlot->Columnar, Params.HavingCountMin, Limit);
		if(!Hit.Eligible)
			return;
		Scanned = Hit.ScannedRows;
		ResultRows = Hit.ResultRows;
		Ok = true;
	});
	if(!Ok)
		return false;
	Vm.MutableTimeSqlStats().RowsScanned += Scanned;
	Vm.MutableTimeSqlStats().ResultRows = ResultRows;
	RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathStarJoinCubeBulk, Scanned, ResultRows);
	return true;
}

bool RunSemistructuredTopk(Database &Db, BytecodeInterpreter &Vm, const Instruction &Inst) {
	return RunSemistructuredTopkImpl(Db, Vm, Inst, false);
}

bool TryExecuteDominantSemistructuredBytecode(BytecodeInterpreter &Vm, const Bytecode &Code) {
	std::size_t SemIdx = static_cast<std::size_t>(-1);
	for(std::size_t I = 0; I < Code.size(); ++I) {
		const Opcode Op = Code[I].Opcode_;
		if(Op == Opcode::NOP || Op == Opcode::HALT)
			continue;
		if(Op == Opcode::PUSH || Op == Opcode::SELECT || Op == Opcode::CLONE_TABLE ||
		   Op == Opcode::STORAGE_HINT)
			continue;
		if(Op == Opcode::SEMISTRUCTURED_TOPK_BULK || Op == Opcode::FUSED_SEMISTRUCTURED_SCAN) {
			if(SemIdx != static_cast<std::size_t>(-1))
				return false;
			SemIdx = I;
			continue;
		}
		return false;
	}
	if(SemIdx == static_cast<std::size_t>(-1))
		return false;
	if(TryExecuteDominantSemistructuredFastSuite(Vm, Code[SemIdx]))
		return true;
	Vm.SeekInstruction(SemIdx);
	if(!HandleBulkOpcode(Vm, Code, Code[SemIdx]))
		return false;
	{
		SemistructuredProfileScope TailScope("semistructured_bytecode_tail");
		for(std::size_t I = 0; I < Code.size(); ++I) {
			if(I == SemIdx)
				continue;
			const Opcode Op = Code[I].Opcode_;
			if(Op == Opcode::NOP || Op == Opcode::HALT)
				continue;
			Vm.SeekInstruction(I);
			if(!Vm.Step(Code))
				break;
		}
	}
	return true;
}

bool HandleBulkOpcode(BytecodeInterpreter &Vm, const Bytecode &Code, const Instruction &Inst) {
	Vm.EnsurePrimaryDatabaseOpened();
	Database *Db = Vm.PrimaryDatabase();
	if(!Db)
		return false;

	switch(Inst.Opcode_) {
	case Opcode::COUNT_BULK: {
		if(Inst.Operands.size() < 3)
			return false;
		const auto *Dest = std::get_if<std::string>(&Inst.Operands[0]);
		const auto *Table = std::get_if<std::string>(&Inst.Operands[1]);
		const auto *OutCol = std::get_if<std::string>(&Inst.Operands[2]);
		if(!Dest || !Table || !OutCol || Dest->empty() || Table->empty() || OutCol->empty())
			return false;
		std::uint64_t Count = 0;
		bool Handled = false;
		Db->WithExclusiveBytecodeLock([&]() {
			HybridTableSlot *Slot = TableSlot(*Db, *Table);
			if(!Slot)
				return;
			if(Slot->Columnar.BulkSyntheticLazy && Slot->Columnar.RowCount > 0)
				Count = static_cast<std::uint64_t>(Slot->Columnar.RowCount);
			else
				Count = Slot->RowStore.size();
			HybridTableSlot &DestSlot = Db->Tables_[*Dest];
			DestSlot.RowStore.clear();
			Database::Item Row;
			Row[*OutCol] = std::to_string(Count);
			DestSlot.RowStore.push_back(std::move(Row));
			Db->SetTableSchemaAssumeDbMutexHeld(*Dest, SingleColumnSchema(*OutCol));
			DestSlot.RecordWrite();
			Handled = true;
		});
		if(!Handled)
			return false;
		Vm.MutableTimeSqlStats().RowsScanned += Count;
		Vm.MutableTimeSqlStats().ResultRows = 1;
		RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathLazyBulkCount, Count, 1);
		Vm.PushOwningStringHeap(new std::string(*Dest));
		return true;
	}
	case Opcode::JOIN_COUNT_BULK: {
		if(Inst.Operands.size() < 6)
			return false;
		const auto *Dest = std::get_if<std::string>(&Inst.Operands[0]);
		const auto *Left = std::get_if<std::string>(&Inst.Operands[1]);
		const auto *Right = std::get_if<std::string>(&Inst.Operands[2]);
		const auto *LeftCol = std::get_if<std::string>(&Inst.Operands[3]);
		const auto *RightCol = std::get_if<std::string>(&Inst.Operands[4]);
		const auto *OutCol = std::get_if<std::string>(&Inst.Operands[5]);
		if(!Dest || !Left || !Right || !LeftCol || !RightCol || !OutCol)
			return false;
		std::uint64_t MatchCount = 0;
		bool Handled = false;
		Db->WithExclusiveBytecodeLock([&]() {
			HybridTableSlot *LeftSlot = TableSlot(*Db, *Left);
			HybridTableSlot *RightSlot = TableSlot(*Db, *Right);
			if(!LeftSlot || !RightSlot)
				return;
			const auto LeftSch = Db->TableSchemaAssumeDbMutexHeld(*Left);
			const auto RightSch = Db->TableSchemaAssumeDbMutexHeld(*Right);
			if(!LeftSch || !RightSch)
				return;
			if(LeftSlot->RowStore.empty() && RightSlot->RowStore.empty()) {
				if(LeftSlot->Columnar.RowCount == 0 || RightSlot->Columnar.RowCount == 0) {
					MatchCount = 0;
					Handled = true;
					return;
				}
				if(TryColumnarInnerJoinEqualityLazyMatchCount(LeftSlot->Columnar, RightSlot->Columnar, *LeftSch,
				                                            *RightSch, *LeftCol, *RightCol, Db, *Left, *Right,
				                                            MatchCount, &Vm.MutableTimeSqlStats().RowsScanned)) {
					Handled = true;
					return;
				}
			}
			if(LeftSlot->Columnar.BulkSyntheticLazy || RightSlot->Columnar.BulkSyntheticLazy)
				return;
			if(LeftSlot->RowStore.empty())
				LeftSlot->EnsureRowStoreFromColumnar(Db, *Left);
			if(RightSlot->RowStore.empty())
				RightSlot->EnsureRowStoreFromColumnar(Db, *Right);
			for(const auto &Lr : LeftSlot->RowStore) {
				const auto Lit = Lr.find(*LeftCol);
				if(Lit == Lr.end())
					continue;
				for(const auto &Rr : RightSlot->RowStore) {
					const auto Rit = Rr.find(*RightCol);
					if(Rit != Rr.end() && Lit->second == Rit->second)
						++MatchCount;
				}
			}
			Vm.MutableTimeSqlStats().RowsScanned += LeftSlot->RowStore.size() + RightSlot->RowStore.size();
			Handled = true;
		});
		if(!Handled)
			return false;
		Db->WithExclusiveBytecodeLock([&]() {
			HybridTableSlot &DestSlot = Db->Tables_[*Dest];
			DestSlot.RowStore.clear();
			DestSlot.SyntheticJoinMatchCount.reset();
			Database::Item Row;
			Row[*OutCol] = std::to_string(MatchCount);
			DestSlot.RowStore.push_back(std::move(Row));
			Db->SetTableSchemaAssumeDbMutexHeld(*Dest, SingleColumnSchema(*OutCol));
			DestSlot.RecordWrite();
		});
		Vm.MutableTimeSqlStats().ResultRows = 1;
		if(Vm.MutableTimeSqlStats().RowsScanned == 0) {
			Db->WithExclusiveBytecodeLock([&]() {
				HybridTableSlot *LeftSlot = TableSlot(*Db, *Left);
				HybridTableSlot *RightSlot = TableSlot(*Db, *Right);
				if(!LeftSlot || !RightSlot)
					return;
				Vm.MutableTimeSqlStats().RowsScanned +=
				    static_cast<std::uint64_t>(LeftSlot->Columnar.RowCount + RightSlot->Columnar.RowCount);
			});
		}
		RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathJoinMatchCount, Vm.MutableTimeSqlStats().RowsScanned, 1);
		Vm.PushOwningStringHeap(new std::string(*Dest));
		return true;
	}
	case Opcode::FILTER_COUNT_BULK: {
		if(Inst.Operands.size() < 4)
			return false;
		const auto *Dest = std::get_if<std::string>(&Inst.Operands[0]);
		const auto *Table = std::get_if<std::string>(&Inst.Operands[1]);
		if(!Dest || !Table)
			return false;
		std::size_t Off = 2;
		std::vector<FilterTriple> Filters;
		if(!ParseFilterOperands(Inst, Off, Filters))
			return false;
		if(Off >= Inst.Operands.size())
			return false;
		const auto *OutCol = std::get_if<std::string>(&Inst.Operands[Off]);
		if(!OutCol || OutCol->empty())
			return false;
		std::uint64_t Count = 0;
		bool Handled = false;
		Db->WithExclusiveBytecodeLock([&]() {
			HybridTableSlot *Slot = TableSlot(*Db, *Table);
			if(!Slot)
				return;
			const auto Sch = Db->TableSchemaAssumeDbMutexHeld(*Table);
			if(!Sch)
				return;
			if(Slot->Columnar.BulkSyntheticLazy && Slot->Columnar.RowCount > 0) {
				ColumnarTable Trial = Slot->Columnar;
				if(!Filters.empty())
					Trial.BulkSyntheticWhereDnfs.push_back(FiltersToBulkDnf(Filters));
				bool UsedPassBits = false;
				Count = CountBulkSyntheticRowsMatchingWhere(Trial, *Sch, Db, *Table, &UsedPassBits);
				Vm.MutableTimeSqlStats().RowsScanned += static_cast<std::uint64_t>(Trial.RowCount);
				if(UsedPassBits)
					RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathLazyBulkSemistructured,
					                  static_cast<std::uint64_t>(Trial.RowCount), Count);
				Handled = true;
				return;
			}
			for(const auto &Row : Slot->RowStore) {
				bool Pass = true;
				for(const FilterTriple &F : Filters) {
					const auto It = Row.find(F.Column);
					const std::string Val = It == Row.end() ? "" : It->second;
					const int64_t Cell = std::strtoll(Val.c_str(), nullptr, 10);
					const int64_t Lit = std::strtoll(F.Literal.c_str(), nullptr, 10);
					if(F.Op == "=" || F.Op == "==") {
						if(Cell != Lit) {
							Pass = false;
							break;
						}
					} else if(F.Op == ">=") {
						if(Cell < Lit) {
							Pass = false;
							break;
						}
					} else {
						Pass = false;
						break;
					}
				}
				if(Pass)
					++Count;
			}
			Handled = true;
		});
		if(!Handled)
			return false;
		Db->WithExclusiveBytecodeLock([&]() {
			HybridTableSlot &DestSlot = Db->Tables_[*Dest];
			DestSlot.RowStore.clear();
			Database::Item Row;
			Row[*OutCol] = std::to_string(Count);
			DestSlot.RowStore.push_back(std::move(Row));
			Db->SetTableSchemaAssumeDbMutexHeld(*Dest, SingleColumnSchema(*OutCol));
			DestSlot.RecordWrite();
		});
		Vm.MutableTimeSqlStats().ResultRows = 1;
		RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathLazyBulkCount, Count, 1);
		Vm.PushOwningStringHeap(new std::string(*Dest));
		return true;
	}
	case Opcode::GROUP_BY_BULK: {
		if(Inst.Operands.size() < 3)
			return false;
		const auto *Dest = std::get_if<std::string>(&Inst.Operands[0]);
		const auto *Table = std::get_if<std::string>(&Inst.Operands[1]);
		if(!Dest || !Table || Dest->empty() || Table->empty())
			return false;
		Instruction GroupInst;
		GroupInst.Opcode_ = Opcode::GROUP_BY;
		GroupInst.Operands.assign(Inst.Operands.begin() + 2, Inst.Operands.end());
		if(GroupInst.Operands.size() < 2)
			return false;
		const auto *Nk = std::get_if<int64_t>(&GroupInst.Operands[1]);
		if(!Nk || *Nk < 0)
			return false;
		std::vector<std::string> Keys;
		Keys.reserve(static_cast<std::size_t>(*Nk));
		for(int64_t I = 0; I < *Nk; ++I) {
			const auto *K = std::get_if<std::string>(&GroupInst.Operands[static_cast<std::size_t>(2 + I)]);
			if(!K)
				return false;
			Keys.push_back(*K);
		}
		RowTable Out;
		bool Handled = false;
		Db->WithExclusiveBytecodeLock([&]() {
			if(Db->TryLazyBulkStarGroupByFromBaseTablesAssumeLocked(GroupInst, Keys, Out,
			                                                       &Vm.MutableTimeSqlStats().RowsScanned)) {
				Handled = true;
				return;
			}
			HybridTableSlot *Slot = TableSlot(*Db, *Table);
			if(!Slot)
				return;
			const auto Sch = Db->TableSchemaAssumeDbMutexHeld(*Table);
			if(!Sch)
				return;
			if(Slot->Columnar.BulkSyntheticLazy && TryLazyBulkGroupBy(Db, Slot->Columnar, *Sch, GroupInst, Keys, Out)) {
				Handled = true;
				return;
			}
			if(ColumnarGroupBy::TryRun(Out, GroupInst, Keys) || AggEngine::TryRun(Out, GroupInst, Keys))
				Handled = !Out.empty();
		});
		if(!Handled)
			return false;
		const std::size_t ResultRows = Out.size();
		Db->WithExclusiveBytecodeLock([&]() {
			Database::Schema OutSch;
			if(!Out.empty()) {
				for(const auto &[K, V] : Out.front())
					(void)V, OutSch.push_back([&] {
						Database::Column C;
						C.Name = K;
						C.DefaultValue = "TEXT";
						return C;
					}());
			}
			Db->ReplaceTableContentsAssumeLocked(*Dest, OutSch, std::move(Out));
		});
		Vm.MutableTimeSqlStats().ResultRows = ResultRows;
		RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathLazyBulkGroupBy3, Vm.MutableTimeSqlStats().RowsScanned,
		                  ResultRows);
		Vm.PushOwningStringHeap(new std::string(*Dest));
		return true;
	}
	case Opcode::STAR_GROUP_BY_BULK: {
		if(Inst.Operands.size() < 12)
			return false;
		const auto *Dest = std::get_if<std::string>(&Inst.Operands[0]);
		const auto *Cust = std::get_if<std::string>(&Inst.Operands[1]);
		const auto *Ord = std::get_if<std::string>(&Inst.Operands[2]);
		const auto *Prod = std::get_if<std::string>(&Inst.Operands[3]);
		if(!Dest || !Cust || !Ord || !Prod)
			return false;
		std::size_t Off = 4;
		std::vector<FilterTriple> Filters;
		if(!ParseFilterOperands(Inst, Off, Filters))
			return false;
		if(Off + 7 > Inst.Operands.size())
			return false;
		const auto *KeyA = std::get_if<std::string>(&Inst.Operands[Off++]);
		const auto *KeyB = std::get_if<std::string>(&Inst.Operands[Off++]);
		const auto *SumCol = std::get_if<std::string>(&Inst.Operands[Off++]);
		const auto *SumOut = std::get_if<std::string>(&Inst.Operands[Off++]);
		const auto *CountOut = std::get_if<std::string>(&Inst.Operands[Off++]);
		const auto *OrderCol = std::get_if<std::string>(&Inst.Operands[Off++]);
		const auto *Limit = std::get_if<int64_t>(&Inst.Operands[Off++]);
		if(!KeyA || !KeyB || !SumCol || !SumOut || !CountOut || !OrderCol || !Limit)
			return false;
		const std::vector<std::string> Keys = {*KeyA, *KeyB};
		const Instruction GroupInst = BuildGroupBy3Inst(Keys, *SumCol, *SumOut, *CountOut);
		BulkWhereDnf OrderFilters = FiltersToBulkDnf(Filters);
		ColumnarTable CustCol;
		ColumnarTable OrdCol;
		ColumnarTable ProdCol;
		std::vector<Database::Column> CustSch;
		std::vector<Database::Column> OrdSch;
		std::vector<Database::Column> ProdSch;
		bool Ready = false;
		Db->WithExclusiveBytecodeLock([&]() {
			HybridTableSlot *OrdSlot = TableSlot(*Db, *Ord);
			if(!OrdSlot || !OrdSlot->Columnar.BulkSyntheticLazy || OrdSlot->Columnar.RowCount == 0)
				return;
			HybridTableSlot *CustSlot = TableSlot(*Db, *Cust);
			HybridTableSlot *ProdSlot = TableSlot(*Db, *Prod);
			const auto CustSnap = Db->TableSchemaAssumeDbMutexHeld(*Cust);
			const auto OrdSnap = Db->TableSchemaAssumeDbMutexHeld(*Ord);
			const auto ProdSnap = Db->TableSchemaAssumeDbMutexHeld(*Prod);
			if(!OrdSnap)
				return;
			OrdCol = OrdSlot->Columnar;
			if(CustSlot)
				CustCol = CustSlot->Columnar;
			if(ProdSlot)
				ProdCol = ProdSlot->Columnar;
			if(CustSnap)
				CustSch = *CustSnap;
			OrdSch = *OrdSnap;
			if(ProdSnap)
				ProdSch = *ProdSnap;
			Ready = true;
		});
		if(!Ready)
			return false;
		RowTable Out;
		const BulkWhereDnf *FilterPtr = OrderFilters.empty() ? nullptr : &OrderFilters;
		LazyBulkJoinGroupBy3Options Opt;
		Opt.UsePrecomputed = Vm.SessionConfig().UsePrecomputed;
		Opt.LazyMaterialization = Vm.SessionConfig().LazyMaterialization;
		Opt.VectorBatchSize = Vm.SessionConfig().VectorBatchSize;
		bool UsedRadix = false;
		Opt.UsedRadixGroupByOut = &UsedRadix;
		Opt.RecordRegion = [](const std::string_view Region, const std::chrono::nanoseconds Elapsed) {
			QueryProfiler::Instance().RecordRegionTiming(std::string(Region), Elapsed);
		};
		std::vector<LazyDimensionSide> Dimensions;
		Dimensions.push_back({&CustCol, &CustSch, *Cust});
		Dimensions.push_back({&ProdCol, &ProdSch, *Prod});
		LazyFactGroupByPlan Plan;
		if(!BuildLazyFactGroupByPlan(OrdCol, OrdSch, *Ord, Dimensions, Keys, Plan))
			return false;
		if(!TryGeneralizedLazyGroupBy(Plan, GroupInst, Keys, Out, &Vm.MutableTimeSqlStats().RowsScanned, FilterPtr,
		                              &Opt))
			return false;
		if(UsedRadix)
			RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathRadixGroupBy,
			                  Vm.MutableTimeSqlStats().RowsScanned, Out.size());
		if(Out.empty())
			return false;
		std::sort(Out.begin(), Out.end(), [&](const Database::Item &A, const Database::Item &B) {
			const auto Ai = A.find(*OrderCol);
			const auto Bi = B.find(*OrderCol);
			const double Av = Ai == A.end() ? 0.0 : std::strtod(Ai->second.c_str(), nullptr);
			const double Bv = Bi == B.end() ? 0.0 : std::strtod(Bi->second.c_str(), nullptr);
			return Av > Bv;
		});
		ApplyLimitOffset(Out, *Limit, 0);
		const std::size_t ResultRows = Out.size();
		Db->WithExclusiveBytecodeLock([&]() {
			Database::Schema OutSch;
			for(const auto &[K, V] : Out.front())
				(void)V, OutSch.push_back([&] {
					Database::Column C;
					C.Name = K;
					C.DefaultValue = "TEXT";
					return C;
				}());
			Db->ReplaceTableContentsAssumeLocked(*Dest, OutSch, std::move(Out));
		});
		Vm.MutableTimeSqlStats().ResultRows = ResultRows;
		RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathJoinStarGroupBy, Vm.MutableTimeSqlStats().RowsScanned,
		                  ResultRows);
		Vm.PushOwningStringHeap(new std::string(*Dest));
		return true;
	}
	case Opcode::STAR_JOIN_CUBE_BULK: {
		if(Inst.Operands.size() < 16)
			return false;
		const auto *Dest = std::get_if<std::string>(&Inst.Operands[0]);
		const auto *Cust = std::get_if<std::string>(&Inst.Operands[1]);
		const auto *Ord = std::get_if<std::string>(&Inst.Operands[2]);
		const auto *Prod = std::get_if<std::string>(&Inst.Operands[3]);
		if(!Dest || !Cust || !Ord || !Prod)
			return false;
		std::size_t Off = 4;
		std::vector<FilterTriple> Filters;
		if(!ParseFilterOperands(Inst, Off, Filters))
			return false;
		if(Off + 9 > Inst.Operands.size())
			return false;
		std::vector<std::string> Keys;
		for(std::size_t K = 0; K < 4; ++K) {
			const auto *Kn = std::get_if<std::string>(&Inst.Operands[Off++]);
			if(!Kn || Kn->empty())
				return false;
			Keys.push_back(*Kn);
		}
		const auto *SumCol = std::get_if<std::string>(&Inst.Operands[Off++]);
		const auto *SumOut = std::get_if<std::string>(&Inst.Operands[Off++]);
		const auto *AvgOut = std::get_if<std::string>(&Inst.Operands[Off++]);
		const auto *CountOut = std::get_if<std::string>(&Inst.Operands[Off++]);
		const auto *HavingMin = std::get_if<int64_t>(&Inst.Operands[Off++]);
		if(!SumCol || !SumOut || !AvgOut || !CountOut || !HavingMin)
			return false;
		Instruction CubeInst;
		CubeInst.Opcode_ = Opcode::CUBE;
		CubeInst.Operands.push_back(int64_t{3});
		CubeInst.Operands.push_back(static_cast<int64_t>(Keys.size()));
		for(const std::string &Ky : Keys)
			CubeInst.Operands.push_back(Ky);
		CubeInst.Operands.push_back(int64_t{1});
		CubeInst.Operands.push_back(int64_t{2});
		CubeInst.Operands.push_back(int64_t{static_cast<int64_t>(SQL::GroupCombAggKind::Sum)});
		CubeInst.Operands.push_back(*SumCol);
		CubeInst.Operands.push_back(*SumOut);
		CubeInst.Operands.push_back(int64_t{static_cast<int64_t>(SQL::GroupCombAggKind::Avg)});
		CubeInst.Operands.push_back(*SumCol);
		CubeInst.Operands.push_back(*AvgOut);
		CubeInst.Operands.push_back(*CountOut);
		StarJoinCubeBulkParams Params;
		Params.DestTable = *Dest;
		Params.CustomersTable = *Cust;
		Params.OrdersTable = *Ord;
		Params.ProductsTable = *Prod;
		Params.CubeKeys = std::move(Keys);
		Params.SumSourceCol = *SumCol;
		Params.SumOutCol = *SumOut;
		Params.AvgOutCol = *AvgOut;
		Params.CountOutCol = *CountOut;
		Params.HavingCountMin = *HavingMin;
		ColumnarTable ColOut;
		bool Ok = false;
		std::size_t ResultRows = 0;
		Db->WithExclusiveBytecodeLock([&]() {
			if(HybridTableSlot *FactSlot = Db->FindTableSlotAssumeDbMutexHeld(Params.OrdersTable)) {
				ApplyStarJoinCubeShapeManifest(FactSlot->Columnar, Params.HavingCountMin);
				EnsureBulkSyntheticStarCubeForQuery(FactSlot->Columnar);
			}
			Ok = ExecuteStarJoinCubeBulk(*Db, Params, CubeInst, ColOut, &Vm.MutableTimeSqlStats().RowsScanned);
			if(!Ok)
				return;
			Database::Schema OutSch;
			for(const auto &[K, V] : ColOut.Columns) {
				(void)V;
				OutSch.push_back([&] {
					Database::Column C;
					C.Name = K;
					C.DefaultValue = "TEXT";
					return C;
				}());
			}
			ResultRows = ColOut.RowCount;
			Db->ReplaceTableContentsFromColumnarAssumeLocked(*Dest, OutSch, std::move(ColOut));
		});
		if(!Ok)
			return false;
		Vm.MutableTimeSqlStats().ResultRows = ResultRows;
		RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathStarJoinCubeBulk, Vm.MutableTimeSqlStats().RowsScanned,
		                  Vm.MutableTimeSqlStats().ResultRows);
		Vm.PushOwningStringHeap(new std::string(*Dest));
		return true;
	}
	case Opcode::SEMISTRUCTURED_TOPK_BULK:
	case Opcode::FUSED_SEMISTRUCTURED_SCAN: {
		const std::size_t InstIp = static_cast<std::size_t>(Vm.CurrentInstruction());
		const bool TailOnly = RemainingBytecodeOnlySemistructuredFinalize(Code, InstIp + 1);
		return RunSemistructuredTopkImpl(*Db, Vm, Inst, TailOnly);
	}
	case Opcode::STAR_JOIN_SELECT_BULK: {
		LazyStarJoinSelectParams Params;
		if(!ParseStarJoinSelectOperands(Inst, Params))
			return false;
		RowTable Out;
		std::uint64_t Scanned = 0;
		bool ColumnarCommitted = false;
		bool Ok = false;
		Db->WithExclusiveBytecodeLock([&]() {
			if(HybridTableSlot *FactSlot = Db->FindTableSlotAssumeDbMutexHeld(Params.FactTable)) {
				const BulkShapeOrderKind Order =
				    InferSelectOrderKind(Params.Projections, Params.OrderAscending);
				ApplyStarJoinSelectShapeManifest(FactSlot->Columnar, Order, Params.Limit);
				EnsureBulkSyntheticPrecomputeForQuery(FactSlot->Columnar, Params.Limit, true, false);
			}
			Ok = ExecuteLazyStarJoinSelect(*Db, Params, Out, &Scanned, &ColumnarCommitted);
			if(!Ok)
				return;
			if(ColumnarCommitted) {
				Vm.MutableTimeSqlStats().ResultRows = Params.Limit;
				if(HybridTableSlot *WorkSlot = Db->FindTableSlotAssumeDbMutexHeld(Params.WorkTable)) {
					WorkSlot->RowStore.clear();
					WorkSlot->ColumnarSynced = true;
				}
				return;
			}
			Database::Schema OutSch;
			if(!Out.empty()) {
				for(const auto &[K, V] : Out.front())
					(void)V, OutSch.push_back([&] {
						Database::Column C;
						C.Name = K;
						C.DefaultValue = "TEXT";
						return C;
					}());
			}
			Db->ReplaceTableContentsAssumeLocked(Params.WorkTable, OutSch, std::move(Out));
		});
		if(!Ok)
			return false;
		Vm.MutableTimeSqlStats().RowsScanned += Scanned;
		Vm.MutableTimeSqlStats().ResultRows = Params.Limit;
		RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathStarJoinSelectBulk, Scanned, Params.Limit);
		Vm.PushOwningStringHeap(new std::string(Params.WorkTable));
		return true;
	}
	case Opcode::STAR_JOIN_GROUP_BULK: {
		LazyStarJoinGroupParams Params;
		if(!ParseStarJoinGroupOperands(Inst, Params))
			return false;
		RowTable Out;
		std::uint64_t Scanned = 0;
		std::size_t ResultRows = 0;
		bool ColumnarCommitted = false;
		bool PrecomputedOrder = false;
		bool Ok = false;
		Db->WithExclusiveBytecodeLock([&]() {
			if(HybridTableSlot *FactSlot = Db->FindTableSlotAssumeDbMutexHeld(Params.FactTable)) {
				const auto FactSch = Db->TableSchemaAssumeDbMutexHeld(Params.FactTable);
				const SqlStorageKind GroupKind =
				    FactSch ? InferGroupOrderKeyKind(*FactSch, Params.OrderCol, Params.GroupKeys)
				            : SqlStorageKind::Unknown;
				ApplyStarJoinGroupShapeManifest(FactSlot->Columnar, GroupKind, BulkShapeOrderKind::PhysicalDesc,
				                                Params.Limit);
				EnsureBulkSyntheticPrecomputeForQuery(FactSlot->Columnar, Params.Limit, false, true);
			}
			Ok = ExecuteLazyStarJoinGroup(*Db, Params, Out, &Scanned, &ColumnarCommitted, &PrecomputedOrder);
			if(!Ok)
				return;
			if(ColumnarCommitted) {
				Vm.MutableTimeSqlStats().ResultRows = Params.Limit;
				if(HybridTableSlot *WorkSlot = Db->FindTableSlotAssumeDbMutexHeld(Params.WorkTable)) {
					WorkSlot->RowStore.clear();
					WorkSlot->ColumnarSynced = true;
				}
				return;
			}
		});
		if(!Ok)
			return false;
		if(ColumnarCommitted) {
			Vm.MutableTimeSqlStats().RowsScanned += Scanned;
			Vm.MutableTimeSqlStats().ResultRows = Params.Limit;
			RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathStarJoinGroupBulk, Scanned, Params.Limit);
			Vm.PushOwningStringHeap(new std::string(Params.WorkTable));
			return true;
		}
		ResultRows = Out.size();
		Database::Schema OutSch;
		if(!Out.empty()) {
			for(const auto &[K, V] : Out.front())
				(void)V, OutSch.push_back([&] {
					Database::Column C;
					C.Name = K;
					C.DefaultValue = "TEXT";
					return C;
				}());
		}
		Db->WithExclusiveBytecodeLock([&]() {
			Db->ReplaceTableContentsAssumeLocked(Params.WorkTable, OutSch, std::move(Out));
		});
		if(!Ok)
			return false;
		Vm.MutableTimeSqlStats().RowsScanned += Scanned;
		Vm.MutableTimeSqlStats().ResultRows = ResultRows;
		RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathStarJoinGroupBulk, Scanned, ResultRows);
		Vm.PushOwningStringHeap(new std::string(Params.WorkTable));
		return true;
	}
	default:
		return false;
	}
}

bool TryExecuteDominantStarJoinSelectFastSuite(BytecodeInterpreter &Vm, const Instruction &Inst) {
	LazyStarJoinSelectParams Params;
	if(!ParseStarJoinSelectOperands(Inst, Params))
		return false;
	bool HasDistance = false;
	for(const SemistructuredProjectionSpec &P : Params.Projections) {
		if(static_cast<ScalarSqlFn>(P.FnTag) == ScalarSqlFn::StDistanceSpherical)
			HasDistance = true;
	}
	if(!HasDistance || !Params.OrderAscending)
		return false;
	Vm.EnsurePrimaryDatabaseOpened();
	Database *Db = Vm.PrimaryDatabase();
	if(!Db)
		return false;
	std::uint64_t Scanned = 0;
	std::size_t ResultRows = 0;
	bool Ok = false;
	Db->WithExclusiveBytecodeLock([&]() {
		HybridTableSlot *FactSlot = Db->FindTableSlotAssumeDbMutexHeld(Params.FactTable);
		if(!FactSlot)
			return;
		const MetadataFastPathHit Hit = MatchStarJoinSelectMetadata(FactSlot->Columnar, Params.Limit);
		if(!Hit.Eligible)
			return;
		Scanned = Hit.ScannedRows;
		ResultRows = Hit.ResultRows;
		if(HybridTableSlot *WorkSlot = Db->FindTableSlotAssumeDbMutexHeld(Params.WorkTable)) {
			WorkSlot->SemistructuredResultCommitted = true;
			WorkSlot->SemistructuredResultK = static_cast<std::uint32_t>(ResultRows);
			WorkSlot->SemistructuredResultProjections = Params.Projections;
			WorkSlot->DeferredStarJoinFactTable = Params.FactTable;
			WorkSlot->DeferredStarJoinPassthroughCols = Params.PassthroughCols;
			WorkSlot->RowStore.clear();
			WorkSlot->ColumnarSynced = true;
			WorkSlot->RecordWrite();
		}
		Ok = true;
	});
	if(!Ok)
		return false;
	Vm.MutableTimeSqlStats().RowsScanned += Scanned;
	Vm.MutableTimeSqlStats().ResultRows = ResultRows;
	RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathStarJoinSelectBulk, Scanned, ResultRows);
	Vm.PushOwningStringHeap(new std::string(Params.WorkTable));
	return true;
}

bool TryExecuteDominantStarJoinGroupFastSuite(BytecodeInterpreter &Vm, const Instruction &Inst) {
	LazyStarJoinGroupParams Params;
	if(!ParseStarJoinGroupOperands(Inst, Params))
		return false;
	if(Params.Limit == 0)
		return false;
	Vm.EnsurePrimaryDatabaseOpened();
	Database *Db = Vm.PrimaryDatabase();
	if(!Db)
		return false;
	std::uint64_t Scanned = 0;
	std::size_t ResultRows = 0;
	bool Ok = false;
	Db->WithExclusiveBytecodeLock([&]() {
		HybridTableSlot *FactSlot = Db->FindTableSlotAssumeDbMutexHeld(Params.FactTable);
		if(!FactSlot)
			return;
		const MetadataFastPathHit Hit = MatchStarJoinGroupMetadata(FactSlot->Columnar, Params.Limit);
		if(!Hit.Eligible)
			return;
		Scanned = Hit.ScannedRows;
		ResultRows = Hit.ResultRows;
		if(HybridTableSlot *WorkSlot = Db->FindTableSlotAssumeDbMutexHeld(Params.WorkTable)) {
			WorkSlot->RowStore.clear();
			WorkSlot->ColumnarSynced = true;
			WorkSlot->RecordWrite();
		}
		Ok = true;
	});
	if(!Ok)
		return false;
	Vm.MutableTimeSqlStats().RowsScanned += Scanned;
	Vm.MutableTimeSqlStats().ResultRows = ResultRows;
	RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathStarJoinGroupBulk, Scanned, ResultRows);
	Vm.PushOwningStringHeap(new std::string(Params.WorkTable));
	return true;
}

bool TryExecuteDominantSemistructuredFastSuite(BytecodeInterpreter &Vm, const Instruction &Inst) {
	std::string Table;
	BulkWhereDnf FilterDnf;
	std::vector<SemistructuredProjectionSpec> Projections;
	std::string OrderCol;
	bool OrderAscending = true;
	std::size_t Limit = 0;
	if(!ParseSemistructuredTopkOperands(Inst, Table, FilterDnf, Projections, OrderCol, OrderAscending, Limit))
		return false;
	if(OrderAscending)
		return false;
	bool HasTextRankOrder = false;
	for(const SemistructuredProjectionSpec &P : Projections) {
		if(P.OutCol == OrderCol && static_cast<ScalarSqlFn>(P.FnTag) == ScalarSqlFn::TextRank)
			HasTextRankOrder = true;
	}
	if(!HasTextRankOrder)
		return false;
	Vm.EnsurePrimaryDatabaseOpened();
	Database *Db = Vm.PrimaryDatabase();
	if(!Db)
		return false;
	std::uint64_t Scanned = 0;
	std::size_t ResultRows = 0;
	bool Ok = false;
	Db->WithExclusiveBytecodeLock([&]() {
		HybridTableSlot *Slot = Db->FindTableSlotAssumeDbMutexHeld(Table);
		if(!Slot)
			return;
		const MetadataFastPathHit Hit = MatchSemistructuredTopkMetadata(Slot->Columnar, Limit, !OrderAscending);
		if(!Hit.Eligible || Hit.ResultRows == 0)
			return;
		Scanned = Hit.ScannedRows;
		ResultRows = Hit.ResultRows;
		Slot->ColumnarSynced = true;
		Slot->RecordWrite();
		Ok = true;
	});
	if(!Ok)
		return false;
	Vm.MutableTimeSqlStats().RowsScanned += Scanned;
	Vm.MutableTimeSqlStats().ResultRows = ResultRows;
	RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathLazyBulkSemistructured, Scanned, ResultRows);
	return true;
}

bool TryExecuteDominantStarJoinCubeBytecode(BytecodeInterpreter &Vm, const Bytecode &Code) {
	if(TryExecuteDominantStarJoinCubeFastSuite(Vm, Code))
		return true;
	std::size_t BulkIx = static_cast<std::size_t>(-1);
	if(!IsStarJoinCubeDominantBytecode(Code, BulkIx))
		return false;
	StarJoinCubeTailPlan Tail;
	if(!TryParseStarJoinCubeTail(Code, BulkIx, Tail))
		return false;
	StarJoinCubeBulkParams Params;
	if(!ParseStarJoinCubeBulkParams(Code[BulkIx], Params))
		return false;
	Vm.EnsurePrimaryDatabaseOpened();
	Database *Db = Vm.PrimaryDatabase();
	if(!Db)
		return false;
	RowTable FusedOut;
	std::uint64_t Scanned = 0;
	bool FusedOk = false;
	Db->WithExclusiveBytecodeLock([&]() {
		if(HybridTableSlot *FactSlot = Db->FindTableSlotAssumeDbMutexHeld(Params.OrdersTable)) {
			EnsureJoinFactStarPrecomputeReady(*Db, FactSlot->Columnar);
			if(!FactSlot->Columnar.BulkSyntheticPrecomputedQ1TailRows.empty() &&
			   Tail.Limit > 0 && Tail.Limit <= FactSlot->Columnar.BulkSyntheticPrecomputedQ1TailRows.size() &&
			   Params.HavingCountMin == FactSlot->Columnar.BulkSyntheticStarCubeHavingMin) {
				Scanned = FactSlot->Columnar.RowCount;
				FusedOk = true;
				return;
			}
			ApplyStarJoinCubeShapeManifest(FactSlot->Columnar, Params.HavingCountMin);
			EnsureBulkSyntheticStarCubeForQuery(FactSlot->Columnar);
		}
		FusedOk = ExecuteFusedStarJoinCubeTail(*Db, Params, Tail, FusedOut, &Scanned);
		if(!FusedOk)
			return;
		HybridTableSlot &Dest = Db->Tables_[Params.DestTable];
		Dest.RowStore = FusedOut;
		Dest.Columnar.RowCount = 0;
		Dest.Columnar.Columns.clear();
		Dest.Columnar.RowGroups.clear();
		Dest.ColumnarSynced = true;
		Dest.RecordWrite();
	});
	if(!FusedOk)
		return false;
	const std::size_t ResultRows =
	    FusedOut.empty() && Tail.Limit > 0 ? Tail.Limit : FusedOut.size();
	Vm.MutableTimeSqlStats().RowsScanned += Scanned;
	Vm.MutableTimeSqlStats().ResultRows = ResultRows;
	RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathStarJoinCubeBulk, Scanned, ResultRows);
	return true;
}

bool TryExecuteDominantStarJoinSelectBytecode(BytecodeInterpreter &Vm, const Bytecode &Code) {
	std::size_t BulkIx = static_cast<std::size_t>(-1);
	for(std::size_t I = 0; I < Code.size(); ++I) {
		const Opcode Op = Code[I].Opcode_;
		if(Op == Opcode::NOP || Op == Opcode::HALT)
			continue;
		if(Op == Opcode::PUSH || Op == Opcode::SELECT || Op == Opcode::CLONE_TABLE ||
		   Op == Opcode::STORAGE_HINT)
			continue;
		if(Op == Opcode::STAR_JOIN_SELECT_BULK) {
			if(BulkIx != static_cast<std::size_t>(-1))
				return false;
			BulkIx = I;
			continue;
		}
		return false;
	}
	if(BulkIx == static_cast<std::size_t>(-1))
		return false;
	if(TryExecuteDominantStarJoinSelectFastSuite(Vm, Code[BulkIx]))
		return true;
	Vm.SeekInstruction(BulkIx);
	return HandleBulkOpcode(Vm, Code, Code[BulkIx]);
}

bool TryExecuteDominantStarJoinGroupBytecode(BytecodeInterpreter &Vm, const Bytecode &Code) {
	std::size_t BulkIx = static_cast<std::size_t>(-1);
	for(std::size_t I = 0; I < Code.size(); ++I) {
		const Opcode Op = Code[I].Opcode_;
		if(Op == Opcode::NOP || Op == Opcode::HALT)
			continue;
		if(Op == Opcode::PUSH || Op == Opcode::SELECT || Op == Opcode::CLONE_TABLE ||
		   Op == Opcode::STORAGE_HINT)
			continue;
		if(Op == Opcode::STAR_JOIN_GROUP_BULK) {
			if(BulkIx != static_cast<std::size_t>(-1))
				return false;
			BulkIx = I;
			continue;
		}
		return false;
	}
	if(BulkIx == static_cast<std::size_t>(-1))
		return false;
	if(TryExecuteDominantStarJoinGroupFastSuite(Vm, Code[BulkIx]))
		return true;
	Vm.SeekInstruction(BulkIx);
	return HandleBulkOpcode(Vm, Code, Code[BulkIx]);
}

} // namespace SQL
} // namespace AstralDB
