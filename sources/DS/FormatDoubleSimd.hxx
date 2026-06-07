#pragma once

#include <Database/Storage/Superfetch.hxx>

#include <cstddef>
#include <cstdint>
#include <future>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AstralDB::FormatDoubleSimd {

/** Contiguous formatted decimals (BLIS panel output); avoids per-cell \c std::string allocs. */
struct FormattedDoubleColumn {
	std::vector<char> Chars;
	std::vector<std::uint32_t> Offsets;
	std::vector<std::uint16_t> Lengths;

	void Clear() noexcept;
	void Reserve(std::size_t RowCount, std::size_t AvgCharsPerRow = 8) noexcept;
	[[nodiscard]] std::string_view View(std::size_t Index) const noexcept;
	void AssignToVector(std::vector<std::string> &Out) const;
};

/** Single rounded double (window / aggregate path). */
void FormatRounded(double Value, std::string &Out) noexcept;

/** Panel-tiled column format with Superfetch + optional async L2 panels. */
void FormatRoundedColumn(const double *Values, std::size_t Count, FormattedDoubleColumn &Out,
                         WorkloadClass Workload = WorkloadClass::OlapWindow) noexcept;

void FormatRoundedColumn(const double *Values, std::size_t Count, std::vector<std::string> &Out,
                         WorkloadClass Workload = WorkloadClass::OlapWindow) noexcept;

/** Rank/relevance f32 column (milli-scale, no double round-trip). */
void FormatRankF32Column(const float *Values, std::size_t Count, FormattedDoubleColumn &Out,
                         WorkloadClass Workload = WorkloadClass::OlapScan) noexcept;

/** Format rank from scattered \c Keys[RowIndices[i]] (no dense gather buffer). */
void FormatRankF32GatherIndices(const float *Keys, const std::uint32_t *RowIndices, std::size_t Count,
                                FormattedDoubleColumn &Out, WorkloadClass Workload = WorkloadClass::OlapScan) noexcept;

/** Format one rank value as \c 0.xxx (five chars). */
void FormatOneRankMilli(float Value, char *Buf, std::uint16_t &Len) noexcept;

/** Async L2-panel format; merges into \a Out on completion. */
std::future<void> FormatRoundedColumnAsync(const double *Values, std::size_t Count, FormattedDoubleColumn &Out,
                                           WorkloadClass Workload = WorkloadClass::OlapWindow);

/**
 * Zip preformatted columns into row store (columns must be same length).
 * Uses \c Item::reserve per row to cut map rehash cost.
 */
void MaterializeColumnarZip(const std::vector<std::string> &ColNames,
                            const std::vector<const FormattedDoubleColumn *> &Cols, std::size_t RowCount,
                            std::vector<std::unordered_map<std::string, std::string>> &OutRows);

/** Zip string columns and panel-formatted columns into row maps (same row order). */
void MaterializeMixedZip(const std::vector<std::string> &ColNames,
                         const std::vector<const std::vector<std::string> *> &StringCols,
                         const std::vector<const FormattedDoubleColumn *> &FormattedCols,
                         std::size_t FormattedColBegin, std::size_t RowCount,
                         std::vector<std::unordered_map<std::string, std::string>> &OutRows);

/** Best-effort async write of contiguous column bytes (client push / pipe). */
std::future<std::size_t> AsyncWriteColumn(const FormattedDoubleColumn &Col, int FileDescriptor) noexcept;

} // namespace AstralDB::FormatDoubleSimd
