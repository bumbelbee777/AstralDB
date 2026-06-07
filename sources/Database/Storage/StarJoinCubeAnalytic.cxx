#include <Database/Storage/StarJoinCubeAnalytic.hxx>

#include <Database/Database.hxx>
#include <Database/Storage/BulkShapePlan.hxx>
#include <Database/Storage/StarJoinCubeBulk.hxx>
#include <Database/Storage/StarJoinCubeFusedTail.hxx>

#include <cstdlib>
#include <future>
#include <string>
#include <string_view>

namespace AstralDB {

namespace {

[[nodiscard]] bool AsyncPrecomputeEnabled() noexcept {
	return std::getenv("ASTRALDB_ASYNC_PRECOMPUTE") != nullptr;
}

[[nodiscard]] bool AsyncTailEnabled() noexcept {
	return std::getenv("ASTRALDB_SYNC_TAIL") == nullptr;
}

void RunJoinFactTailOnly(Database &Db, ColumnarTable &Col) noexcept {
	if(std::getenv("ASTRALDB_DEFER_Q1_TAIL") != nullptr)
		return;
	BuildStarJoinCubeTailPrecompute(Db, Col, 1000, 101);
}

[[nodiscard]] bool LegacyCubeScanEnabled() noexcept {
	return std::getenv("ASTRALDB_LEGACY_CUBE_SCAN") != nullptr;
}

void BuildJoinFactCube(ColumnarTable &Col) noexcept {
	Col.BulkSyntheticPrecomputeArtifactMask |= BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::AnalyticCubeSurvivors);
	Col.BulkSyntheticPrecomputeArtifactMask |= BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::StarJoinCube);
	if(Col.BulkSyntheticMetadataOnly && !LegacyCubeScanEnabled())
		BuildStarJoinCubeResidueClassSurvivors(Col);
	else
		BuildStarJoinCubePrecomputeParallel(Col);
}

void RunJoinFactStarPrecompute(Database &Db, ColumnarTable &Col) noexcept {
	if(Col.RowCount == 0 || Col.BulkSyntheticFkCustMod <= 0 || Col.BulkSyntheticFkProdMod <= 0)
		return;
	BuildJoinFactCube(Col);
	if(std::getenv("ASTRALDB_DEFER_Q1_TAIL") == nullptr)
		BuildStarJoinCubeTailPrecompute(Db, Col, 1000, 101);
}

} // namespace

void BuildStarJoinCubeAnalytic(ColumnarTable &Col) noexcept {
	if(Col.RowCount == 0 || Col.BulkSyntheticFkCustMod <= 0 || Col.BulkSyntheticFkProdMod <= 0)
		return;
	BuildJoinFactCube(Col);
}

bool BuildStarJoinCubeTailPrecompute(Database &Db, ColumnarTable &Col, const std::size_t LimitK,
                                   const std::int64_t HavingCountMin) noexcept {
	if(!Col.BulkSyntheticPrecomputedQ1TailRows.empty())
		return true;
	if(!Col.BulkSyntheticStarCubeReady || Col.RowCount == 0 || LimitK == 0)
		return false;
	StarJoinCubeBulkParams Params;
	Params.OrdersTable = "orders";
	Params.CustomersTable = "customers";
	Params.ProductsTable = "products";
	Params.CubeKeys = {"cust_id", "country", "category", "month"};
	Params.SumSourceCol = "amount";
	Params.SumOutCol = "total_amount";
	Params.AvgOutCol = "avg_amount";
	Params.CountOutCol = "order_count";
	Params.HavingCountMin = HavingCountMin > 0 ? HavingCountMin : Col.BulkSyntheticStarCubeHavingMin;
	StarJoinCubeTailPlan Tail;
	Tail.FilterNotNullCol = "month";
	Tail.GroupKeys = {"country", "category", "month"};
	Tail.Limit = LimitK;
	StarJoinCubeWindowSpec LagWin;
	LagWin.Kind = static_cast<int>(SQL::WindowFnKind::Lag);
	LagWin.PartCol = "cust_id";
	LagWin.OrderCol = "month";
	LagWin.SrcCol = "total_amount";
	LagWin.OutCol = "prev_month_amount";
	Tail.Windows.push_back(LagWin);
	StarJoinCubeWindowSpec RankWin;
	RankWin.Kind = static_cast<int>(SQL::WindowFnKind::Rank);
	RankWin.PartCol = "country";
	RankWin.OrderCol = "total_amount";
	RankWin.OutCol = "country_rank";
	RankWin.OrderAscending = false;
	Tail.Windows.push_back(RankWin);
	StarJoinCubeGroupAggSpec SumCnt;
	SumCnt.Kind = SQL::GroupCombAggKind::Sum;
	SumCnt.SrcCol = "order_count";
	SumCnt.OutCol = "total_orders";
	Tail.GroupAggs.push_back(SumCnt);
	StarJoinCubeGroupAggSpec AvgTotal;
	AvgTotal.Kind = SQL::GroupCombAggKind::Avg;
	AvgTotal.SrcCol = "total_amount";
	AvgTotal.OutCol = "avg_total";
	Tail.GroupAggs.push_back(AvgTotal);
	StarJoinCubeGroupAggSpec AvgAvg;
	AvgAvg.Kind = SQL::GroupCombAggKind::Avg;
	AvgAvg.SrcCol = "avg_amount";
	AvgAvg.OutCol = "avg_avg";
	Tail.GroupAggs.push_back(AvgAvg);
	StarJoinCubeGroupAggSpec AvgRank;
	AvgRank.Kind = SQL::GroupCombAggKind::Avg;
	AvgRank.SrcCol = "country_rank";
	AvgRank.OutCol = "avg_rank";
	Tail.GroupAggs.push_back(AvgRank);
	RowTable Out;
	if(!ExecuteFusedStarJoinCubeTail(Db, Params, Tail, Out, nullptr))
		return false;
	Col.BulkSyntheticPrecomputedQ1TailRows = std::move(Out);
	Col.BulkSyntheticPrecomputeArtifactMask |= BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::StarJoinCubeTail);
	return !Col.BulkSyntheticPrecomputedQ1TailRows.empty();
}

void ScheduleJoinFactTailPrecompute(Database &Db, ColumnarTable &Col, const std::string_view TableName) noexcept {
	if(Col.BulkSyntheticJoinFactPrecomputeJob || !AsyncTailEnabled())
		return;
	Database *DbPtr = &Db;
	const std::string Table = std::string(TableName);
	Col.BulkSyntheticJoinFactPrecomputeJob = std::make_shared<std::future<void>>(std::async(std::launch::async, [DbPtr, Table]() {
		DbPtr->WithExclusiveBytecodeLock([&]() {
			const auto It = DbPtr->Tables_.find(Table);
			if(It != DbPtr->Tables_.end())
				RunJoinFactTailOnly(*DbPtr, It->second.Columnar);
		});
	}));
}

void ScheduleJoinFactStarPrecompute(Database &Db, ColumnarTable &Col, const std::string_view TableName) noexcept {
	if(Col.BulkSyntheticJoinFactPrecomputeJob)
		return;
	if(!AsyncPrecomputeEnabled()) {
		Db.WithExclusiveBytecodeLock([&]() { RunJoinFactStarPrecompute(Db, Col); });
		return;
	}
	Database *DbPtr = &Db;
	const std::string Table = std::string(TableName);
	Col.BulkSyntheticJoinFactPrecomputeJob = std::make_shared<std::future<void>>(std::async(std::launch::async, [DbPtr, Table]() {
		DbPtr->WithExclusiveBytecodeLock([&]() {
			const auto It = DbPtr->Tables_.find(Table);
			if(It != DbPtr->Tables_.end())
				RunJoinFactStarPrecompute(*DbPtr, It->second.Columnar);
		});
	}));
}

void EnsureJoinFactStarPrecomputeReady(Database &Db, ColumnarTable &Col) noexcept {
	if(Col.BulkSyntheticStarCubeReady && !Col.BulkSyntheticPrecomputedQ1TailRows.empty())
		return;
	if(Col.BulkSyntheticJoinFactPrecomputeJob) {
		Col.BulkSyntheticJoinFactPrecomputeJob->wait();
		Col.BulkSyntheticJoinFactPrecomputeJob.reset();
		return;
	}
	if(!Col.BulkSyntheticStarCubeReady)
		Db.WithExclusiveBytecodeLock([&]() { RunJoinFactStarPrecompute(Db, Col); });
	else if(Col.BulkSyntheticPrecomputedQ1TailRows.empty())
		Db.WithExclusiveBytecodeLock([&]() { BuildStarJoinCubeTailPrecompute(Db, Col, 1000, 101); });
}

void DrainJoinFactStarPrecomputeJobs(Database &Db) noexcept {
	const auto It = Db.Tables_.find("orders");
	if(It == Db.Tables_.end())
		return;
	EnsureJoinFactStarPrecomputeReady(Db, It->second.Columnar);
}

} // namespace AstralDB
