#pragma once

#include <DS/FormatDoubleSimd.hxx>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AstralDB {

struct SemistructuredProjectionSpec {
	std::string OutCol;
	int FnTag = 0;
	std::vector<std::pair<int64_t, std::string>> Args;
};

/** Insert-time columnar strips for a DESC top-K winner list (arena + offsets, zero-copy views). */
struct BulkSyntheticProjectionStripPack {
	std::uint32_t K = 0;
	/** Fn + args only (ignores SELECT aliases); matches insert-time warehouse discover layout. */
	std::uint64_t SemanticsFingerprint = 0;
	std::uint64_t LayoutFingerprint = 0;
	std::vector<std::string> ColumnNames;
	std::vector<FormatDoubleSimd::FormattedDoubleColumn> Columns;
};

/** Insert-time physical cell strips keyed by \c SemistructuredCellStripKey (fn + path/pattern). */
struct BulkSyntheticWinnerCellStrips {
	std::uint32_t K = 0;
	std::unordered_map<std::uint64_t, FormatDoubleSimd::FormattedDoubleColumn> Cells;
};

} // namespace AstralDB
