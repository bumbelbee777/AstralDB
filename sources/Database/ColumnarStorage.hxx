#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace AstralDB {
namespace SQL {
struct Instruction;
}

using RowItem = std::unordered_map<std::string, std::string>;
using RowTable = std::vector<RowItem>;

/** Column-major replica of a row table for analytics scans. */
struct ColumnarTable {
	std::unordered_map<std::string, std::vector<std::string>> Columns;
	std::size_t RowCount = 0;

	void RebuildFromRows(const RowTable &Rows);
	RowTable MaterializeAllRows() const;
};

/** Column-oriented GROUP BY fast path for SUM/MIN/MAX/AVG + optional COUNT(*). */
struct ColumnarGroupBy {
	static bool TryRun(RowTable &Tbl, const SQL::Instruction &Inst, const std::vector<std::string> &ActiveKeys);
};

} // namespace AstralDB
