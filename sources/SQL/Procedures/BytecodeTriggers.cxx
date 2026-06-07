#include <SQL/Procedures/BytecodeTriggers.hxx>
#include <SQL/Bytecode/BytecodeInspect.hxx>
#include <SQL/Procedures/BytecodeProcedures.hxx>
#include <SQL/SQL.hxx>
#include <DS/JSON.hxx>
#include <Database/Database.hxx>
#include <IO/Error.hxx>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace AstralDB::SQL {

std::vector<const StoredTriggerEntry *> TriggersForTableEvent(const TriggerCatalog &Catalog, std::string_view Table,
                                                              TriggerTiming Timing, TriggerEvent Event) {
	std::vector<const StoredTriggerEntry *> Out;
	const auto It = Catalog.TableIndex.find(std::string(Table));
	if(It == Catalog.TableIndex.end())
		return Out;
	for(const std::string &Name : It->second) {
		for(const StoredTriggerEntry &E : Catalog.Triggers) {
			if(E.Name != Name || !E.Enabled)
				continue;
			if(E.Timing == Timing && E.Event == Event)
				Out.push_back(&E);
		}
	}
	return Out;
}

CompiledBytecode CompileTriggerBody(Logger *Logger, OptimizationLevel OptLevel, const Database *CatalogDb,
                                    std::string_view BodySql) {
	Parser P(BodySql);
	(void)P;
	return BuildCompiledBytecode(Logger, OptLevel, CatalogDb);
}

LoadedAbcFile LoadTriggerBytecode(const StoredTriggerEntry &Entry) {
	return LoadAbcFile(Entry.AbcPath);
}

StoredTriggerEntry CacheTriggerFromSql(TriggerCatalog &Catalog, const std::filesystem::path &SessionDbPath,
                                       StoredTriggerEntry Spec, Logger *Logger, OptimizationLevel OptLevel,
                                       const Database *CatalogDb, bool IfNotExists, bool OrReplace) {
	if(auto Existing = FindTrigger(Catalog, Spec.Name)) {
		if(IfNotExists)
			return *Existing;
		if(!OrReplace)
			throw std::runtime_error(Err::Prefixed("TRIG", "Trigger \"" + Spec.Name + "\" already exists."));
	}
	const std::filesystem::path CacheDir = DefaultTriggerCacheDir(SessionDbPath);
	std::error_code Ec;
	std::filesystem::create_directories(CacheDir, Ec);
	const auto Compiled = CompileTriggerBody(Logger, OptLevel, CatalogDb, Spec.BodySql);
	const std::filesystem::path AbcPath = CacheDir / (Spec.Name + ".abc");
	const std::filesystem::path SqlPath = CacheDir / (Spec.Name + ".sql");
	SaveAbcFile(AbcPath, Compiled);
	{
		std::ofstream SqlOut(SqlPath);
		if(!SqlOut)
			throw std::runtime_error(Err::Prefixed("TRIG", "Cannot write trigger SQL cache: " + SqlPath.string()));
		SqlOut << Spec.BodySql;
	}
	UnregisterTrigger(Catalog, Spec.Name);
	Spec.AbcPath = AbcPath;
	Spec.SqlPath = SqlPath;
	Spec.SourceHash = HashTriggerSource(Spec.BodySql);
	const auto Analysis = AnalyzeBytecode(Compiled.Instructions);
	Spec.BytecodeMeta.InstructionCount = Analysis.InstructionCount;
	Spec.BytecodeMeta.CalledProcedures = ScanProcedureCallsInSql(Spec.BodySql);
	Catalog.Triggers.push_back(std::move(Spec));
	RebuildTriggerRelations(Catalog);
	return Catalog.Triggers.back();
}

bool UnregisterTrigger(TriggerCatalog &Catalog, std::string_view Name) {
	const auto It = std::find_if(Catalog.Triggers.begin(), Catalog.Triggers.end(),
	                             [&](const StoredTriggerEntry &E) { return E.Name == Name; });
	if(It == Catalog.Triggers.end())
		return false;
	DropTriggerCacheFiles(*It);
	Catalog.Triggers.erase(It);
	RebuildTriggerRelations(Catalog);
	return true;
}

void DropTriggerCacheFiles(const StoredTriggerEntry &Entry) {
	std::error_code Ec;
	if(!Entry.AbcPath.empty())
		std::filesystem::remove(Entry.AbcPath, Ec);
	if(!Entry.SqlPath.empty())
		std::filesystem::remove(Entry.SqlPath, Ec);
}


} // namespace AstralDB::SQL
