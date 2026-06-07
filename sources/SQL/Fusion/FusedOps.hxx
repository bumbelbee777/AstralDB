#pragma once

#include <Database/Database.hxx>
#include <Database/Storage/ColumnarStorage.hxx>
#include <SQL/Fusion/FusionPlanTypes.hxx>

#include <cstdint>
#include <string>
#include <vector>

namespace AstralDB {
namespace SQL {

struct FusedScanFilterParams {
	std::string Table;
	std::vector<FilterTriple> Filters;
};

struct FusedScanFilterAggSumParams {
	std::string Table;
	std::vector<FilterTriple> Filters;
	std::vector<std::string> GroupKeys;
	std::string SumColumn;
	std::string SumOutputColumn;
};

struct FusedScanFilterAggLimitParams : FusedScanFilterAggSumParams {
	int64_t Limit = -1;
	int64_t Offset = 0;
};

struct FusedJoinFilterParams {
	std::string LeftTable;
	std::string RightTable;
	std::string LeftCol;
	std::string RightCol;
	std::vector<FilterTriple> Filters;
};

/** Columnar scan + SIMD filter; returns true when a fast path handled the request. */
bool ExecuteFusedScanFilter(Database &Db, const FusedScanFilterParams &Params, RowTable &Out);

/** Scan, filter, GROUP BY + SUM via AggEngine / lazy bulk. */
bool ExecuteFusedScanFilterAggSum(Database &Db, const FusedScanFilterAggSumParams &Params, RowTable &Out,
                                  std::uint64_t *RowsScannedOut = nullptr);

/** Scan + filter + agg + LIMIT/OFFSET without full materialization when possible. */
bool ExecuteFusedScanFilterAggLimit(Database &Db, const FusedScanFilterAggLimitParams &Params, RowTable &Out,
                                    std::uint64_t *RowsScannedOut = nullptr);

/** Inner join with post-join filter pushdown. */
bool ExecuteFusedJoinFilter(Database &Db, const FusedJoinFilterParams &Params, RowTable &Out,
                            std::uint64_t *RowsScannedOut = nullptr);

/** Merge consecutive FILTER_DNF stacks on lazy bulk tables. */
bool ExecuteFusedFilterMerge(Database &Db, const std::string &Table, const std::vector<FilterTriple> &Filters,
                           RowTable &Out);

class BytecodeInterpreter;
struct Instruction;

/** VM dispatch for FUSED_* opcodes. Returns false if operands invalid. */
bool HandleFusedOpcode(BytecodeInterpreter &Vm, const Instruction &Inst);

} // namespace SQL
} // namespace AstralDB
