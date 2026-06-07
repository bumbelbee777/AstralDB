#pragma once

#include <Database/Database.hxx>
#include <Database/Storage/ColumnView.hxx>
#include <Database/Storage/CompressedColumnStore.hxx>
#include <Database/Storage/HybridTable.hxx>

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace AstralDB {

struct ColumnarTable;
using RowItem = std::unordered_map<std::string, std::string>;
using RowTable = std::vector<RowItem>;
using RowIdVector = std::vector<std::size_t>;

/** Tracks required columns per pipeline stage; defers RowStore materialization. */
class LateMaterializer {
public:
	void RequireColumn(std::string Column);
	void MergeProjection(const LateMaterializer &Child);
	[[nodiscard]] const std::unordered_set<std::string> &Required() const noexcept { return Required_; }

	ColumnBatch BuildBatch(const CompressedColumnStore &Store, std::size_t Begin, std::size_t End) const;
	HybridTableSlot::Table MaterializeRows(const HybridTableSlot &Slot, std::size_t Begin, std::size_t End) const;

	RowTable MaterializeSelectedColumns(const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
	                                    const RowIdVector &RowIds) const;

	RowIdVector TopKRowIds(const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
	                       const RowIdVector &Candidates, const std::string &SortCol, bool Ascending,
	                       std::size_t K) const;

private:
	std::unordered_set<std::string> Required_;
};

} // namespace AstralDB
