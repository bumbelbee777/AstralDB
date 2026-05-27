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

/** O(n) sliding SUM for \c ROWS BETWEEN fixed preceding and \c CURRENT ROW on columnar storage. */
bool TrySlidingSumRowsFrame(ColumnarTable &Col, const std::string &PartCol, const std::string &OrderCol,
                            const std::string &SrcCol, const std::string &OutCol, std::size_t PrecedingRows,
                            bool OrderAscending);

/** Append \p Count synthetic rows into columnar columns (schema-aware names). */
void AppendBulkSyntheticColumnar(ColumnarTable &Col, const std::vector<std::string> &ColNames, int64_t Count,
                                 int64_t StartId, int64_t Step);

} // namespace AstralDB
