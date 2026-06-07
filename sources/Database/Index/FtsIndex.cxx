#include <Database/Index/FtsIndex.hxx>

#include <Database/Index/TextSearch.hxx>

#include <algorithm>
#include <iterator>

namespace AstralDB {

void FtsIndex::InsertPosting(std::vector<int64_t> &List, const int64_t RowId) {
	if(List.empty() || List.back() < RowId) {
		List.push_back(RowId);
		return;
	}
	auto It = std::lower_bound(List.begin(), List.end(), RowId);
	if(It == List.end() || *It != RowId)
		List.insert(It, RowId);
}

void FtsIndex::ErasePosting(std::vector<int64_t> &List, const int64_t RowId) {
	auto It = std::lower_bound(List.begin(), List.end(), RowId);
	if(It != List.end() && *It == RowId)
		List.erase(It);
}

void FtsIndex::Clear() {
	Postings_.clear();
}

void FtsIndex::Insert(const int64_t RowId, const std::string_view Text) {
	const std::vector<std::string> Terms = TextSearch::DistinctTerms(Text);
	for(const std::string &Term : Terms)
		InsertPosting(Postings_[Term], RowId);
}

void FtsIndex::Remove(const int64_t RowId, const std::string_view Text) {
	const std::vector<std::string> Terms = TextSearch::DistinctTerms(Text);
	for(const std::string &Term : Terms) {
		auto It = Postings_.find(Term);
		if(It != Postings_.end())
			ErasePosting(It->second, RowId);
	}
}

std::vector<int64_t> FtsIndex::Search(const std::string_view Keywords) const {
	const std::vector<std::string> Branches = TextSearch::OrBranches(Keywords);
	if(Branches.empty())
		return {};
	std::vector<int64_t> Union;
	for(const std::string &Branch : Branches) {
		const std::vector<std::string> Terms = TextSearch::TokenizeQuery(Branch);
		if(Terms.empty())
			continue;
		std::vector<int64_t> Acc;
		bool First = true;
		for(const std::string &Term : Terms) {
			auto It = Postings_.find(Term);
			if(It == Postings_.end() || It->second.empty()) {
				Acc.clear();
				break;
			}
			const std::vector<int64_t> &Post = It->second;
			if(First) {
				Acc = Post;
				First = false;
				continue;
			}
			std::vector<int64_t> Inter;
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
			std::vector<int64_t> Merged;
			Merged.reserve(Union.size() + Acc.size());
			std::set_union(Union.begin(), Union.end(), Acc.begin(), Acc.end(), std::back_inserter(Merged));
			Union = std::move(Merged);
		}
	}
	return Union;
}

} // namespace AstralDB
