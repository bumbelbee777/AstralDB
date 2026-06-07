#include <Database/PtBridge/PtBridge.hxx>

#include <Database/Catalog/TriggerFire.hxx>
#include <Database/Database.hxx>
#include <IO/Error.hxx>
#include <SQL/Bytecode/BytecodeInterpreter.hxx>
#include <SQL/Procedures/BytecodeProcedures.hxx>
#include <SQL/Procedures/BytecodeTriggers.hxx>

#include <chrono>
#include <filesystem>

namespace AstralDB {
namespace {

constexpr SQL::OptimizationLevel kProcTrigOptLevel = SQL::OptimizationLevel::Advanced;

void RunTriggerBytecode(Database &Db, Logger *Logger, const StoredTriggerEntry &Entry) {
	if(!std::filesystem::exists(Entry.AbcPath))
		throw std::runtime_error("Trigger bytecode cache missing: " + Entry.AbcPath.string());
	const auto Loaded = SQL::LoadAbcFile(Entry.AbcPath);
	SQL::BytecodeInterpreter Interpreter(Logger);
	Interpreter.DatabasePath(Db.DbPath_);
	Interpreter.SetBorrowedPrimaryDatabase(&Db);
	Interpreter.EnsurePrimaryDatabaseOpened();
	const std::vector<std::string> *Pool = Loaded.StringPool.empty() ? nullptr : &Loaded.StringPool;
	Interpreter.RunNestedBytecode(Loaded.Instructions, Pool);
}

void RunTriggerProcedure(Database &Db, Logger *Logger, const StoredTriggerEntry &Entry) {
	if(Entry.ProcedureName.empty())
		throw std::runtime_error("Trigger \"" + Entry.Name + "\" has no procedure name.");
	SQL::ProcedureCatalog Catalog = SQL::LoadProcedureCatalog(SQL::DefaultProcedureCatalogPath(Db.DbPath_));
	if(Catalog.CatalogPath.empty())
		Catalog.CatalogPath = SQL::DefaultProcedureCatalogPath(Db.DbPath_);
	const auto P = SQL::FindProcedure(Catalog, Entry.ProcedureName);
	if(!P)
		throw std::runtime_error("Trigger \"" + Entry.Name + "\" references unknown procedure \"" +
		                         Entry.ProcedureName + "\".");
	const auto Loaded = SQL::LoadProcedureBytecode(*P, Catalog);
	SQL::BytecodeInterpreter Interpreter(Logger);
	Interpreter.DatabasePath(Db.DbPath_);
	Interpreter.SetBorrowedPrimaryDatabase(&Db);
	Interpreter.EnsurePrimaryDatabaseOpened();
	const std::vector<std::string> *Pool = Loaded.StringPool.empty() ? nullptr : &Loaded.StringPool;
	Interpreter.RunNestedBytecode(Loaded.Instructions, Pool);
}

int64_t NowUnixMs() {
	using namespace std::chrono;
	return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

} // namespace

namespace PtBridge {

void CacheProcedureFromDefinition(Database &Db, std::string_view ProcedureName, std::string BodySql, bool OrReplace,
                                  std::string SourceDialect, std::string_view ExceptionHandlersJson,
                                  std::string_view ControlFlowJson) {
	const auto ExceptionHandlers = SQL::DecodeExceptionHandlersJson(ExceptionHandlersJson);
	SQL::ProcedureCatalog Catalog;
	const auto CatPath = SQL::DefaultProcedureCatalogPath(Db.DbPath_);
	Catalog.CatalogPath = CatPath;
	Catalog = SQL::LoadProcedureCatalog(CatPath);
	SQL::UnregisterProcedure(Catalog, ProcedureName);
	SQL::CacheProcedureFromSql(Catalog, Db.DbPath_, std::string(ProcedureName), std::move(BodySql), Db.GetLogger(),
	                           kProcTrigOptLevel, &Db, true, OrReplace, std::move(SourceDialect), ExceptionHandlers,
	                           ControlFlowJson);
	SQL::SaveProcedureCatalog(Catalog);
}

void DropProcedureFromCatalog(const std::filesystem::path &SessionDbPath, std::string_view ProcedureName,
                              bool IfExists) {
	SQL::ProcedureCatalog Catalog;
	const auto CatPath = SQL::DefaultProcedureCatalogPath(SessionDbPath);
	Catalog = SQL::LoadProcedureCatalog(CatPath);
	if(Catalog.CatalogPath.empty())
		Catalog.CatalogPath = CatPath;
	if(!SQL::UnregisterProcedure(Catalog, ProcedureName) && !IfExists)
		throw std::runtime_error(Err::Prefixed("storage", "DROP PROCEDURE: \"" + std::string(ProcedureName) +
		                                                              "\" does not exist."));
	SQL::SaveProcedureCatalog(Catalog);
}

void ReplayWalCacheProcedure(Database &Db, std::string_view ProcedureName, std::string BodySql,
                             std::string ControlFlowBlob) {
	SQL::ProcedureCatalog Catalog;
	const auto CatPath = SQL::DefaultProcedureCatalogPath(Db.DbPath_);
	Catalog.CatalogPath = CatPath;
	Catalog = SQL::LoadProcedureCatalog(CatPath);
	SQL::CacheProcedureFromSql(Catalog, Db.DbPath_, std::string(ProcedureName), std::move(BodySql), Db.GetLogger(),
	                           kProcTrigOptLevel, &Db, true, false, std::string{}, {}, ControlFlowBlob);
	SQL::SaveProcedureCatalog(Catalog);
}

void ReplayWalDropProcedureFromCatalog(const std::filesystem::path &SessionDbPath, std::string_view ProcedureName) {
	SQL::ProcedureCatalog Catalog;
	const auto CatPath = SQL::DefaultProcedureCatalogPath(SessionDbPath);
	Catalog = SQL::LoadProcedureCatalog(CatPath);
	if(Catalog.CatalogPath.empty())
		Catalog.CatalogPath = CatPath;
	SQL::UnregisterProcedure(Catalog, ProcedureName);
	SQL::SaveProcedureCatalog(Catalog);
}

StoredTriggerEntry CacheTriggerFromDefinition(Database &Db, StoredTriggerEntry Spec, bool OrReplace) {
	TriggerCatalog Catalog;
	const auto CatPath = DefaultTriggerCatalogPath(Db.DbPath_);
	Catalog.CatalogPath = CatPath;
	Catalog = LoadTriggerCatalog(CatPath);
	SQL::UnregisterTrigger(Catalog, Spec.Name);
	const StoredTriggerEntry Cached =
	    SQL::CacheTriggerFromSql(Catalog, Db.DbPath_, std::move(Spec), Db.GetLogger(), kProcTrigOptLevel, &Db, true,
	                             OrReplace);
	SaveTriggerCatalog(Catalog);
	return Cached;
}

void DropTriggerFromCatalog(const std::filesystem::path &SessionDbPath, std::string_view TriggerName, bool IfExists) {
	TriggerCatalog Catalog;
	const auto CatPath = DefaultTriggerCatalogPath(SessionDbPath);
	Catalog = LoadTriggerCatalog(CatPath);
	if(Catalog.CatalogPath.empty())
		Catalog.CatalogPath = CatPath;
	if(!SQL::UnregisterTrigger(Catalog, TriggerName) && !IfExists)
		throw std::runtime_error(
		    Err::Prefixed("storage", "DROP TRIGGER: \"" + std::string(TriggerName) + "\" does not exist."));
	SaveTriggerCatalog(Catalog);
}

void ReplayWalCacheTrigger(Database &Db, StoredTriggerEntry Spec) {
	TriggerCatalog Catalog;
	const auto CatPath = DefaultTriggerCatalogPath(Db.DbPath_);
	Catalog.CatalogPath = CatPath;
	Catalog = LoadTriggerCatalog(CatPath);
	SQL::UnregisterTrigger(Catalog, Spec.Name);
	SQL::CacheTriggerFromSql(Catalog, Db.DbPath_, Spec, Db.GetLogger(), kProcTrigOptLevel, &Db, true, true);
	SaveTriggerCatalog(Catalog);
}

void ReplayWalDropTriggerFromCatalog(const std::filesystem::path &SessionDbPath, std::string_view TriggerName) {
	TriggerCatalog Catalog;
	const auto CatPath = DefaultTriggerCatalogPath(SessionDbPath);
	Catalog = LoadTriggerCatalog(CatPath);
	if(Catalog.CatalogPath.empty())
		Catalog.CatalogPath = CatPath;
	SQL::UnregisterTrigger(Catalog, TriggerName);
	SaveTriggerCatalog(Catalog);
}

} // namespace PtBridge

void FireTriggersAssumeLocked(Database &Db, std::string_view Table, TriggerTiming Timing, TriggerEvent Event,
                              const std::unordered_map<std::string, std::string> *OldRow,
                              const std::unordered_map<std::string, std::string> *NewRow, bool MutexAlreadyHeld) {
	if(Db.TriggerFireDepth() >= Db.TriggerFireDepthLimit())
		throw std::runtime_error("Trigger recursion depth limit exceeded.");
	struct DepthGuard {
		Database &Db_;
		DepthGuard(Database &D) : Db_(D) { Db_.BumpTriggerFireDepth(); }
		~DepthGuard() { Db_.PopTriggerFireDepth(); }
	} Depth(Db);

	if(MutexAlreadyHeld)
		DatabaseVmBumpDbMutexDepth();
	struct VmDepthPop {
		bool Active_;
		~VmDepthPop() {
			if(Active_)
				DatabaseVmPopDbMutexDepth();
		}
	} VmPop{MutexAlreadyHeld};

	TriggerCatalog Catalog = LoadTriggerCatalog(DefaultTriggerCatalogPath(Db.DbPath_));
	if(Catalog.CatalogPath.empty())
		Catalog.CatalogPath = DefaultTriggerCatalogPath(Db.DbPath_);

	std::vector<StoredTriggerEntry> Matches;
	for(const auto &[Name, Mem] : Db.TriggerDefinitionsAssumeLocked()) {
		(void)Name;
		if(!Mem.Enabled || Mem.TableName != Table || Mem.Timing != Timing || Mem.Event != Event)
			continue;
		StoredTriggerEntry Run = Mem;
		if(const auto Cached = FindTrigger(Catalog, Mem.Name)) {
			Run.AbcPath = Cached->AbcPath;
			Run.SqlPath = Cached->SqlPath;
			Run.BytecodeMeta = Cached->BytecodeMeta;
		}
		if(Run.AbcPath.empty()) {
			const auto Guess = DefaultTriggerCacheDir(Db.DbPath_) / (Mem.Name + ".abc");
			if(std::filesystem::exists(Guess))
				Run.AbcPath = Guess;
		}
		if(Run.AbcPath.empty())
			continue;
		Matches.push_back(std::move(Run));
	}
	if(Matches.empty())
		return;

	Db.InstallTriggerScratchRowAssumeLocked("__astral_trig_old", OldRow);
	Db.InstallTriggerScratchRowAssumeLocked("__astral_trig_new", NewRow);

	for(const StoredTriggerEntry &Ep : Matches) {
		TriggerFireRecord Rec;
		Rec.TriggerName = Ep.Name;
		Rec.TableName = Ep.TableName;
		Rec.Timing = TriggerTimingTag(Ep.Timing);
		Rec.Event = TriggerEventTag(Ep.Event);
		Rec.FiredAtUnixMs = static_cast<std::uint64_t>(NowUnixMs());
		Rec.Ok = true;
		try {
			if(Db.GetLogger())
				Db.GetLogger()->Info("Firing trigger \"" + Ep.Name + "\" on " + Ep.TableName + " (" + Rec.Timing +
				                     " " + Rec.Event + ")");
			if(Ep.ActionKind == "procedure")
				RunTriggerProcedure(Db, Db.GetLogger(), Ep);
			else
				RunTriggerBytecode(Db, Db.GetLogger(), Ep);
		} catch(const std::exception &Ex) {
			Rec.Ok = false;
			Rec.Detail = Ex.what();
			AppendTriggerFireRecord(Catalog, Rec);
			SaveTriggerCatalog(Catalog);
			throw;
		}
		AppendTriggerFireRecord(Catalog, Rec);
	}
	SaveTriggerCatalog(Catalog);
}

} // namespace AstralDB
