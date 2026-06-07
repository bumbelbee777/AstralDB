#include <Database/Storage/TableIndexAccess.hxx>

namespace AstralDB {

std::optional<SimplePredicate> ParseSingleIndexedPredicate(
    const std::vector<std::vector<std::tuple<std::string, std::string, std::string>>> &Branches) {
	if(Branches.size() != 1 || Branches[0].size() != 1)
		return std::nullopt;
	const auto &[Col, Op, Val] = Branches[0][0];
	if(Op == "=" || Op == "==" || Op == ">" || Op == ">=" || Op == "<" || Op == "<=" || Op == "BETWEEN")
		return SimplePredicate{Col, Op, Val};
	return std::nullopt;
}

void EnsureTableBTreeIndexesAssumeLocked(Database &Db, const std::string &TableName) {
	Db.EnsureBTreeIndexesForTableAssumeLocked(TableName);
}

void RebuildTableBTreeIndexesAssumeLocked(Database &Db, const std::string &TableName) {
	Db.RebuildBTreeIndexesForTableAssumeLocked(TableName);
}

std::optional<std::vector<std::size_t>> LookupRowsByBTreeIndexAssumeLocked(Database &Db, const std::string &TableName,
                                                                        const SimplePredicate &Pred) {
	return Db.LookupRowsByBTreeAssumeLocked(TableName, Pred.Column, Pred.Op, Pred.Value);
}

bool TryLimitRowsByPrimaryIndexAssumeLocked(Database &Db, const std::string &TableName, std::size_t LimitCount,
                                            Database::Table &OutRows) {
	return Db.TryLimitByPrimaryIndexAssumeLocked(TableName, LimitCount, OutRows);
}

} // namespace AstralDB
