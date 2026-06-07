#pragma once

#include <Database/Database.hxx>
#include <Database/Storage/ColumnarStorage.hxx>
#include <Database/Storage/ColumnarStorage.hxx>
#include <Database/Execution/BytecodeTypes.hxx>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace AstralDB {

struct LazyBulkJoinGroupBy3Options {
	bool UsePrecomputed = true;
	bool LazyMaterialization = true;
	std::uint32_t VectorBatchSize = 4096;
	bool *UsedRadixGroupByOut = nullptr;
	std::function<void(std::string_view, std::chrono::nanoseconds)> RecordRegion;
};

bool TryColumnarFilterDnfLazy(ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                              const std::vector<std::vector<FilterPredicateTriple>> &Branches, const Database *Db,
                              const std::string &ContextTable, RowTable &Out, std::uint64_t *RowsScannedOut);

std::uint64_t CountBulkSyntheticRowsMatchingWhere(const ColumnarTable &Col,
                                                  const std::vector<Database::Column> &Schema, const Database *Db,
                                                  const std::string &ContextTable, bool *UsedPassBitsOut = nullptr);

void MaterializeLazyBulkToRowStore(const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                                   const Database *Db, const std::string &TableName, RowTable &Out);

bool MaterializeLazyBulkToRowStore(const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                                   const Database *Db, const std::string &TableName, RowTable &Out,
                                   std::size_t MaxRows);

bool TryColumnarInnerJoinEqualityLazy(const ColumnarTable &Left, const ColumnarTable &Right,
                                      const std::vector<Database::Column> &LeftSchema,
                                      const std::vector<Database::Column> &RightSchema,
                                      const std::string &LeftCol, const std::string &RightCol, RowTable &Result,
                                      const Database *Db, const std::string &LeftTable, const std::string &RightTable,
                                      std::uint64_t *RowsScannedOut);

bool BulkSyntheticRowPassesWhereStackFast(const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                                          std::size_t RowIndex, const Database *Db, const std::string &ContextTable);

bool TryLazyBulkGroupBy(Database *Db, ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                        const SQL::Instruction &Inst, const std::vector<std::string> &ActiveKeys, RowTable &Out);

bool TryLazyBulkStarGroupByFromBaseTables(Database &Db, const SQL::Instruction &Inst,
                                          const std::vector<std::string> &GroupKeys, RowTable &Out,
                                          std::uint64_t *RowsScannedOut = nullptr);

bool TryLazyBulkWarehouseCube(Database &Db, const SQL::Instruction &Inst, std::string_view CubeTable,
                              const std::vector<std::string> &GroupKeys, RowTable &Out,
                              std::uint64_t *RowsScannedOut);

bool ColumnarBulkCellString(const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                            const std::string &ColName, std::size_t RowIndex, std::string &Out);

/** UPDATE/DELETE on lazy synthetic bulk without RowStore materialization. */
[[nodiscard]] bool TryLazyBulkSyntheticUpdate(
    ColumnarTable &Col, const std::vector<Database::Column> &Schema,
    const std::vector<std::pair<std::string, std::string>> &Assignments,
    const std::vector<std::vector<FilterPredicateTriple>> &Branches);
[[nodiscard]] bool TryLazyBulkSyntheticDelete(ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                                              const std::vector<std::vector<FilterPredicateTriple>> &Branches);

bool TryColumnarInnerJoinEqualityLazyMatchCount(const ColumnarTable &Left, const ColumnarTable &Right,
                                                const std::vector<Database::Column> &LeftSchema,
                                                const std::vector<Database::Column> &RightSchema,
                                                const std::string &LeftCol, const std::string &RightCol,
                                                const Database *Db, const std::string &LeftTable,
                                                const std::string &RightTable, std::uint64_t &OutMatchCount,
                                                std::uint64_t *RowsScannedOut = nullptr);

bool HydrateLazyBulkColumns(ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                            const std::vector<std::string> &ColNames);

bool ApplyLazyScalarProjection(ColumnarTable &Col, const std::vector<Database::Column> &Schema, const Database *Db,
                               const std::string &ContextTable, int FnTag,
                               const std::vector<std::pair<int64_t, std::string>> &Args,
                               const std::string &OutColName) noexcept;

bool TryLazyBulkOrderByColumnar(ColumnarTable &Col, const std::string &SortCol, bool Ascending,
                                std::size_t TopKeep) noexcept;

} // namespace AstralDB
