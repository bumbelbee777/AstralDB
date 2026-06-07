#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace AstralDB {

inline constexpr std::size_t kColumnRowGroupSize = 65536;

struct ColumnMinMax {
	std::string Min;
	std::string Max;
	bool Numeric = false;
	int64_t MinI64 = 0;
	int64_t MaxI64 = 0;
	double MinF64 = 0.0;
	double MaxF64 = 0.0;
};

struct RowGroupStats {
	std::size_t StartRow = 0;
	std::size_t EndRow = 0;
	std::unordered_map<std::string, ColumnMinMax> ColumnStats;
};

/** Per-column min/max for one row group. */
void UpdateColumnMinMax(ColumnMinMax &Stats, const std::string &Value);

/** Build row-group zone maps from column vectors. */
void RebuildRowGroupZoneMaps(std::vector<RowGroupStats> &Out,
                             const std::unordered_map<std::string, std::vector<std::string>> &Columns,
                             std::size_t RowCount);

/** True if the row group might contain a row matching (col op val). Conservative on unknown types. */
[[nodiscard]] bool RowGroupMayContain(const RowGroupStats &Group, const std::string &Col, const std::string &Op,
                                      const std::string &Val);

/** Row indices in [0, RowCount) whose groups may match the predicate. */
[[nodiscard]] std::vector<std::size_t> RowGroupsToScan(const std::vector<RowGroupStats> &Groups, std::size_t RowCount,
                                                       const std::string &Col, const std::string &Op,
                                                       const std::string &Val);

} // namespace AstralDB
