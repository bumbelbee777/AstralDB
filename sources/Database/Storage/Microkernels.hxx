#pragma once

#include <Database/Database.hxx>
#include <Database/Storage/HybridTable.hxx>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace AstralDB {

struct ColumnarTable;

namespace Microkernels {

/** Fixed 6-wide ROWS BETWEEN 5 PRECEDING AND CURRENT ROW on bulk synthetic partition/amount. */
bool SlidingSumBulkSynthetic6(ColumnarTable &Col, const std::string &OutCol, std::size_t PrecedingRows,
                             int64_t PartitionMod, bool SkipOutputStore = false) noexcept;

void FormatSumString(std::string &Out, double Sum) noexcept;

unsigned WindowUnroll() noexcept;
unsigned FormatBatchSize() noexcept;

/** Write \c count row ids \c start + i*step into \a Out (SIMD-friendly scalar loop). */
void FormatRowIdColumn(int64_t StartId, int64_t Step, std::size_t Count, std::vector<std::string> &Out);

/** Sum of arithmetic sequence \c start + i*step for \c count terms (AVX2 when available). */
int64_t SumArithmeticSequenceI64(int64_t StartId, int64_t Step, std::size_t Count) noexcept;

/**
 * Inner join match count for lazy bulk warehouse shapes: equal row counts, join on aligned FK/PK ints,
 * no residual WHERE on either side.
 */
bool TryLazyBulkInnerJoinMatchCount(const ColumnarTable &Left, const ColumnarTable &Right,
                                    const std::vector<Database::Column> &LeftSchema,
                                    const std::vector<Database::Column> &RightSchema, const std::string &LeftCol,
                                    const std::string &RightCol, std::uint64_t &OutMatchCount,
                                    std::uint64_t *RowsScannedOut) noexcept;

/** Materialize first \a MaxRows lazy-bulk rows into \a Out (uses batch row-id formatting when possible). */
void MaterializeLazyBulkPrefix(const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                               std::size_t MaxRows, HybridTableSlot::Table &Out);

/**
 * Batched materialization when \c BulkSyntheticSlidingSumByRow is populated (nuke-style window).
 * Requires physical-order lazy bulk and predicates that match every row.
 */
bool TryMaterializeLazyBulkWindow(const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                                  std::size_t MaxRows, HybridTableSlot::Table &Out) noexcept;

/**
 * Columnar projection for nuke-style window output (no \c RowStore maps).
 * Sum stays in \c BulkSyntheticSlidingSumByRow; optional \c acct string column when \a MaterializeAcct.
 */
bool PublishLazyBulkWindowColumnar(ColumnarTable &Col, const std::vector<Database::Column> &Schema, std::size_t MaxRows,
                                   bool MaterializeAcct) noexcept;

/** LIMIT + SELECT finalize: logical result count only (window doubles already stored). */
bool CommitLazyBulkWindowProjection(ColumnarTable &Col, std::size_t MaxRows) noexcept;

/** Panel-format \c BulkSyntheticSlidingSumColumn up to \a MaxRows into \c FormattedColumns (lazy). */
void EnsureLazyBulkWindowFormatted(ColumnarTable &Col, std::size_t MaxRows) noexcept;

void FormatSumStringColumn(const double *Values, std::size_t Count, std::vector<std::string> &Out) noexcept;

void FormatAcctFkColumn(int64_t StartId, int64_t Step, int64_t PartitionMod, std::size_t Count,
                        std::vector<std::string> &Out) noexcept;

} // namespace Microkernels
} // namespace AstralDB
