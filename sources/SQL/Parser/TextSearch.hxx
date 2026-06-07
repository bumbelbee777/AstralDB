#pragma once

#include <Database/Index/TextSearch.hxx>

namespace AstralDB::SQL::TextSearch {
using AstralDB::TextSearch::FoldAscii;
using AstralDB::TextSearch::TokenizeQuery;
using AstralDB::TextSearch::OrBranches;
using AstralDB::TextSearch::DistinctTerms;
using AstralDB::TextSearch::ContainsAllTerms;
using AstralDB::TextSearch::MatchesQuery;
using AstralDB::TextSearch::RankScore;
using AstralDB::TextSearch::MatchAgainst;
} // namespace AstralDB::SQL::TextSearch
