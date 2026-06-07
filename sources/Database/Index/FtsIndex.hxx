#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AstralDB {

/** Inverted full-text index: lowercase term -> sorted distinct row ids (\c int64_t ). */
class FtsIndex {
	std::unordered_map<std::string, std::vector<int64_t>> Postings_;

	static void InsertPosting(std::vector<int64_t> &List, int64_t RowId);
	static void ErasePosting(std::vector<int64_t> &List, int64_t RowId);

public:
	void Clear();

	void Insert(int64_t RowId, std::string_view Text);
	void Remove(int64_t RowId, std::string_view Text);

	/** Rows whose indexed text satisfies every token in \a Keywords (OR of branches, AND within branch). */
	[[nodiscard]] std::vector<int64_t> Search(std::string_view Keywords) const;

	[[nodiscard]] std::size_t TermCount() const { return Postings_.size(); }
};

} // namespace AstralDB
