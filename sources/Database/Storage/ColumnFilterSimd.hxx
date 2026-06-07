#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace AstralDB {

enum class FilterCompareOp : uint8_t { Eq, Ne, Gt, Ge, Lt, Le };

/** SIMD filter on parsed int64 columns; returns selected row indices (AVX-512 / AVX2 / SVE / NEON). */
void FilterI64Simd(const int64_t *Values, std::size_t Count, FilterCompareOp Op, int64_t Literal,
                   std::vector<std::size_t> &OutIndices);

/** Legacy name — delegates to \c FilterI64Simd . */
inline void FilterI64Avx2(const int64_t *Values, std::size_t Count, FilterCompareOp Op, int64_t Literal,
                          std::vector<std::size_t> &OutIndices) {
	FilterI64Simd(Values, Count, Op, Literal, OutIndices);
}

/** Sum / min / max on int64 column (SIMD when available). */
int64_t SumI64Avx2(const int64_t *Values, std::size_t Count);
int64_t MinI64Avx2(const int64_t *Values, std::size_t Count);
int64_t MaxI64Avx2(const int64_t *Values, std::size_t Count);

/** Parse string column to int64 scratch (for SIMD paths). */
void ParseColumnToI64(const std::vector<std::string> &Cells, std::vector<int64_t> &Out);

/** Scan precomputed pass bitset; append surviving row indices (0-based). */
void ScanPassBitsIndices(const std::uint64_t *Bits, std::size_t RowCount, std::vector<std::size_t> &OutIndices);

/** O(passing) scan using insert-time sparse word index. */
void ScanPassBitsIndicesSparse(const std::uint64_t *Bits, const std::uint32_t *SparseWords, std::size_t SparseWordCount,
                               std::size_t RowCount, std::vector<std::size_t> &OutIndices);

/** Skip 64K row groups with zero passes; scan non-zero words inside active groups. */
void ScanPassBitsGroupWalk(const std::uint64_t *Bits, const std::uint32_t *GroupPassCounts, std::size_t GroupCount,
                           std::size_t RowCount, std::vector<std::size_t> &OutIndices);

/** Per-slot warehouse GROUP BY accumulator (country_idx * cat_count + category_idx). */
struct WarehouseLutAggSlot {
	std::int64_t Cnt = 0;
	double Sum = 0;
};

/** Fused pass-bit scan + LUT group aggregate (no hash map, no index materialization). */
void FusedPassBitWarehouseLutAgg(const std::uint64_t *Bits, const std::uint32_t *GroupPassCounts,
                                 std::size_t RowCount, std::size_t GroupCount, const std::uint8_t *SlotByRow,
                                 const double *AmountByRow, WarehouseLutAggSlot *Slots, std::size_t SlotCount);

/** Parallel row-group partition of \c FusedPassBitWarehouseLutAgg when JobSystem is active. */
void FusedPassBitWarehouseLutAggParallel(const std::uint64_t *Bits, const std::uint32_t *GroupPassCounts,
                                         std::size_t RowCount, std::size_t GroupCount, const std::uint8_t *SlotByRow,
                                         const double *AmountByRow, WarehouseLutAggSlot *Slots, std::size_t SlotCount);

/** Direct pass-row aggregate from insert-time dense pass lists (O(pass) not O(rows)). */
void FusedPassRowDirectAgg(const std::uint8_t *PassSlots, const double *PassAmounts, std::size_t PassCount,
                           WarehouseLutAggSlot *Slots, std::size_t SlotCount);

void FusedPassRowDirectAggParallel(const std::uint8_t *PassSlots, const double *PassAmounts, std::size_t PassCount,
                                   WarehouseLutAggSlot *Slots, std::size_t SlotCount);

/** Batch scan one row-group slice of pass bits (SIMD prefetch when available). */
void ScanPassBitsGroup(const std::uint64_t *Bits, std::size_t GroupStart, std::size_t GroupEnd, std::size_t RowCount,
                       std::vector<std::size_t> &OutIndices);

/** Top-K row indices from a pass bitset without materializing all passing indices. */
struct PassBitTopKParams {
	const std::uint64_t *Bits = nullptr;
	std::size_t RowCount = 0;
	std::size_t K = 0;
	bool Ascending = false;
	/** When non-null, \c Keys[RowIndex] is the sort key (must cover \c RowCount). */
	const float *Keys = nullptr;
	/** When \c Keys is null and \c PhysicalRankStep != 0, rank from \c PhysicalRankStart + row * step. */
	int64_t PhysicalRankStart = 0;
	int64_t PhysicalRankStep = 0;
	/** Used when \c Keys is null and \c PhysicalRankStep == 0. */
	std::function<float(std::size_t RowIndex)> KeyFn;
	/** Insert-time sparse pass-bit word index (preferred walk). */
	const std::uint32_t *SparsePassWords = nullptr;
	std::size_t SparsePassWordCount = 0;
	/** Per 64K-row group pass counts; skips zero-pass groups when sparse list is empty. */
	const std::uint32_t *PassGroupCounts = nullptr;
	std::size_t PassGroupCount = 0;
	/** When non-zero, precomputed pass-row count for algorithm selection. */
	std::uint64_t KnownPassCount = 0;
	/** Optional per 64K group max rank (size = PassGroupCount); skips groups in bucket collect. */
	const float *PassGroupMaxRank = nullptr;
};

void SelectTopKFromPassBits(const PassBitTopKParams &Params, std::vector<std::size_t> &OutSortedRowIndices);

/** One sparse/bit walk + fixed-size heap (DESC/ASC with dense \c Keys). */
void SelectTopKFromPassBitsHeap(const PassBitTopKParams &Params, std::vector<std::size_t> &OutSortedRowIndices);

/** O(K) DESC top-K by physical row index (reverse sparse/bit walk). */
void SelectTopKPhysicalDescReverseWalk(const PassBitTopKParams &Params, std::vector<std::size_t> &OutSortedRowIndices);

} // namespace AstralDB
