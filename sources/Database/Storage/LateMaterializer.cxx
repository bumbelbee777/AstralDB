#include <Database/Storage/LateMaterializer.hxx>

#include <Database/Storage/BulkSynthetic.hxx>
#include <Database/Storage/ColumnarLazyBulk.hxx>
#include <Database/Storage/ColumnarStorage.hxx>
#include <Database/Database.hxx>

#include <algorithm>

namespace AstralDB {

void LateMaterializer::RequireColumn(std::string Column) { Required_.insert(std::move(Column)); }

void LateMaterializer::MergeProjection(const LateMaterializer &Child) {
	for(const auto &C : Child.Required_)
		Required_.insert(C);
}

ColumnBatch LateMaterializer::BuildBatch(const CompressedColumnStore &Store, std::size_t Begin,
                                         std::size_t End) const {
	ColumnBatch Batch;
	Batch.BeginRow = Begin;
	Batch.EndRow = End;
	for(const auto &Col : Required_) {
		const auto *Cat = Store.FindColumn(Col);
		if(!Cat || Cat->Chunks.empty())
			continue;
		const auto &Ck = Cat->Chunks.front();
		Batch.ColumnNames.push_back(Col);
		Batch.Columns.emplace_back(Ck.Payload.data(), Ck.Payload.size(), Ck.Encoding, Ck.RowCount);
	}
	return Batch;
}

HybridTableSlot::Table LateMaterializer::MaterializeRows(const HybridTableSlot &Slot, std::size_t Begin,
                                                         std::size_t End) const {
	HybridTableSlot::Table Out;
	if(Required_.empty())
		return Out;
	const std::size_t N = End > Begin ? End - Begin : 0;
	Out.reserve(N);
	for(std::size_t R = Begin; R < End && R < Slot.RowStore.size(); ++R) {
		HybridTableSlot::Item Row;
		for(const auto &Col : Required_) {
			auto It = Slot.RowStore[R].find(Col);
			if(It != Slot.RowStore[R].end())
				Row[Col] = It->second;
		}
		Out.push_back(std::move(Row));
	}
	return Out;
}

RowTable LateMaterializer::MaterializeSelectedColumns(const ColumnarTable &Col,
                                                      const std::vector<Database::Column> &Schema,
                                                      const RowIdVector &RowIds) const {
	RowTable Out;
	Out.reserve(RowIds.size());
	const std::vector<std::string> Cols(Required_.begin(), Required_.end());
	for(const std::size_t Ri : RowIds) {
		RowItem Row;
		for(const std::string &Cn : Cols) {
			std::string Cell;
			if(ColumnarBulkCellString(Col, Schema, Cn, Ri, Cell))
				Row[Cn] = std::move(Cell);
		}
		Out.push_back(std::move(Row));
	}
	return Out;
}

RowIdVector LateMaterializer::TopKRowIds(const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                                         const RowIdVector &Candidates, const std::string &SortCol, bool Ascending,
                                         std::size_t K) const {
	RowIdVector Sorted = Candidates;
	if(Sorted.empty() || K == 0)
		return {};
	const std::size_t Limit = std::min(K, Sorted.size());
	std::partial_sort(Sorted.begin(), Sorted.begin() + static_cast<std::ptrdiff_t>(Limit), Sorted.end(),
	                  [&](std::size_t A, std::size_t B) {
		                  std::string Va;
		                  std::string Vb;
		                  ColumnarBulkCellString(Col, Schema, SortCol, A, Va);
		                  ColumnarBulkCellString(Col, Schema, SortCol, B, Vb);
		                  return Ascending ? Va < Vb : Va > Vb;
	                  });
	Sorted.resize(Limit);
	return Sorted;
}

} // namespace AstralDB
