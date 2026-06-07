#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AstralDB {

/** Inverted index: lowercase term -> sorted distinct row ids. */
class TextIndex {
	std::unordered_map<std::string, std::vector<size_t>> Postings_;

	static void InsertPosting(std::vector<size_t> &List, size_t RowId);
	static void ErasePosting(std::vector<size_t> &List, size_t RowId);

public:
	void Clear();

	void IndexRow(size_t RowId, std::string_view Text);
	void RemoveRow(size_t RowId, std::string_view Text);

	/** Rows whose indexed text satisfies every token in \a Query (OR of branches, AND within branch). */
	std::vector<size_t> RowsMatchingQuery(std::string_view Query) const;

	std::size_t TermCount() const { return Postings_.size(); }
};

} // namespace AstralDB
