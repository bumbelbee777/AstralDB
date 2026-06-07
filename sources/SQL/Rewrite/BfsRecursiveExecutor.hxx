#pragma once

#include <Database/Database.hxx>
#include <SQL/SQL.hxx>

#include <string>
#include <unordered_map>
#include <vector>

namespace AstralDB {
namespace SQL {

/** Level-at-a-time batch executor for recursive CTE fixpoints. */
class BfsRecursiveExecutor {
public:
	using RowTable = std::vector<std::unordered_map<std::string, std::string>>;

	RowTable Execute(const CteClause &Cte, Database &Db);

private:
	static std::string RowSignature(const std::unordered_map<std::string, std::string> &Row);
	void DeduplicateIntoWork(RowTable &Work, RowTable &Delta);
};

} // namespace SQL
} // namespace AstralDB
