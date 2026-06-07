#pragma once

#include <Database/Database.hxx>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace AstralDB {

struct MaterializedPathEntry {
	std::string Path;
	int Depth = 0;
};

/** Per-table parent/child hierarchy index for recursive CTE acceleration. */
class HierarchyPathIndex {
public:
	void Clear();
	/** Build from rows with \p IdCol and \p ParentCol (parent empty or "0" for roots). */
	bool TryBuildFromTable(const Database::Table &Rows, const std::string &IdCol, const std::string &ParentCol);
	[[nodiscard]] bool Empty() const noexcept { return ById_.empty(); }
	[[nodiscard]] std::optional<MaterializedPathEntry> Lookup(const std::string &Id) const;
	void Insert(const std::string &Id, MaterializedPathEntry Ent);

private:
	std::unordered_map<std::string, MaterializedPathEntry> ById_;
};

/** Detect id/parent_id columns and accelerate recursive fixpoint merge when possible. */
bool TryRecursiveCteHierarchyMerge(Database &Db, const std::string &WorkTable, const std::string &DeltaTable,
                                   HierarchyPathIndex &Index);

} // namespace AstralDB
