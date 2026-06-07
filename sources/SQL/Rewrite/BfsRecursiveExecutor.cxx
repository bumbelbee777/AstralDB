#include <SQL/Rewrite/BfsRecursiveExecutor.hxx>

#include <Database/Storage/HierarchyPath.hxx>

#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace AstralDB {
namespace SQL {

namespace {

std::string RowSignatureCanon(const std::unordered_map<std::string, std::string> &Row) {
	std::vector<std::string> Keys;
	Keys.reserve(Row.size());
	for(const auto &Kv : Row)
		Keys.push_back(Kv.first);
	std::sort(Keys.begin(), Keys.end());
	std::ostringstream O;
	for(const std::string &K : Keys) {
		O << K << '\x1E';
		const auto It = Row.find(K);
		O << (It == Row.end() ? "" : It->second) << '\x1F';
	}
	return O.str();
}

} // namespace

std::string BfsRecursiveExecutor::RowSignature(const std::unordered_map<std::string, std::string> &Row) {
	return RowSignatureCanon(Row);
}

void BfsRecursiveExecutor::DeduplicateIntoWork(RowTable &Work, RowTable &Delta) {
	std::unordered_set<std::string> Seen;
	Seen.reserve(Work.size() + Delta.size());
	for(const auto &R : Work)
		Seen.insert(RowSignature(R));
	for(auto &R : Delta) {
		if(Seen.insert(RowSignature(R)).second)
			Work.push_back(std::move(R));
	}
	Delta.clear();
}

BfsRecursiveExecutor::RowTable BfsRecursiveExecutor::Execute(const CteClause &Cte, Database &Db) {
	RowTable Work;
	if(!Cte.Anchor)
		return Work;
	const std::string &WorkName = Cte.PhysicalTable;
	const std::string DeltaName = WorkName + "_delta";
	Db.WithExclusiveBytecodeLock([&]() {
		auto WIt = Db.Tables_.find(WorkName);
		if(WIt != Db.Tables_.end())
			Work = WIt->second.RowStore;
	});
	if(!Cte.IsRecursive())
		return Work;
	HierarchyPathIndex Index;
	for(int Level = 0; Level < 1024; ++Level) {
		(void)Level;
		bool Grew = false;
		Db.WithExclusiveBytecodeLock([&]() {
			if(TryRecursiveCteHierarchyMerge(Db, WorkName, DeltaName, Index)) {
				Grew = true;
				return;
			}
			auto WIt = Db.Tables_.find(WorkName);
			auto DIt = Db.Tables_.find(DeltaName);
			if(WIt == Db.Tables_.end() || DIt == Db.Tables_.end())
				return;
			RowTable Delta = std::move(DIt->second.RowStore);
			DIt->second.RowStore.clear();
			const std::size_t Before = WIt->second.RowStore.size();
			DeduplicateIntoWork(WIt->second.RowStore, Delta);
			if(WIt->second.RowStore.size() > Before)
				Grew = true;
		});
		if(!Grew)
			break;
	}
	Db.WithExclusiveBytecodeLock([&]() {
		auto WIt = Db.Tables_.find(WorkName);
		if(WIt != Db.Tables_.end())
			Work = WIt->second.RowStore;
	});
	return Work;
}

} // namespace SQL
} // namespace AstralDB
