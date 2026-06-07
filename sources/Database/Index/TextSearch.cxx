#include <Database/Index/TextSearch.hxx>

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace AstralDB {
namespace TextSearch {

std::string FoldAscii(std::string_view Text) {
	std::string Out;
	Out.reserve(Text.size());
	for(unsigned char C : Text)
		Out.push_back(static_cast<char>(std::tolower(C)));
	return Out;
}

std::vector<std::string> TokenizeQuery(std::string_view Query) {
	std::vector<std::string> Out;
	std::string Cur;
	auto Flush = [&]() {
		if(!Cur.empty()) {
			Out.push_back(std::move(Cur));
			Cur.clear();
		}
	};
	for(unsigned char C : Query) {
		if(std::isspace(C) || std::ispunct(C)) {
			Flush();
			continue;
		}
		Cur.push_back(static_cast<char>(std::tolower(C)));
	}
	Flush();
	return Out;
}

std::vector<std::string> OrBranches(std::string_view Query) {
	std::vector<std::string> Out;
	std::string Cur;
	bool InQuote = false;
	for(size_t I = 0; I < Query.size(); ++I) {
		const char C = Query[I];
		if(C == '"') {
			InQuote = !InQuote;
			Cur.push_back(C);
			continue;
		}
		if(!InQuote && C == '|') {
			if(!Cur.empty())
				Out.push_back(Cur);
			Cur.clear();
			continue;
		}
		Cur.push_back(C);
	}
	if(!Cur.empty())
		Out.push_back(Cur);
	if(Out.empty() && !Query.empty())
		Out.emplace_back(Query);
	return Out;
}

std::vector<std::string> DistinctTerms(std::string_view Text) {
	const std::vector<std::string> Terms = TokenizeQuery(Text);
	std::vector<std::string> Out;
	std::unordered_set<std::string> Seen;
	Seen.reserve(Terms.size());
	for(const std::string &T : Terms) {
		if(Seen.insert(T).second)
			Out.push_back(T);
	}
	return Out;
}

bool ContainsAllTerms(std::string_view Haystack, std::string_view Query) {
	thread_local std::string CachedQuery;
	thread_local std::vector<std::string> CachedTerms;
	const std::vector<std::string> *TermsPtr = nullptr;
	if(CachedQuery == Query) {
		TermsPtr = &CachedTerms;
	} else {
		CachedQuery.assign(Query);
		CachedTerms = TokenizeQuery(Query);
		TermsPtr = &CachedTerms;
	}
	const std::vector<std::string> &Terms = *TermsPtr;
	if(Terms.empty())
		return false;
	const std::string Folded = FoldAscii(Haystack);
	for(const std::string &Term : Terms) {
		if(Folded.find(Term) == std::string::npos)
			return false;
	}
	return true;
}

bool MatchesQuery(std::string_view Haystack, std::string_view Query) {
	const std::vector<std::string> Branches = OrBranches(Query);
	if(Branches.empty())
		return false;
	for(const std::string &Branch : Branches) {
		if(ContainsAllTerms(Haystack, Branch))
			return true;
	}
	return false;
}

double RankScore(std::string_view Haystack, std::string_view Query) {
	const std::vector<std::string> Branches = OrBranches(Query);
	if(Branches.empty())
		return 0.0;
	const std::vector<std::string> Terms = TokenizeQuery(Branches.front());
	if(Terms.empty())
		return 0.0;
	const std::string Folded = FoldAscii(Haystack);
	std::size_t Hits = 0;
	for(const std::string &Term : Terms) {
		if(Folded.find(Term) != std::string::npos)
			++Hits;
	}
	return static_cast<double>(Hits) / static_cast<double>(Terms.size());
}

bool MatchAgainst(std::string_view Haystack, std::string_view Query) {
	const std::string Folded = FoldAscii(Haystack);
	std::string_view Q = Query;
	bool Any = false;
	while(!Q.empty()) {
		while(!Q.empty() && std::isspace(static_cast<unsigned char>(Q.front())))
			Q.remove_prefix(1);
		if(Q.empty())
			break;
		if(Q.front() == '"') {
			Q.remove_prefix(1);
			const size_t End = Q.find('"');
			if(End == std::string_view::npos)
				return false;
			const std::string Phrase = FoldAscii(Q.substr(0, End));
			Any = true;
			if(!Phrase.empty() && Folded.find(Phrase) == std::string::npos)
				return false;
			Q.remove_prefix(End + 1);
			continue;
		}
		size_t I = 0;
		while(I < Q.size() && !std::isspace(static_cast<unsigned char>(Q[I])) && Q[I] != '"')
			++I;
		const std::string Term = FoldAscii(Q.substr(0, I));
		Q.remove_prefix(I);
		if(Term.empty())
			continue;
		Any = true;
		if(Folded.find(Term) == std::string::npos)
			return false;
	}
	return Any;
}

} // namespace TextSearch
} // namespace AstralDB
