#pragma once

#include <Database/Database.hxx>

#include <optional>
#include <string>
#include <vector>

namespace AstralDB {

struct SimplePredicate {
	std::string Column;
	std::string Op;
	std::string Value;
};

/** Register PK / indexed columns after CREATE TABLE or bulk load. */
void EnsureTableBTreeIndexesAssumeLocked(Database &Db, const std::string &TableName);

/** Rebuild all B-tree indexes for a table from row store. */
void RebuildTableBTreeIndexesAssumeLocked(Database &Db, const std::string &TableName);

/** Single-branch equality / range on an indexed column → row indices in row store. */
[[nodiscard]] std::optional<std::vector<std::size_t>> LookupRowsByBTreeIndexAssumeLocked(
    Database &Db, const std::string &TableName, const SimplePredicate &Pred);

/** LIMIT n via PK index walk (ordered by key). */
[[nodiscard]] bool TryLimitRowsByPrimaryIndexAssumeLocked(Database &Db, const std::string &TableName,
                                                          std::size_t LimitCount, Database::Table &OutRows);

/** True when FILTER_DNF is a single indexed predicate. */
[[nodiscard]] std::optional<SimplePredicate> ParseSingleIndexedPredicate(
    const std::vector<std::vector<std::tuple<std::string, std::string, std::string>>> &Branches);

} // namespace AstralDB
