#pragma once

#include <Database/Database.hxx>
#include <Database/Storage/ColumnarLazyBulk.hxx>
#include <Database/Storage/ColumnarStorage.hxx>
#include <Database/Storage/BulkSyntheticSemistructured.hxx>
#include <Database/Execution/BytecodeTypes.hxx>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace AstralDB {

struct LazyDimensionSide {
	const ColumnarTable *Table = nullptr;
	const std::vector<Database::Column> *Schema = nullptr;
	std::string TableName;
};

enum class LazyGroupKeySource : std::uint8_t {
	DimensionFk = 0,
	FactColumn = 1,
	FactTimeBucket = 2,
};

struct LazyFactGroupByBinding {
	const Database::Column *FactFk = nullptr;
	const Database::Column *GroupKeyColumn = nullptr;
	LazyDimensionSide Dimension;
	LazyGroupKeySource Source = LazyGroupKeySource::DimensionFk;
};

[[nodiscard]] std::string LazyGroupKeyCellValue(const LazyFactGroupByBinding &Binding, int64_t PrimaryRowId);

struct LazyFactGroupByPlan {
	const ColumnarTable *Fact = nullptr;
	const std::vector<Database::Column> *FactSchema = nullptr;
	std::string FactTableName;
	std::vector<LazyFactGroupByBinding> Bindings;
	std::vector<LazyDimensionSide> AllDimensions;
	std::vector<SemistructuredProjectionSpec> ComputedScalars;
	std::string SumColumn;
};

[[nodiscard]] bool BuildLazyFactGroupByPlan(const ColumnarTable &Fact, const std::vector<Database::Column> &FactSchema,
                                            std::string_view FactTableName,
                                            const std::vector<LazyDimensionSide> &Dimensions,
                                            const std::vector<std::string> &GroupKeys, LazyFactGroupByPlan &Out);

[[nodiscard]] bool TryGeneralizedLazyGroupBy(const LazyFactGroupByPlan &Plan, const SQL::Instruction &GroupInst,
                                           const std::vector<std::string> &GroupKeys, RowTable &Out,
                                           std::uint64_t *RowsScannedOut, const BulkWhereDnf *FactFilters,
                                           const LazyBulkJoinGroupBy3Options *Options);

/** Multi-combo-agg star GROUP BY (fact + N dimensions, arbitrary GROUP BY operand tail). */
[[nodiscard]] bool TryGeneralizedLazyGroupByMulti(const LazyFactGroupByPlan &Plan, const SQL::Instruction &GroupInst,
                                                  const std::vector<std::string> &GroupKeys, RowTable &Out,
                                                  std::uint64_t *RowsScannedOut, const BulkWhereDnf *FactFilters,
                                                  const LazyBulkJoinGroupBy3Options *Options,
                                                  std::string_view OrderCol = {}, bool OrderDescending = false,
                                                  std::size_t Limit = 0, bool *PrecomputedOrderOut = nullptr);

} // namespace AstralDB
