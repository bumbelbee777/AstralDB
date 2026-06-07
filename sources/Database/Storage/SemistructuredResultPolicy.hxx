#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace AstralDB {
namespace SemistructuredResultPolicy {

/** Minimum table rows before insert-time winner cell-strip panels are built. */
[[nodiscard]] std::size_t InsertCellStripMinTableRows() noexcept;

/** Minimum LIMIT K for insert-time cell strips (smaller K skipped unless table is huge). */
[[nodiscard]] std::uint32_t InsertCellStripMinK() noexcept;

/** Minimum rows before query uses insert cell strips (else direct LUT). */
[[nodiscard]] std::size_t QueryInsertCellStripMinTableRows() noexcept;

/** Minimum rows before query may use cached projection strip packs. */
[[nodiscard]] std::size_t QueryStripPackCacheMinTableRows() noexcept;

/** Minimum winner count before parallel row materialize/zip. */
[[nodiscard]] std::size_t ParallelMaterializeMinWinners(std::size_t TableRowCount) noexcept;

[[nodiscard]] bool ShouldBuildInsertCellStrips(std::size_t TableRowCount, std::uint32_t K) noexcept;
[[nodiscard]] bool ShouldUseInsertCellStripsAtQuery(std::size_t TableRowCount, std::size_t WinnerCount) noexcept;
[[nodiscard]] bool ShouldCacheStripPackAtQuery(std::size_t TableRowCount) noexcept;

} // namespace SemistructuredResultPolicy
} // namespace AstralDB
