#pragma once

#include <Database/Storage/ColumnarStorage.hxx>
#include <Database/Database.hxx>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {

/** Row-id-only JSON field materialization (no JSON parse / cell string). */
bool BulkSyntheticJsonExtractRow(int64_t RowId, std::string_view Path, std::string &Out) noexcept;

/** Row-id-only XML path extract for synthetic customer documents. */
bool BulkSyntheticXmlExtractRow(int64_t RowId, std::string_view Path, std::string &Out) noexcept;

[[nodiscard]] bool BulkSyntheticXmlValidRow(int64_t RowId) noexcept;

[[nodiscard]] std::size_t BulkSyntheticBioLengthRow(int64_t RowId) noexcept;

[[nodiscard]] double BulkSyntheticBioRankRow(int64_t RowId) noexcept;

[[nodiscard]] float BulkSyntheticBioRankF32Row(int64_t RowId) noexcept;

/** Deterministic MATCH score for synthetic review_text (no string materialization). */
[[nodiscard]] float BulkSyntheticReviewMatchF32Row(int64_t RowId) noexcept;

/** Fill \c Col.BulkSyntheticLazyRankF32 when empty (physical-order fast path). */
void EnsureBulkSyntheticLazyRankF32(ColumnarTable &Col) noexcept;

bool BulkSyntheticRegexpExtractRow(int64_t RowId, std::string_view Pattern, std::string &Out) noexcept;

void CollectBulkSyntheticPassingIndices(const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                                        const Database *Db, const std::string &ContextTable,
                                        std::vector<std::size_t> &OutIndices) noexcept;

bool ApplyLazyScalarProjection(ColumnarTable &Col, const std::vector<Database::Column> &Schema, const Database *Db,
                               const std::string &ContextTable, int FnTag,
                               const std::vector<std::pair<int64_t, std::string>> &Args,
                               const std::string &OutColName) noexcept;

bool TryLazyBulkOrderByColumnar(ColumnarTable &Col, const std::string &SortCol, bool Ascending,
                                std::size_t TopKeep) noexcept;

#include <Database/Storage/SemistructuredResultStripsTypes.hxx>

bool MaterializeSemistructuredWinners(const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                                      const std::vector<SemistructuredProjectionSpec> &Projections,
                                      const std::vector<std::size_t> &WinnerRowIndices, RowTable &Out) noexcept;

/** Single-pass filter + row-id projections + numeric top-K + slim materialize (lazy bulk). */
bool ExecuteFusedSemistructuredScan(Database &Db, const std::string &Table, const BulkWhereDnf &FilterDnf,
                                    const std::vector<SemistructuredProjectionSpec> &Projections,
                                    const std::string &OrderCol, bool OrderAscending, std::size_t Limit,
                                    RowTable &Out, std::uint64_t *RowsScannedOut,
                                    bool *ColumnarCommittedOut = nullptr) noexcept;

} // namespace AstralDB
