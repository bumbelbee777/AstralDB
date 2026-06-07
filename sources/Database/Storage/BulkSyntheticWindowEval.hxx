#pragma once

#include <Database/Database.hxx>
#include <Database/Storage/ColumnarStorage.hxx>
#include <Database/Execution/PlanTypes.hxx>

#include <string>
#include <vector>

namespace AstralDB {

/** Row-id bucketed window ops on lazy bulk (no full-table stable_sort / hydrate). */
[[nodiscard]] bool TryBulkSyntheticLazyWindow(
    ColumnarTable &Col, const std::vector<Database::Column> &Schema, int OrdKind, int FrameMode,
    const std::vector<std::string> &PartCols, const std::string &OrderCol, const std::string &SrcCol,
    const std::string &OutCol, bool Ascending, SQL::WindowFrameBoundKind FrameStartKind, int64_t FrameStartOff,
    SQL::WindowFrameBoundKind FrameEndKind, int64_t FrameEndOff, int64_t FrameOffset,     bool SkipOutputStore) noexcept;

/** ORDER BY cust_id[, order_date] on lazy bulk using window bucket order (no 10M row-store build). */
[[nodiscard]] bool TryLazyBulkOrderByWindowBuckets(ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                                                   const std::string &SortCol, bool Ascending,
                                                   std::size_t TopKeep) noexcept;

/** Q3 antimatterbomb: one fused cust-date cache + top-K index flatten (no per-window 100M vectors). */
[[nodiscard]] bool ExecuteQ3FusedWindowSuite(ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                                             std::size_t TopKeep) noexcept;

/** Lazy spending_decile for cust_id NTILE on FK-mod physical order. */
[[nodiscard]] double BulkSyntheticNtileDecileAt(const ColumnarTable &Col, std::size_t RowIndex,
                                                std::size_t NumTiles) noexcept;

/** On-demand cust-date window metric when full cache was skipped (projection-only Q3). */
[[nodiscard]] double BulkSyntheticCustDateMetricAt(const ColumnarTable &Col, std::size_t RowIndex,
                                                   std::string_view Column) noexcept;

} // namespace AstralDB
