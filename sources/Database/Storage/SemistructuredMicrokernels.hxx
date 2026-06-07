#pragma once

#include <Database/Storage/BulkSyntheticSemistructured.hxx>
#include <Database/Storage/ColumnFilterSimd.hxx>
#include <Database/Storage/ColumnarStorage.hxx>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace AstralDB {
namespace SemistructuredMicrokernels {

/** How winner rows are materialized after semistructured top-K (shape-based, not query id). */
enum class SemistructuredMaterializeKind : std::uint8_t {
	/** Per-column batch eval + mixed zip (fallback). */
	GenericBatched = 0,
	/** Lazy bulk, physical row order: shared row-id strip + columnar batch fills. */
	PhysicalLazyColumnar = 1,
};

struct SemistructuredMaterializePlan {
	SemistructuredMaterializeKind Kind = SemistructuredMaterializeKind::GenericBatched;
};

[[nodiscard]] SemistructuredMaterializePlan ClassifySemistructuredMaterialize(
    const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
    const std::vector<SemistructuredProjectionSpec> &Projections, std::size_t WinnerCount) noexcept;

/** Physical-order rank column fill (SIMD panels + optional JobSystem). */
void FillBioRankF32Column(ColumnarTable &Col) noexcept;

/** Set \c Col.BulkSyntheticTopKBits for winner row indices (cleared then filled). */
void BuildTopKPassBits(ColumnarTable &Col, const std::vector<std::size_t> &WinnerRowIndices) noexcept;

/** Dense gather of (key, rowIndex) for pass-bit rows (sparse / group walk). */
void GatherPassBitKeys(const PassBitTopKParams &Params, std::vector<float> &OutKeys,
                       std::vector<std::uint32_t> &OutRows) noexcept;

/**
 * Top-K from pass bits: partial_sort gather when \c K is large and keys are dense;
 * otherwise delegates to \c SelectTopKFromPassBits.
 */
void SelectTopKPassBits(const PassBitTopKParams &Params, std::vector<std::size_t> &OutSortedRowIndices) noexcept;

void SelectTopKPassBitsPartialSort(const PassBitTopKParams &Params, std::vector<std::size_t> &OutSortedRowIndices);

/**
 * Panel-tiled batch materialize; rank projections use \c FormatRankF32Column (no string format on keys).
 * Dispatches \c PhysicalLazyColumnar when \c ClassifySemistructuredMaterialize matches.
 */
bool MaterializeWinnersBatched(const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                               const std::vector<SemistructuredProjectionSpec> &Projections,
                               const std::vector<std::size_t> &WinnerRowIndices, RowTable &Out) noexcept;

bool MaterializeWinnersPhysicalColumnar(const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                                        const std::vector<SemistructuredProjectionSpec> &Projections,
                                        const std::vector<std::size_t> &WinnerRowIndices, RowTable &Out) noexcept;

} // namespace SemistructuredMicrokernels
} // namespace AstralDB
