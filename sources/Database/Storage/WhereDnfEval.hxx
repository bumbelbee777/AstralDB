#pragma once

#include <Database/Database.hxx>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace AstralDB {

/** Evaluate a packed FILTER_Dnf blob against a row map. false if blob invalid. */
bool EvaluatePackedWhereDnf(const Database *Db, const std::unordered_map<std::string, std::string> &Row,
                            std::string_view PackedDnfBlob);

using WherePredicateTriple = std::tuple<std::string, std::string, std::string>;
bool MatchWhereDnfBranches(const Database *Db, const std::string &ContextTable,
                           const std::unordered_map<std::string, std::string> &Row,
                           const std::vector<std::vector<WherePredicateTriple>> &Dnf);

} // namespace AstralDB
