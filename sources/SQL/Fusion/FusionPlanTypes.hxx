#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace AstralDB {
namespace SQL {

enum class FusionKind : std::uint8_t {
	None = 0,
	ScanFilter,
	ScanFilterAgg,
	ScanFilterAggLimit,
	ScanFilterProjectLimit,
	JoinFilter,
	FilterMerge,
	/** Q1-shaped: customers–orders–products, filter, CUBE(4), COUNT/SUM/AVG, HAVING. */
	StarJoinCube
};

struct FusedProjectionSpec {
	std::string OutColumn;
	int16_t FnTag = 0;
	std::vector<std::pair<int64_t, std::string>> Args;
};

struct FilterTriple {
	std::string Column;
	std::string Op;
	std::string Literal;
};

struct FusionPlan {
	FusionKind Kind = FusionKind::None;
	std::string Table;
	std::string JoinRightTable;
	std::string JoinLeftCol;
	std::string JoinRightCol;
	std::vector<FilterTriple> Filters;
	std::vector<std::string> GroupKeys;
	std::string SumColumn;
	std::string SumOutputColumn;
	int64_t Limit = -1;
	int64_t Offset = 0;
	std::vector<FusedProjectionSpec> Projections;
	std::string OrderColumn;
	bool OrderAscending = true;
	std::string CustomersTable;
	std::string ProductsTable;
	std::string AvgOutputColumn;
	int64_t HavingCountMin = 0;
};

} // namespace SQL
} // namespace AstralDB
