#include <Database/TriggerRuntime.hxx>
#include <Database/Database.hxx>
#include <SQL/BytecodeInterpreter.hxx>
#include <SQL/BytecodeProcedures.hxx>
#include <chrono>
#include <filesystem>

namespace AstralDB {

namespace {

int64_t NowUnixMs() {
	using namespace std::chrono;
	return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void RunTriggerBytecode(Database &Db, Logger *Logger, const SQL::StoredTriggerEntry &Entry) {
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

void RunTriggerProcedure(Database &Db, Logger *Logger, const SQL::StoredTriggerEntry &Entry) {
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

} // namespace

void FireTriggersAssumeLocked(Database &Db, std::string_view Table, SQL::TriggerTiming Timing, SQL::TriggerEvent Event,
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

	SQL::TriggerCatalog Catalog = SQL::LoadTriggerCatalog(SQL::DefaultTriggerCatalogPath(Db.DbPath_));
	if(Catalog.CatalogPath.empty())
		Catalog.CatalogPath = SQL::DefaultTriggerCatalogPath(Db.DbPath_);

	std::vector<SQL::StoredTriggerEntry> Matches;
	for(const auto &[Name, Mem] : Db.TriggerDefinitionsAssumeLocked()) {
		(void)Name;
		if(!Mem.Enabled || Mem.TableName != Table || Mem.Timing != Timing || Mem.Event != Event)
			continue;
		SQL::StoredTriggerEntry Run = Mem;
		if(const auto Cached = SQL::FindTrigger(Catalog, Mem.Name)) {
			Run.AbcPath = Cached->AbcPath;
			Run.SqlPath = Cached->SqlPath;
			Run.BytecodeMeta = Cached->BytecodeMeta;
		}
		if(Run.AbcPath.empty()) {
			const auto Guess = SQL::DefaultTriggerCacheDir(Db.DbPath_) / (Mem.Name + ".abc");
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

	for(const SQL::StoredTriggerEntry &Ep : Matches) {
		SQL::TriggerFireRecord Rec;
		Rec.TriggerName = Ep.Name;
		Rec.TableName = Ep.TableName;
		Rec.Timing = SQL::TriggerTimingTag(Ep.Timing);
		Rec.Event = SQL::TriggerEventTag(Ep.Event);
		Rec.FiredAtUnixMs = static_cast<std::uint64_t>(NowUnixMs());
		Rec.Ok = true;
		try {
			if(Db.GetLogger())
				Db.GetLogger()->Info("Firing trigger \"" + Ep.Name + "\" on " + Ep.TableName + " (" +
				                     Rec.Timing + " " + Rec.Event + ")");
			if(Ep.ActionKind == "procedure")
				RunTriggerProcedure(Db, Db.GetLogger(), Ep);
			else
				RunTriggerBytecode(Db, Db.GetLogger(), Ep);
		} catch(const std::exception &Ex) {
			Rec.Ok = false;
			Rec.Detail = Ex.what();
			SQL::AppendTriggerFireRecord(Catalog, Rec);
			SQL::SaveTriggerCatalog(Catalog);
			throw;
		}
		SQL::AppendTriggerFireRecord(Catalog, Rec);
	}
	SQL::SaveTriggerCatalog(Catalog);
}

} // namespace AstralDB
