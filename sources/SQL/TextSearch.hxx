#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {
namespace SQL {
namespace TextSearch {

/** Lowercase ASCII copy for case-insensitive term matching. */
std::string FoldAscii(std::string_view Text);

/** Whitespace/punctuation-delimited terms (empty tokens dropped). */
std::vector<std::string> TokenizeQuery(std::string_view Query);

/** Split on \c | outside double quotes (OR of AND-branches). */
std::vector<std::string> OrBranches(std::string_view Query);

/** Unique folded terms from document text (for inverted indexes). */
std::vector<std::string> DistinctTerms(std::string_view Text);

/** Every query token must appear as a substring of \a Haystack (ASCII fold). */
bool ContainsAllTerms(std::string_view Haystack, std::string_view Query);

/** Any OR-branch matches (\c ContainsAllTerms per branch). */
bool MatchesQuery(std::string_view Haystack, std::string_view Query);

/** Simple coverage score in \c [0,1] (matched term fraction of first branch). */
double RankScore(std::string_view Haystack, std::string_view Query);

/** Double-quoted spans in \a Query must appear contiguously; unquoted tokens use \c ContainsAllTerms. */
bool MatchAgainst(std::string_view Haystack, std::string_view Query);

} // namespace TextSearch
} // namespace SQL
} // namespace AstralDB
