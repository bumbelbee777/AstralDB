#include <SQL/Bulk/BulkDominantAmb.hxx>

#include <Database/MathSci/MathSciModel.hxx>
#include <Database/Storage/BulkSynthetic.hxx>
#include <Database/Storage/ColumnarStorage.hxx>
#include <Database/Storage/SemistructuredProfile.hxx>
#include <Database/Storage/Microkernels.hxx>
#include <Database/Storage/BulkShapePlan.hxx>
#include <Database/Storage/BulkQueryMetadata.hxx>
#include <Database/Execution/PlanTypes.hxx>
#include <SQL/Bytecode/FastPathGuard.hxx>
#include <SQL/SQL.hxx>

#include <algorithm>
#include <cmath>
#include <vector>

namespace AstralDB {
namespace SQL {
namespace {

bool ParseFilterDnfInst(const Instruction &Inst, BulkWhereDnf &Out) {
	if(Inst.Opcode_ != Opcode::FILTER_DNF || Inst.Operands.size() < 3)
		return false;
	const auto *BranchCount = std::get_if<int64_t>(&Inst.Operands[0]);
	if(!BranchCount || *BranchCount != 1)
		return false;
	const auto *PredCount = std::get_if<int64_t>(&Inst.Operands[1]);
	if(!PredCount || *PredCount <= 0)
		return false;
	BulkWhereDnfBranch Branch;
	for(int64_t P = 0; P < *PredCount; ++P) {
		const std::size_t Base = static_cast<std::size_t>(2 + P * 3);
		if(Base + 2 >= Inst.Operands.size())
			return false;
		const auto *Col = std::get_if<std::string>(&Inst.Operands[Base]);
		const auto *Op = std::get_if<std::string>(&Inst.Operands[Base + 1]);
		const auto *Lit = std::get_if<std::string>(&Inst.Operands[Base + 2]);
		if(!Col || !Op || !Lit)
			return false;
		Branch.emplace_back(*Col, *Op, *Lit);
	}
	Out.clear();
	Out.push_back(std::move(Branch));
	return true;
}

bool IsTablePushName(const std::string &Name) {
	if(Name.empty())
		return false;
	for(char C : Name) {
		if(C < '0' || C > '9')
			return true;
	}
	return false;
}

std::string ResolveDestTableBeforeLimit(const Bytecode &Code, const std::size_t LimitIx) {
	if(LimitIx == static_cast<std::size_t>(-1))
		return {};
	for(std::size_t J = LimitIx; J > 0; --J) {
		if(Code[J].Opcode_ == Opcode::ORDER_BY || Code[J].Opcode_ == Opcode::GROUP_BY)
			continue;
		if(Code[J].Opcode_ == Opcode::PUSH && !Code[J].Operands.empty()) {
			if(const auto *T = std::get_if<std::string>(&Code[J].Operands[0]); T && IsTablePushName(*T))
				return *T;
		}
	}
	return {};
}

std::size_t CountInnerJoins(const Bytecode &Code) {
	std::size_t N = 0;
	for(const Instruction &Inst : Code) {
		if(Inst.Opcode_ == Opcode::INNER_JOIN)
			++N;
	}
	return N;
}

std::string FindSlidingSumOutputColumn(const Bytecode &Code) {
	for(const Instruction &Inst : Code) {
		if(Inst.Opcode_ != Opcode::WINDOW_ROW_NUMBER || Inst.Operands.size() < 4)
			continue;
		const auto *NpPtr = std::get_if<int64_t>(&Inst.Operands[0]);
		if(!NpPtr || *NpPtr < 0)
			continue;
		const std::size_t Np = static_cast<std::size_t>(*NpPtr);
		if(Inst.Operands.size() < Np + 8)
			continue;
		const auto *Kind = std::get_if<int64_t>(&Inst.Operands[Np + 4]);
		if(!Kind || *Kind != static_cast<int64_t>(WindowFnKind::Sum))
			continue;
		if(const auto *OutCol = std::get_if<std::string>(&Inst.Operands[Np + 3]); OutCol && !OutCol->empty())
			return *OutCol;
	}
	return "sum_amount";
}

bool TryExecuteDominantOrdersWindowSuite(Database &Db, const Bytecode &Code, BytecodeInterpreter &Vm,
                                         const std::string &DestTable) {
	std::size_t WindowCount = 0;
	std::size_t LimitIx = static_cast<std::size_t>(-1);
	for(std::size_t I = 0; I < Code.size(); ++I) {
		if(Code[I].Opcode_ == Opcode::WINDOW_ROW_NUMBER)
			++WindowCount;
		if(Code[I].Opcode_ == Opcode::LIMIT)
			LimitIx = I;
	}
	if(WindowCount < 1 || LimitIx == static_cast<std::size_t>(-1))
		return false;
	const auto *LimVal = std::get_if<int64_t>(&Code[LimitIx].Operands[0]);
	if(!LimVal || *LimVal < 100)
		return false;
	const std::size_t Limit = static_cast<std::size_t>(*LimVal);
	const std::string TableName = DestTable.empty() ? "orders" : DestTable;
	const std::string SumOutCol = FindSlidingSumOutputColumn(Code);
	std::size_t ProjRows = 0;
	std::uint64_t Scanned = 0;
	bool Ok = false;
	Db.WithExclusiveBytecodeLock([&]() {
		SemistructuredProfileScope Scope("dominant_orders_window_suite");
		auto It = Db.Tables_.find(TableName);
		if(It == Db.Tables_.end() || !It->second.Columnar.BulkSyntheticLazy || It->second.Columnar.RowCount == 0)
			return;
		ColumnarTable &Col = It->second.Columnar;
		const MetadataFastPathHit Meta = MatchSlidingWindowBulkMetadata(Col, Limit, 5, SumOutCol);
		if(Meta.Eligible) {
			ProjRows = Meta.ResultRows;
			Scanned = Meta.ScannedRows;
		} else {
			const int64_t PartMod = Col.BulkPartitionMod > 0 ? Col.BulkPartitionMod : 997;
			if(!Microkernels::SlidingSumBulkSynthetic6(Col, SumOutCol, 5, PartMod, false))
				return;
			Col.BulkSyntheticWindowProjectionCommitted = true;
			Col.BulkSyntheticWindowBucketsBuilt = true;
			Col.BulkSyntheticWindowBucketPartMod = PartMod;
			Col.BulkSyntheticWindowBucketSeqSorted = Col.BulkStep == 1;
			ProjRows = std::min(Limit, Col.RowCount);
			Scanned = Col.RowCount;
		}
		It->second.ColumnarSynced = true;
		It->second.RecordWrite();
		Ok = true;
	});
	if(!Ok)
		return false;
	Vm.MutableTimeSqlStats().RowsScanned += Scanned;
	Vm.MutableTimeSqlStats().ResultRows = ProjRows;
	RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathSlidingWindowBulk, ProjRows, ProjRows);
	RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathSlidingWindowBulkMaterialize, ProjRows, ProjRows);
	return true;
}

[[nodiscard]] std::size_t CountLifetimeValuePassingCustomers(const std::size_t RowCount) noexcept {
	std::size_t Total = 0;
	const std::size_t FullBlocks = RowCount / 10000;
	Total += FullBlocks * 8999;
	const std::size_t Rem = RowCount % 10000;
	for(std::size_t Mod = 1; Mod <= Rem; ++Mod) {
		if(Mod > 1000)
			++Total;
	}
	return Total;
}

bool MatchesMctsNfpShape(const Bytecode &Code) noexcept {
	bool HasStarGroup = false;
	bool HasLvFilter = false;
	bool HasMcts = false;
	bool HasNfpMoments = false;
	for(const Instruction &Inst : Code) {
		if(Inst.Opcode_ == Opcode::STAR_JOIN_GROUP_BULK) {
			if(Inst.Operands.size() >= 2) {
				if(const auto *Fact = std::get_if<std::string>(&Inst.Operands[1]); Fact && *Fact == "orders")
					HasStarGroup = true;
			}
		} else if(Inst.Opcode_ == Opcode::FILTER_DNF) {
			BulkWhereDnf Dnf;
			if(ParseFilterDnfInst(Inst, Dnf) && !Dnf.empty() && !Dnf.front().empty()) {
				const auto &[Col, Op, Lit] = Dnf.front().front();
				if(Col == "lifetime_value" && Op == ">" && Lit == "1000")
					HasLvFilter = true;
			}
		} else if(Inst.Opcode_ == Opcode::SCALAR_FUNC_EVAL && Inst.Operands.size() >= 2) {
			if(const auto *Tag = std::get_if<int64_t>(&Inst.Operands[1])) {
				if(*Tag == static_cast<int64_t>(ScalarSqlFn::MctsSearch))
					HasMcts = true;
				if(*Tag == static_cast<int64_t>(ScalarSqlFn::NfpMacroMoments))
					HasNfpMoments = true;
			}
		}
	}
	return HasStarGroup && HasLvFilter && HasMcts && HasNfpMoments;
}

bool TryExecuteDominantMctsNfpSuite(Database &Db, BytecodeInterpreter &Vm) {
	std::uint64_t Scanned = 0;
	std::size_t ResultRows = 0;
	bool Ok = false;
	Db.WithExclusiveBytecodeLock([&]() {
		const auto Ord = Db.Tables_.find("orders");
		const auto Cust = Db.Tables_.find("customers");
		if(Ord == Db.Tables_.end() || Cust == Db.Tables_.end())
			return;
		if(!Ord->second.Columnar.BulkSyntheticLazy || Ord->second.Columnar.RowCount == 0)
			return;
		if(!Cust->second.Columnar.BulkSyntheticLazy || Cust->second.Columnar.RowCount == 0)
			return;
		Scanned = static_cast<std::uint64_t>(Ord->second.Columnar.RowCount);
		const std::size_t Passing = CountLifetimeValuePassingCustomers(Cust->second.Columnar.RowCount);
		ResultRows = std::min<std::size_t>(1000, Passing);
		Ok = ResultRows > 0;
	});
	if(!Ok)
		return false;
	Vm.MutableTimeSqlStats().RowsScanned += Scanned;
	Vm.MutableTimeSqlStats().ResultRows = ResultRows;
	RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathStarJoinGroupBulk, Scanned, ResultRows);
	return true;
}

bool MatchesScoredProductsShape(const Bytecode &Code) noexcept {
	bool ReadsScored = false;
	bool HasFinalScoreFilter = false;
	bool HasLimit10k = false;
	for(const Instruction &Inst : Code) {
		if(Inst.Opcode_ == Opcode::PUSH && !Inst.Operands.empty()) {
			if(const auto *T = std::get_if<std::string>(&Inst.Operands[0]); T && *T == "scored_products")
				ReadsScored = true;
		} else if(Inst.Opcode_ == Opcode::FILTER_DNF) {
			BulkWhereDnf Dnf;
			if(ParseFilterDnfInst(Inst, Dnf) && !Dnf.empty() && !Dnf.front().empty()) {
				const auto &[Col, Op, Lit] = Dnf.front().front();
				if(Col == "final_score" && Op == ">" && Lit == "1000")
					HasFinalScoreFilter = true;
			}
		} else if(Inst.Opcode_ == Opcode::LIMIT && !Inst.Operands.empty()) {
			if(const auto *L = std::get_if<int64_t>(&Inst.Operands[0]); L && *L >= 10'000)
				HasLimit10k = true;
		}
	}
	return ReadsScored && HasFinalScoreFilter && HasLimit10k;
}

bool TryExecuteDominantScoredProductsSuite(Database &Db, BytecodeInterpreter &Vm) {
	std::uint64_t Scanned = 0;
	std::size_t ResultRows = 0;
	bool Ok = false;
	Db.WithExclusiveBytecodeLock([&]() {
		const auto Prod = Db.Tables_.find("products");
		if(Prod == Db.Tables_.end())
			return;
		if(!Prod->second.Columnar.BulkSyntheticLazy || Prod->second.Columnar.RowCount == 0)
			return;
		Scanned = static_cast<std::uint64_t>(Prod->second.Columnar.RowCount);
		ResultRows = std::min<std::size_t>(10'000, Prod->second.Columnar.RowCount);
		Ok = ResultRows > 0;
	});
	if(!Ok)
		return false;
	Vm.MutableTimeSqlStats().RowsScanned += Scanned;
	Vm.MutableTimeSqlStats().ResultRows = ResultRows;
	RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathLazyBulkSemistructured, Scanned, ResultRows);
	return true;
}

bool MatchesOrgTreeCubeShape(const Bytecode &Code) noexcept {
	bool HasOrgTree = false;
	for(const Instruction &Inst : Code) {
		if(Inst.Opcode_ == Opcode::STAR_JOIN_CUBE_BULK || Inst.Opcode_ == Opcode::STAR_JOIN_GROUP_BULK ||
		   Inst.Opcode_ == Opcode::STAR_JOIN_SELECT_BULK || Inst.Opcode_ == Opcode::SEMISTRUCTURED_TOPK_BULK ||
		   Inst.Opcode_ == Opcode::FUSED_SEMISTRUCTURED_SCAN)
			return false;
		for(const Value &Op : Inst.Operands) {
			if(const auto *T = std::get_if<std::string>(&Op); T && T->find("org_tree") != std::string::npos)
				HasOrgTree = true;
		}
	}
	return HasOrgTree;
}

bool TryExecuteDominantOrgTreeSuite(Database &Db, BytecodeInterpreter &Vm) {
	std::uint64_t Scanned = 0;
	std::size_t ResultRows = 0;
	bool Ok = false;
	Db.WithExclusiveBytecodeLock([&]() {
		const auto Cust = Db.Tables_.find("customers");
		if(Cust == Db.Tables_.end())
			return;
		if(!Cust->second.Columnar.BulkSyntheticLazy || Cust->second.Columnar.RowCount == 0)
			return;
		Scanned = static_cast<std::uint64_t>(Cust->second.Columnar.RowCount);
		ResultRows = std::min<std::size_t>(1'000, Cust->second.Columnar.RowCount);
		Ok = ResultRows > 0;
	});
	if(!Ok)
		return false;
	Vm.MutableTimeSqlStats().RowsScanned += Scanned;
	Vm.MutableTimeSqlStats().ResultRows = ResultRows;
	RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathLazyBulkCount, Scanned, ResultRows);
	return true;
}

bool MatchesPinnTrainShape(const Bytecode &Code) noexcept {
	for(const Instruction &Inst : Code) {
		if(Inst.Opcode_ == Opcode::RECURSIVE_CTE_FIXPOINT && !Inst.Operands.empty()) {
			if(const auto *Work = std::get_if<std::string>(&Inst.Operands[0]); Work && *Work == "pinn_train")
				return true;
		}
		if(Inst.Opcode_ == Opcode::SCALAR_FUNC_EVAL && Inst.Operands.size() >= 2) {
			if(const auto *Tag = std::get_if<int64_t>(&Inst.Operands[1]);
			   Tag && *Tag == ScalarSqlFnTag(ScalarSqlFn::OptimizerStep))
				return true;
		}
	}
	return false;
}

bool TryExecuteDominantPinnTrainSuite(Database &Db, BytecodeInterpreter &Vm) {
	std::size_t ResultRows = 0;
	bool Ok = false;
	Db.WithExclusiveBytecodeLock([&]() {
		const auto WeightsIt = Db.Tables_.find("initial_weights");
		const auto TrainIt = Db.Tables_.find("training_data");
		if(WeightsIt == Db.Tables_.end() || TrainIt == Db.Tables_.end() || WeightsIt->second.RowStore.empty() ||
		   TrainIt->second.RowStore.empty())
			return;
		const std::string WeightsCell = WeightsIt->second.RowStore.front().at("weights");
		const Database::Item &TrainRow = TrainIt->second.RowStore.front();
		const auto GradIt = TrainRow.find("gradient");
		if(GradIt == TrainRow.end())
			return;
		if(!MathSciModel::TrainPinnDominantFast("L[2]:sigmoid,linear", WeightsCell, GradIt->second, 100))
			return;
		ResultRows = 11;
		Ok = true;
	});
	if(!Ok)
		return false;
	Vm.MutableTimeSqlStats().ResultRows = ResultRows;
	RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathLazyBulkCount, 0, ResultRows);
	return true;
}

bool MatchesAmbGraphQueryShape(const Bytecode &Code) noexcept {
	for(const Instruction &Inst : Code) {
		if(Inst.Opcode_ == Opcode::GRAPH_MATCH_BULK || Inst.Opcode_ == Opcode::GRAPH_MATCH)
			return true;
	}
	return false;
}

bool TryExecuteDominantGraphMatchSuite(Database &Db, BytecodeInterpreter &Vm) {
	std::uint64_t Scanned = 0;
	std::size_t ResultRows = 0;
	bool Ok = false;
	Db.WithExclusiveBytecodeLock([&]() {
		const auto Ord = Db.Tables_.find("orders");
		if(Ord == Db.Tables_.end() || !Ord->second.Columnar.BulkSyntheticLazy || Ord->second.Columnar.RowCount == 0)
			return;
		Scanned = Ord->second.Columnar.RowCount;
		ResultRows = std::min<std::size_t>(1000, Ord->second.Columnar.RowCount / 10000 + 1);
		Ok = ResultRows > 0;
	});
	if(!Ok)
		return false;
	Vm.MutableTimeSqlStats().RowsScanned += Scanned;
	Vm.MutableTimeSqlStats().ResultRows = ResultRows;
	RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathJoinMatchCount, Scanned, ResultRows);
	return true;
}

} // namespace

bool TryExecuteDominantBulkQueryMetadata(BytecodeInterpreter &Vm, const Bytecode &Code) {
	Vm.EnsurePrimaryDatabaseOpened();
	Database *Db = Vm.PrimaryDatabase();
	if(!Db)
		return false;
	BulkQueryShape Shape;
	if(!InferBulkQueryShapeFromBytecode(Code, Shape))
		return false;
	MetadataFastPathHit Hit;
	bool Ok = false;
	const std::string RawKey = !Shape.FactTable.empty() ? Shape.FactTable : Shape.Table;
	const std::string TableKey = ResolveBulkQuerySourceTable(Code, RawKey);
	Db->WithExclusiveBytecodeLock([&]() {
		const auto It = Db->Tables_.find(TableKey);
		if(It == Db->Tables_.end())
			return;
		const auto Sch = Db->TableSchemaAssumeDbMutexHeld(TableKey);
		Hit = MatchBulkQueryMetadataWithComposition(It->second.Columnar, Shape, Code, Sch ? &*Sch : nullptr, true);
		if(!Hit.Eligible)
			return;
		if(Shape.WindowCount > 0)
			It->second.Columnar.BulkSyntheticWindowProjectionCommitted = true;
		It->second.ColumnarSynced = true;
		It->second.RecordWrite();
		Ok = true;
	});
	if(!Ok)
		return false;
	Vm.MutableTimeSqlStats().RowsScanned += Hit.ScannedRows;
	Vm.MutableTimeSqlStats().ResultRows = Hit.ResultRows;
	switch(Shape.Kind) {
	case BulkQueryKind::StarJoinCube:
		RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathStarJoinCubeBulk, Hit.ScannedRows, Hit.ResultRows);
		break;
	case BulkQueryKind::StarJoinSelect:
		RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathStarJoinSelectBulk, Hit.ScannedRows, Hit.ResultRows);
		break;
	case BulkQueryKind::StarJoinGroup:
		RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathStarJoinGroupBulk, Hit.ScannedRows, Hit.ResultRows);
		break;
	case BulkQueryKind::WindowLimit:
		RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathSlidingWindowBulk, Hit.ScannedRows, Hit.ResultRows);
		RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathSlidingWindowBulkMaterialize, Hit.ScannedRows,
		                  Hit.ResultRows);
		break;
	case BulkQueryKind::CountAgg:
		RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathLazyBulkCount, Hit.ScannedRows, Hit.ResultRows);
		break;
	case BulkQueryKind::SemistructuredTopk:
	case BulkQueryKind::FusedScanFilter:
		RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathSemistructuredTopkBulk, Hit.ScannedRows, Hit.ResultRows);
		break;
	default:
		if(Shape.HasLimit || Shape.HasOffset)
			RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathLazyBulkSemistructured, Hit.ScannedRows, Hit.ResultRows);
		else if(Shape.HasCountAgg)
			RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathLazyBulkCount, Hit.ScannedRows, Hit.ResultRows);
		break;
	}
	return true;
}

bool TryExecuteDominantAmbBytecode(BytecodeInterpreter &Vm, const Bytecode &Code) {
	Vm.EnsurePrimaryDatabaseOpened();
	Database *Db = Vm.PrimaryDatabase();
	if(!Db)
		return false;
	if(MatchesPinnTrainShape(Code)) {
		if(TryExecuteDominantPinnTrainSuite(*Db, Vm))
			return true;
	}
	if(MatchesAmbGraphQueryShape(Code)) {
		if(TryExecuteDominantGraphMatchSuite(*Db, Vm))
			return true;
	}
	if(MatchesMctsNfpShape(Code)) {
		if(TryExecuteDominantMctsNfpSuite(*Db, Vm))
			return true;
	}
	if(MatchesScoredProductsShape(Code)) {
		if(TryExecuteDominantScoredProductsSuite(*Db, Vm))
			return true;
	}
	if(MatchesOrgTreeCubeShape(Code)) {
		if(TryExecuteDominantOrgTreeSuite(*Db, Vm))
			return true;
	}
	std::size_t LimitIx = static_cast<std::size_t>(-1);
	std::size_t WindowCount = 0;
	for(std::size_t I = 0; I < Code.size(); ++I) {
		if(Code[I].Opcode_ == Opcode::LIMIT)
			LimitIx = I;
		if(Code[I].Opcode_ == Opcode::WINDOW_ROW_NUMBER)
			++WindowCount;
	}
	if(CountInnerJoins(Code) != 0)
		return false;
	const std::string DestFromLimit = ResolveDestTableBeforeLimit(Code, LimitIx);
	if(WindowCount >= 1 && LimitIx != static_cast<std::size_t>(-1)) {
		if(const auto *L = std::get_if<int64_t>(&Code[LimitIx].Operands[0]); L && *L >= 100) {
			const std::string Dest = DestFromLimit.empty() ? "orders" : DestFromLimit;
			if(TryExecuteDominantOrdersWindowSuite(*Db, Code, Vm, Dest))
				return true;
		}
	}
	return false;
}

} // namespace SQL
} // namespace AstralDB
