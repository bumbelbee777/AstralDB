#pragma once

#include <Database/Database.hxx>
#include <Database/Storage/BulkSyntheticSemistructured.hxx>
#include <Database/Storage/ColumnarStorage.hxx>

#include <cstdint>
#include <string>
#include <vector>

namespace AstralDB {

struct LazyStarJoinSelectParams {
	std::string WorkTable;
	std::string FactTable;
	std::vector<std::string> DimTables;
	BulkWhereDnf Filters;
	std::vector<SemistructuredProjectionSpec> Projections;
	std::vector<std::string> PassthroughCols;
	std::string OrderCol;
	bool OrderAscending = true;
	std::string OrderCol2;
	bool OrderAscending2 = true;
	std::size_t Limit = 0;
};

[[nodiscard]] float BulkSyntheticReviewMatchF32Row(int64_t RowId) noexcept;

[[nodiscard]] bool ExecuteLazyStarJoinSelect(Database &Db, const LazyStarJoinSelectParams &Params, RowTable &Out,
                                             std::uint64_t *RowsScannedOut, bool *ColumnarCommittedOut = nullptr);

struct LazyStarJoinGroupParams {
	std::string WorkTable;
	std::string FactTable;
	std::vector<std::string> DimTables;
	BulkWhereDnf Filters;
	std::vector<std::string> GroupKeys;
	SQL::Instruction GroupInst;
	std::vector<SemistructuredProjectionSpec> ComputedScalars;
	std::string OrderCol;
	bool OrderDescending = false;
	std::size_t Limit = 0;
};

[[nodiscard]] bool ExecuteLazyStarJoinGroup(Database &Db, const LazyStarJoinGroupParams &Params, RowTable &Out,
                                            std::uint64_t *RowsScannedOut, bool *ColumnarCommittedOut = nullptr,
                                            bool *PrecomputedOrderOut = nullptr);

} // namespace AstralDB
