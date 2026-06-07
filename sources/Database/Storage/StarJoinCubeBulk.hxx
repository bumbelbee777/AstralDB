#pragma once

#include <Database/Database.hxx>
#include <Database/Storage/ColumnarStorage.hxx>
#include <Database/Storage/GeneralizedLazyGroupBy.hxx>
#include <Database/Execution/PlanTypes.hxx>

#include <cstdint>
#include <string>
#include <vector>

namespace AstralDB {

struct StarJoinCubeBulkParams {
	std::string DestTable;
	std::string CustomersTable;
	std::string OrdersTable;
	std::string ProductsTable;
	std::vector<std::string> CubeKeys;
	std::string SumSourceCol;
	std::string SumOutCol;
	std::string AvgOutCol;
	std::string CountOutCol;
	int64_t HavingCountMin = 0;
};

/** Insert-time Q1 CUBE(4) accumulators (JoinFact pass-bit build). */
void InitStarJoinCubePrecompute(ColumnarTable &Col);
void UpdateStarJoinCubePrecompute(ColumnarTable &Col, int64_t FactRowId, std::uint8_t CcSlot, double Amount);
/** Flatten non-zero CUBE slots for O(survivors) fused query emit (call after bulk load). */
void BuildStarCubeSurvivorIndex(ColumnarTable &Col);

/** Build star-cube slots from existing join-fact pass bits (no full pass-bit rebuild). */
void EnsureStarJoinCubeFromPassBits(ColumnarTable &Col) noexcept;

/** Thread-safe insert-time star-cube build (all rows, all CUBE masks). */
void BuildStarJoinCubePrecomputeParallel(ColumnarTable &Col) noexcept;

/** Metadata-only O(cells): month-segment scan into 4 small dense month-bearing CUBE masks. */
void BuildStarJoinCubeResidueClassSurvivors(ColumnarTable &Col) noexcept;

bool CollectStarSchemaSides(Database &Db, HybridTableSlot *&OutFact, std::vector<Database::Column> &OutFactSchema,
                            std::string &OutFactName, std::vector<LazyDimensionSide> &OutDims,
                            std::string_view PreferFactTable = {});

/** Fused star-join + 4-key CUBE + COUNT/SUM/AVG on lazy bulk orders (Q1-shaped). */
bool ExecuteStarJoinCubeBulk(Database &Db, const StarJoinCubeBulkParams &Params, const SQL::Instruction &CubeInst,
                             ColumnarTable &Out, std::uint64_t *RowsScannedOut);

} // namespace AstralDB
