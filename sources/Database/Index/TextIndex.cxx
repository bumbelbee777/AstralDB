#include <Database/Index/TextIndex.hxx>

#include <Database/Index/TextSearch.hxx>

#include <algorithm>
#include <iterator>

namespace AstralDB {

void TextIndex::InsertPosting(std::vector<size_t> &List, size_t RowId) {
	if(List.empty() || List.back() < RowId) {
		List.push_back(RowId);
		return;
	}
	auto It = std::lower_bound(List.begin(), List.end(), RowId);
	if(It == List.end() || *It != RowId)
		List.insert(It, RowId);
}

void TextIndex::ErasePosting(std::vector<size_t> &List, size_t RowId) {
	auto It = std::lower_bound(List.begin(), List.end(), RowId);
	if(It != List.end() && *It == RowId)
		List.erase(It);
}

void TextIndex::Clear() {
	Postings_.clear();
}

void TextIndex::IndexRow(size_t RowId, std::string_view Text) {
	const std::vector<std::string> Terms = TextSearch::DistinctTerms(Text);
	for(const std::string &Term : Terms)
		InsertPosting(Postings_[Term], RowId);
}

void TextIndex::RemoveRow(size_t RowId, std::string_view Text) {
	const std::vector<std::string> Terms = TextSearch::DistinctTerms(Text);
	for(const std::string &Term : Terms) {
		auto It = Postings_.find(Term);
		if(It != Postings_.end())
			ErasePosting(It->second, RowId);
	}
}

std::vector<size_t> TextIndex::RowsMatchingQuery(std::string_view Query) const {
	const std::vector<std::string> Branches = TextSearch::OrBranches(Query);
	if(Branches.empty())
		return {};
	std::vector<size_t> Union;
	for(const std::string &Branch : Branches) {
		const std::vector<std::string> Terms = TextSearch::TokenizeQuery(Branch);
		if(Terms.empty())
			continue;
		std::vector<size_t> Acc;
		bool First = true;
		for(const std::string &Term : Terms) {
			auto It = Postings_.find(Term);
			if(It == Postings_.end() || It->second.empty()) {
				Acc.clear();
				break;
			}
			const std::vector<size_t> &Post = It->second;
			if(First) {
				Acc = Post;
				First = false;
				continue;
			}
			std::vector<size_t> Inter;
			Inter.reserve(std::min(Acc.size(), Post.size()));
			std::set_intersection(Acc.begin(), Acc.end(), Post.begin(), Post.end(), std::back_inserter(Inter));
			Acc = std::move(Inter);
			if(Acc.empty())
				break;
		}
		if(Acc.empty())
			continue;
		if(Union.empty())
			Union = std::move(Acc);
		else {
			std::vector<size_t> Merged;
			Merged.reserve(Union.size() + Acc.size());
			std::set_union(Union.begin(), Union.end(), Acc.begin(), Acc.end(), std::back_inserter(Merged));
			Union = std::move(Merged);
		}
	}
	return Union;
}

} // namespace AstralDB
