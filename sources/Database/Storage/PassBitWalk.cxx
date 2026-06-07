#include <Database/Storage/PassBitWalk.hxx>

#include <Database/Storage/BulkSyntheticPrecompute.hxx>

namespace AstralDB {

PassBitWordWalk PassBitWordWalk::FromColumn(const ColumnarTable &Col) noexcept {
	PassBitWordWalk Walk;
	if(!Col.BulkSyntheticPassBits.empty()) {
		Walk.Bits = Col.BulkSyntheticPassBits.data();
		Walk.RowCount = Col.RowCount;
	}
	if(!Col.BulkSyntheticPassSparseWords.empty()) {
		Walk.SparseWords = Col.BulkSyntheticPassSparseWords.data();
		Walk.SparseWordCount = Col.BulkSyntheticPassSparseWords.size();
	}
	if(!Col.BulkSyntheticPassGroupCounts.empty()) {
		Walk.GroupCounts = Col.BulkSyntheticPassGroupCounts.data();
		Walk.GroupCount = Col.BulkSyntheticPassGroupCounts.size();
	}
	return Walk;
}

} // namespace AstralDB
