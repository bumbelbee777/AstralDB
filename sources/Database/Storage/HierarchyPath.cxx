#include <Database/Storage/HierarchyPath.hxx>

#include <unordered_set>

namespace AstralDB {

void HierarchyPathIndex::Insert(const std::string &Id, MaterializedPathEntry Ent) { ById_[Id] = std::move(Ent); }

void HierarchyPathIndex::Clear() { ById_.clear(); }

bool HierarchyPathIndex::TryBuildFromTable(const Database::Table &Rows, const std::string &IdCol,
                                           const std::string &ParentCol) {
	Clear();
	if(Rows.empty())
		return false;
	for(const Database::Item &Row : Rows) {
		const auto IdIt = Row.find(IdCol);
		if(IdIt == Row.end() || IdIt->second.empty())
			continue;
		const auto ParIt = Row.find(ParentCol);
		const std::string Parent = ParIt == Row.end() ? "" : ParIt->second;
		MaterializedPathEntry Ent;
		if(Parent.empty() || Parent == "0") {
			Ent.Path = "/" + IdIt->second;
			Ent.Depth = 0;
		} else {
			const auto Pit = ById_.find(Parent);
			if(Pit == ById_.end())
				return false;
			Ent.Path = Pit->second.Path + "/" + IdIt->second;
			Ent.Depth = Pit->second.Depth + 1;
		}
		ById_[IdIt->second] = std::move(Ent);
	}
	return !ById_.empty();
}

std::optional<MaterializedPathEntry> HierarchyPathIndex::Lookup(const std::string &Id) const {
	const auto It = ById_.find(Id);
	if(It == ById_.end())
		return std::nullopt;
	return It->second;
}

namespace {

bool FindHierarchyColumns(const Database::Schema &Sch, std::string &IdCol, std::string &ParentCol) {
	for(const Database::Column &C : Sch) {
		if(C.IsPrimaryKey)
			IdCol = C.Name;
		if(C.Name == "id" && IdCol.empty())
			IdCol = C.Name;
		if(C.Name == "parent_id" || C.Name == "parent" || C.Name == "manager_id")
			ParentCol = C.Name;
	}
	return !IdCol.empty() && !ParentCol.empty() && IdCol != ParentCol;
}

} // namespace

bool TryRecursiveCteHierarchyMerge(Database &Db, const std::string &WorkTable, const std::string &DeltaTable,
                                   HierarchyPathIndex &Index) {
	const auto Sch = Db.TableSchemaAssumeDbMutexHeld(WorkTable);
	if(!Sch)
		return false;
	std::string IdCol;
	std::string ParentCol;
	if(!FindHierarchyColumns(*Sch, IdCol, ParentCol))
		return false;
	auto WIt = Db.Tables_.find(WorkTable);
	auto DIt = Db.Tables_.find(DeltaTable);
	if(WIt == Db.Tables_.end() || DIt == Db.Tables_.end())
		return false;
	if(DIt->second.RowStore.empty())
		return false;
	if(Index.Empty() && !Index.TryBuildFromTable(WIt->second.RowStore, IdCol, ParentCol))
		return false;
	bool Grew = false;
	std::unordered_set<std::string> Seen;
	for(const Database::Item &R : WIt->second.RowStore) {
		const auto It = R.find(IdCol);
		if(It != R.end())
			Seen.insert(It->second);
	}
	for(const Database::Item &R : DIt->second.RowStore) {
		const auto IdIt = R.find(IdCol);
		if(IdIt == R.end())
			continue;
		if(!Seen.insert(IdIt->second).second)
			continue;
		const auto ParIt = R.find(ParentCol);
		const std::string Parent = ParIt == R.end() ? "" : ParIt->second;
		MaterializedPathEntry Ent;
		if(Parent.empty() || Parent == "0") {
			Ent.Path = "/" + IdIt->second;
			Ent.Depth = 0;
		} else {
			const auto Pit = Index.Lookup(Parent);
			if(!Pit)
				return false;
			Ent.Path = Pit->Path + "/" + IdIt->second;
			Ent.Depth = Pit->Depth + 1;
		}
		Database::Item Enriched = R;
		Enriched["__path"] = Ent.Path;
		Enriched["__depth"] = std::to_string(Ent.Depth);
		WIt->second.RowStore.push_back(std::move(Enriched));
		Index.Insert(IdIt->second, Ent);
		Grew = true;
	}
	DIt->second.RowStore.clear();
	return Grew;
}

} // namespace AstralDB
