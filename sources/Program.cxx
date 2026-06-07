#include <SQL/SQL.hxx>
#include <SQL/Bytecode/BytecodeInterpreter.hxx>
#include <SQL/Profiler/SqlPipeline.hxx>
#include <SQL/Profiler/QueryProfiler.hxx>
#include <SQL/Bytecode/FastPathGuard.hxx>
#include <SQL/Bytecode/Bytecode.hxx>
#include <SQL/Bytecode/BytecodeFormat.hxx>
#include <SQL/Bytecode/BytecodeInspect.hxx>
#include <SQL/Bytecode/BytecodeDebug.hxx>
#include <SQL/Bytecode/QueryCheckpoint.hxx>
#include <SQL/Shape/QueryShapeRouter.hxx>
#include <SQL/Procedures/BytecodeProcedures.hxx>
#include <SQL/Procedures/BytecodeTriggers.hxx>
#include <IO/Logger.hxx>
#include <IO/Error.hxx>
#include <IO/ErrMsg.hxx>
#include <IO/Job.hxx>
#include <Database/Database.hxx>
#include <Database/Storage/StarJoinCubeAnalytic.hxx>
#include <astraldb/AstralDB.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
#include <csignal>
#else
#define NOMINMAX
#include <windows.h>
#endif

static bool gCliCheckpointOnSignal = false;
static std::optional<std::filesystem::path> gCliResumeCheckpointPath;

#if !defined(_WIN32)
static void QueryCheckpointSignalHandler(int) {
	AstralDB::SQL::gRequestQueryCheckpoint.store(true, std::memory_order_release);
}
#else
static BOOL WINAPI QueryCheckpointConsoleHandler(DWORD CtrlType) {
	if(CtrlType == CTRL_C_EVENT || CtrlType == CTRL_BREAK_EVENT) {
		AstralDB::SQL::gRequestQueryCheckpoint.store(true, std::memory_order_release);
		return TRUE;
	}
	return FALSE;
}
#endif

static void InstallQueryCheckpointSignalHandlers() {
	if(!gCliCheckpointOnSignal)
		return;
#if !defined(_WIN32)
	std::signal(SIGINT, QueryCheckpointSignalHandler);
	std::signal(SIGTERM, QueryCheckpointSignalHandler);
#else
	SetConsoleCtrlHandler(QueryCheckpointConsoleHandler, TRUE);
#endif
}

// Helper function to read file contents
std::string ReadFile(const std::string& Path) {
	std::ifstream File(Path, std::ios::binary);
	if (!File) {
		throw std::runtime_error(
		    AstralDB::Err::Prefixed("CLI", std::string(AstralDB::ErrMsg::CliNoFile) + Path));
	}
	std::string Contents((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
	if (Contents.empty()) {
		throw std::runtime_error(
		    AstralDB::Err::Prefixed("CLI", std::string(AstralDB::ErrMsg::CliEmptyFile) + Path));
	}
	if (Contents.size() >= 3 && static_cast<unsigned char>(Contents[0]) == 0xEF && static_cast<unsigned char>(Contents[1]) == 0xBB &&
	    static_cast<unsigned char>(Contents[2]) == 0xBF)
		Contents.erase(0, 3);
	return Contents;
}

static void PrintTimeSqlLine(AstralDB::Logger *Log, const std::string &Line) {
	std::cerr << Line << std::flush;
	std::cout << Line << std::flush;
	if(Log)
		Log->Info(Line);
	if(const char *Path = std::getenv("ASTRALDB_TIME_SQL_OUT")) {
		if(FILE *F = std::fopen(Path, "a")) {
			std::fwrite(Line.data(), 1, Line.size(), F);
			std::fclose(F);
		}
	}
}

static void FinalizeTimeSqlProfile(AstralDB::SQL::BytecodeInterpreter &Interpreter, const double CompileMs,
                                   const double ExecMs) {
	auto &Cfg = Interpreter.SessionConfig();
	if(Cfg.ProfileName)
		AstralDB::SQL::QueryProfiler::Instance().BeginQuery(*Cfg.ProfileName);
	AstralDB::SQL::QueryProfile Profile;
	Profile.Name = Cfg.ProfileName.value_or("time-sql");
	Profile.Region = Cfg.RegionName.value_or("");
	Profile.CompileTime = std::chrono::duration_cast<std::chrono::nanoseconds>(
	    std::chrono::duration<double, std::milli>(CompileMs));
	Profile.ExecuteTime =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double, std::milli>(ExecMs));
	const auto Stats = Interpreter.LastTimeSqlStats();
	Profile.ScannedRows = Stats.RowsScanned;
	Profile.ResultRows = Stats.ResultRows;
	Profile.VmUsed = AstralDB::SQL::VmChoiceLabel(Cfg.Vm);
	Profile.Verified = !Stats.IntegrityFailed;
	AstralDB::SQL::QueryProfiler::Instance().EndQuery(Profile);
	if(Cfg.ProfileDumpPath) {
		AstralDB::SQL::QueryProfiler::Instance().DumpToJSON(*Cfg.ProfileDumpPath);
		const auto &Regions = AstralDB::SQL::QueryProfiler::Instance().RegionTimings();
		if(!Regions.empty()) {
			std::ostringstream O;
			O << "[time-sql-profile] dump=" << *Cfg.ProfileDumpPath;
			for(const auto &[Name, Ns] : Regions)
				O << " " << Name << "_ms=" << std::fixed << std::setprecision(3)
				  << (static_cast<double>(Ns.count()) / 1'000'000.0);
			O << "\n";
			PrintTimeSqlLine(nullptr, O.str());
		}
	}
}

struct CliBytecodeOptions {
	bool TraceExecution = false;
	std::size_t DebugMaxSteps = 0;
	std::vector<std::size_t> Breakpoints;
	std::optional<std::filesystem::path> ProcCatalog;
	std::optional<std::string> BytecodeAspect;
	bool CompileWithStringPool = false;
};

static int CliTimeSqlWarmup = 0;
static int CliTimeSqlRuns = 1;
static bool CliTimeSqlDurable = false;
static std::optional<std::string> CliTimeSqlSetupPath;

static void KickWalIoOverlap(AstralDB::Database *Db) {
	if(Db)
		Db->FlushWalToDisk();
}

static double QuiesceWalIoMs(AstralDB::Database *Db) {
	if(!Db)
		return 0.0;
	using Clock = std::chrono::steady_clock;
	using Ms = std::chrono::duration<double, std::milli>;
	const auto T0 = Clock::now();
	Db->QuiesceAsyncFsync();
	return Ms(Clock::now() - T0).count();
}

static void AppendWalTimingFields(std::ostringstream &O, double WalQuiesceMs, double SetupCompileMs,
                                  double QueryCompileMs) {
	if(SetupCompileMs > 0.0 || QueryCompileMs > 0.0) {
		if(SetupCompileMs > 0.0)
			O << " setup_compile_ms=" << SetupCompileMs;
		if(QueryCompileMs > 0.0)
			O << " query_compile_ms=" << QueryCompileMs;
	}
	if(CliTimeSqlDurable || WalQuiesceMs > 0.0)
		O << " wal_quiesce_ms=" << WalQuiesceMs;
	if(CliTimeSqlDurable)
		O << " durable=1";
}

static void CommitDurableDatabase(AstralDB::SQL::BytecodeInterpreter &Interpreter) {
	if(!CliTimeSqlDurable)
		return;
	AstralDB::Database *Db = Interpreter.MutatingDatabase();
	if(!Db)
		return;
	Db->SetSkipExitSyncOnDestroy(false);
	Db->FlushWalToDisk();
	Db->QuiesceAsyncFsync();
	Db->SyncToFile();
}

static bool CliArgIs(const std::string &Arg, std::initializer_list<const char *> Names) {
	for(const char *N : Names)
		if(Arg == N)
			return true;
	return false;
}

static void PrintBytecodeHelpLines() {
	std::cout << "  Bytecode (.abc) tools (long flags; shorthands in parentheses):\n";
	std::cout << "  --inspect-bytecode FILE (-ib)     Summary analysis\n";
	std::cout << "  --bytecode-aspect FILE KIND (-ba) Query one aspect\n";
	std::cout << "  --disasm-bytecode FILE (-db)      Pretty disassembly\n";
	std::cout << "  --validate-bytecode FILE (-vb)    Structural validation\n";
	std::cout << "  --debug-bytecode FILE (-dbg)      Execute with VM trace\n";
	std::cout << "  --trace-bytecode (-tb)            Trace when running -fb / --proc-call\n";
	std::cout << "  --debug-steps N (-ds)             Cap traced steps (0 = unlimited)\n";
	std::cout << "  --breakpoint IP (-bp)             Instruction-index breakpoint\n";
	std::cout << "  --compile-pool (-cp)              With -cc: string pool trailer\n";
	std::cout << "  Procedures (catalog + astraldb_procs_cache/*.abc):\n";
	std::cout << "  --proc-catalog PATH (-pc)         Registry JSON path\n";
	std::cout << "  --proc-register NAME FILE (-pr)   Register .abc module\n";
	std::cout << "  --proc-unregister NAME (-pu)      Remove registration\n";
	std::cout << "  --proc-list (-pl)                 List procedures\n";
	std::cout << "  --proc-info NAME (-pi)            Metadata + bytecode summary\n";
	std::cout << "  --proc-call NAME (-px)            Execute registered .abc\n";
	std::cout << "  --proc-relations (-pg)            Procedure <-> .abc relation graph\n";
	std::cout << "  SQL: CREATE PROCEDURE n AS (…); PL/pgSQL, PL/SQL, T-SQL bodies; CALL n; DROP PROCEDURE n;\n";
	std::cout << "  Triggers (catalog + astraldb_triggers_cache/*.abc):\n";
	std::cout << "  --trig-list (-tl)                 List triggers\n";
	std::cout << "  --trig-info NAME (-ti)            Trigger metadata\n";
	std::cout << "  --trig-relations (-tg)            Table → trigger index\n";
	std::cout << "  --trig-fires (-tf)                Recent trigger fire log\n";
	std::cout << "  SQL: CREATE TRIGGER …; ALTER TRIGGER … ENABLE|DISABLE; DROP TRIGGER …;\n";
}


static std::filesystem::path ResolveTrigCatalog(const std::filesystem::path &SessionDb) {
	return AstralDB::DefaultTriggerCatalogPath(SessionDb);
}

static std::filesystem::path ResolveProcCatalog(const std::filesystem::path &SessionDb,
    const std::optional<std::filesystem::path> &Override) {
	if(Override.has_value())
		return *Override;
	return AstralDB::SQL::DefaultProcedureCatalogPath(SessionDb);
}

static int RunBytecodeInspect(const std::filesystem::path &AbcPath) {
	const auto Loaded = AstralDB::SQL::LoadAbcFile(AbcPath);
	std::cout << AstralDB::SQL::FormatBytecodeAnalysis(AstralDB::SQL::AnalyzeBytecode(Loaded.Instructions));
	if(!Loaded.StringPool.empty())
		std::cout << "string_pool_entries=" << Loaded.StringPool.size() << "\n";
	return 0;
}

static int RunBytecodeAspect(const std::filesystem::path &AbcPath, std::string_view Aspect) {
	const auto Loaded = AstralDB::SQL::LoadAbcFile(AbcPath);
	std::cout << AstralDB::SQL::QueryBytecodeAspect(Loaded.Instructions, Aspect);
	return 0;
}

static int RunBytecodeDisasm(const std::filesystem::path &AbcPath) {
	const auto Loaded = AstralDB::SQL::LoadAbcFile(AbcPath);
	std::cout << AstralDB::SQL::DisassemblePretty(Loaded.Instructions);
	return 0;
}

static int RunBytecodeValidate(const std::filesystem::path &AbcPath) {
	const auto Loaded = AstralDB::SQL::LoadAbcFile(AbcPath);
	const auto Report = AstralDB::SQL::ValidateBytecode(Loaded.Instructions);
	for(const auto &W : Report.Warnings)
		std::cerr << "warning: " << W << "\n";
	for(const auto &E : Report.Errors)
		std::cerr << "error: " << E << "\n";
	return Report.Ok ? 0 : 2;
}

static void ExecuteBytecodeWithOptions(AstralDB::SQL::BytecodeInterpreter &Interpreter,
    const AstralDB::SQL::Bytecode &Code, const std::vector<std::string> *Pool,
    const CliBytecodeOptions &BcOpts, AstralDB::Logger *Logger) {
	std::unique_ptr<AstralDB::SQL::VmDebugSession> Session;
	if(BcOpts.TraceExecution || BcOpts.DebugMaxSteps > 0 || !BcOpts.Breakpoints.empty()) {
		AstralDB::SQL::VmDebugConfig Cfg;
		Cfg.TraceToStderr = true;
		Cfg.MaxTraceSteps = BcOpts.DebugMaxSteps;
		Cfg.BreakpointIps = BcOpts.Breakpoints;
		Session = std::make_unique<AstralDB::SQL::VmDebugSession>(Cfg);
		Session->AttachListener(AstralDB::SQL::MakeVmDebugListener(std::cerr, Cfg));
		Interpreter.SetDebugSession(Session.get());
	}
	Interpreter.Execute(Code, Pool);
	if(Session) {
		const auto &Rep = Session->Report();
		if(Logger && (BcOpts.TraceExecution || Rep.HaltedEarly))
			Logger->Info("VM debug: steps=" + std::to_string(Rep.StepsExecuted) +
			             " halted_early=" + (Rep.HaltedEarly ? "yes" : "no"));
		if(Rep.HaltedEarly && !Rep.HaltReason.empty())
			std::cerr << "[debug] halted: " << Rep.HaltReason << " at ip=" << Rep.HaltedAtIp << "\n";
		Interpreter.SetDebugSession(nullptr);
	}
}

static void ApplyCliGlobalFlags(int Argc, char** Argv, bool& Verbose, std::string& LogFile,
    AstralDB::SQL::OptimizationLevel& OptLevel, bool& MemoryOnly, std::string& CompileOut,
    std::filesystem::path *DatabasePathOpt, bool *DatabasePathProvided,
    std::optional<std::string> *CliUserOut, std::optional<std::string> *CliPasswordOut,
    std::optional<std::filesystem::path> *AuditFileOut, CliBytecodeOptions *BcOpts) {
	for(int I = 1; I < Argc; ++I) {
		const std::string Arg(Argv[I]);
		if(Arg == "-V" || Arg == "--verbose")
			Verbose = true;
		else if(Arg == "-l" || Arg == "--log-file") {
			if(I + 1 < Argc)
				LogFile = Argv[++I];
			else
				throw std::runtime_error("No file provided after -l/--log-file");
		} else if(Arg == "-m" || Arg == "--mmap")
			MemoryOnly = true;
		else if(Arg == "--database") {
			if(I + 1 < Argc && DatabasePathOpt && DatabasePathProvided) {
				*DatabasePathOpt = Argv[++I];
				*DatabasePathProvided = true;
			} else
				throw std::runtime_error("No path provided after --database");
		} else if((Arg == "-U" || Arg == "--user") && CliUserOut) {
			if(I + 1 < Argc)
				*CliUserOut = std::string(Argv[++I]);
			else
				throw std::runtime_error("No username after -U/--user");
		} else if((Arg == "-P" || Arg == "--password") && CliPasswordOut) {
			if(I + 1 < Argc)
				*CliPasswordOut = std::string(Argv[++I]);
			else
				throw std::runtime_error("No password after -P/--password");
		} else if(Arg == "-o" || Arg == "--output") {
			if(I + 1 < Argc)
				CompileOut = Argv[++I];
			else
				throw std::runtime_error("No file provided after -o/--output");
		} else if(Arg == "-O0")
			OptLevel = AstralDB::SQL::OptimizationLevel::None;
		else if(Arg == "-O1")
			OptLevel = AstralDB::SQL::OptimizationLevel::Basic;
		else if(Arg == "-O2")
			OptLevel = AstralDB::SQL::OptimizationLevel::Advanced;
		else if(Arg == "-O3")
			OptLevel = AstralDB::SQL::OptimizationLevel::Aggressive;
		else if(Arg == "-O4")
			OptLevel = AstralDB::SQL::OptimizationLevel::Maximum;
		else if(Arg == "--audit-file" && AuditFileOut) {
			if(I + 1 < Argc)
				*AuditFileOut = std::filesystem::path(Argv[++I]);
			else
				throw std::runtime_error("No path provided after --audit-file");
		} else if(Arg == "--time-sql-warmup") {
			if(I + 1 < Argc)
				CliTimeSqlWarmup = std::max(0, std::atoi(Argv[++I]));
			else
				throw std::runtime_error("No count provided after --time-sql-warmup");
		} else if(Arg == "--time-sql-runs") {
			if(I + 1 < Argc)
				CliTimeSqlRuns = std::max(1, std::atoi(Argv[++I]));
			else
				throw std::runtime_error("No count provided after --time-sql-runs");
		} else if(Arg == "--time-sql-setup") {
			if(I + 1 < Argc)
				CliTimeSqlSetupPath = Argv[++I];
			else
				throw std::runtime_error("No file provided after --time-sql-setup");
		} else if(Arg == "--time-sql-durable") {
			CliTimeSqlDurable = true;
		} else if(BcOpts && CliArgIs(Arg, {"--trace-bytecode", "-tb"}))
			BcOpts->TraceExecution = true;
		else if(BcOpts && CliArgIs(Arg, {"--compile-pool", "-cp"}))
			BcOpts->CompileWithStringPool = true;
		else if(BcOpts && CliArgIs(Arg, {"--debug-steps", "-ds"})) {
			if(I + 1 < Argc)
				BcOpts->DebugMaxSteps = static_cast<std::size_t>(std::stoull(Argv[++I]));
			else
				throw std::runtime_error("No count after --debug-steps / -ds");
		} else if(BcOpts && CliArgIs(Arg, {"--breakpoint", "-bp"})) {
			if(I + 1 < Argc)
				BcOpts->Breakpoints.push_back(static_cast<std::size_t>(std::stoull(Argv[++I])));
			else
				throw std::runtime_error("No instruction index after --breakpoint / -bp");
		} else if(Arg == "--proc-catalog" || Arg == "-pc") {
			if(I + 1 < Argc)
				BcOpts->ProcCatalog = std::filesystem::path(Argv[++I]);
			else
				throw std::runtime_error("No path after --proc-catalog / -pc");
		} else if(Arg == "--checkpoint-on-signal") {
			gCliCheckpointOnSignal = true;
		} else if(Arg == "--resume-checkpoint") {
			if(I + 1 < Argc && Argv[I + 1][0] != '-')
				gCliResumeCheckpointPath = std::filesystem::path(Argv[++I]);
			else
				gCliResumeCheckpointPath = std::filesystem::path{};
		}
	}
}

static void MaybeResumeQueryCheckpoint(AstralDB::SQL::BytecodeInterpreter &Interpreter,
                                       const AstralDB::SQL::Bytecode &Code) {
	if(!gCliResumeCheckpointPath.has_value())
		return;
	const std::filesystem::path Path =
	    gCliResumeCheckpointPath->empty()
	        ? AstralDB::SQL::DefaultQueryCheckpointPath(Interpreter.DatabasePath())
	        : *gCliResumeCheckpointPath;
	if(const auto Loaded = AstralDB::SQL::LoadQueryCheckpoint(Path))
		(void)AstralDB::SQL::ResumeQueryCheckpoint(Interpreter, Code, *Loaded);
}

static void ApplyCliAuditLog(AstralDB::Database *Db, const std::optional<std::filesystem::path> &AuditPath) {
	if(!Db || !AuditPath.has_value())
		return;
	Db->SetAuditLogPath(AuditPath);
}

static std::filesystem::path MakeSessionDatabasePath(bool MemoryOnly) {
	if(!MemoryOnly)
		return "astral.db";
	namespace fs = std::filesystem;
	std::random_device Rd;
	const unsigned long long Salt =
	    (static_cast<unsigned long long>(Rd()) << 32) ^ static_cast<unsigned long long>(Rd());
	const auto Ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
	        .count();
	return fs::temp_directory_path() / ("astraldb_session_" + std::to_string(Salt ^ static_cast<unsigned long long>(Ns))
	                                    + ".db");
}

struct EphemeralSessionDatabase {
	std::filesystem::path Path;
	bool Remove = false;
	~EphemeralSessionDatabase() {
		if(!Remove)
			return;
		std::error_code Ec;
		std::filesystem::remove(Path, Ec);
	}
};

static std::optional<std::string> EffectiveCliUser(const std::optional<std::string> &FromFlags) {
	if(FromFlags.has_value() && !FromFlags->empty())
		return FromFlags;
	if(const char *Ev = std::getenv("ASTRALDB_USER")) {
		const std::string S(Ev);
		if(!S.empty())
			return S;
	}
	return std::nullopt;
}

static void ApplyCliSessionAuth(AstralDB::Database *Db, const std::optional<std::string> &CliUserFlags,
    const std::optional<std::string> &CliPasswordFlags) {
	if(!Db)
		return;
	const auto EffUser = EffectiveCliUser(CliUserFlags);
	if(!EffUser.has_value())
		return;
	std::string Pw;
	if(CliPasswordFlags.has_value() && !CliPasswordFlags->empty())
		Pw = *CliPasswordFlags;
	else if(const char *Ev = std::getenv("ASTRALDB_PASSWORD"))
		Pw = Ev;
	if(Pw.empty()) {
		throw std::runtime_error(AstralDB::Err::Prefixed(
		    "CLI",
		    "Password required when authenticating: use -P/--password after -U/--user, or set ASTRALDB_PASSWORD."));
	}
	if(!Db->AuthenticateUser(*EffUser, Pw)) {
		throw std::runtime_error(
		    AstralDB::Err::Prefixed("CLI", "Authentication failed for user \"" + *EffUser + "\"."));
	}
}

// REPL mode implementation
void RunREPL(AstralDB::Logger& Logger, const std::filesystem::path& SessionDbPath,
    AstralDB::SQL::OptimizationLevel OptLevel, const std::optional<std::string> &CliUserFlags,
    const std::optional<std::string> &CliPasswordFlags,
    const std::optional<std::filesystem::path> &AuditPath) {
	std::cout << "AstralDB REPL v" << ASTRALDB_VERSION << "\n";
	std::cout << "Type 'exit' or 'quit' to exit\n";
	std::cout << "Type 'help' for available commands\n\n";

	std::string Input;
	std::vector<std::string> History;
	const size_t MaxHistorySize = 1000;
	AstralDB::SQL::BytecodeInterpreter Interpreter(&Logger);
	Interpreter.DatabasePath(SessionDbPath);
	Interpreter.EnsurePrimaryDatabaseOpened();
	ApplyCliSessionAuth(Interpreter.PrimaryDatabase(), CliUserFlags, CliPasswordFlags);
	ApplyCliAuditLog(Interpreter.PrimaryDatabase(), AuditPath);

	while (true) {
		std::cout << "astraldb> ";
		std::getline(std::cin, Input);

		// Handle special commands
		if (Input == "exit" || Input == "quit") {
			break;
		} else if (Input == "help") {
			std::cout << "Available commands:\n";
			std::cout << "  exit, quit - Exit the REPL\n";
			std::cout << "  help - Show this help message\n";
			std::cout << "  history - Show command history\n";
			std::cout << "  clear - Clear the screen\n";
			continue;
		} else if (Input == "history") {
			for (size_t i = 0; i < History.size(); ++i) {
				std::cout << i + 1 << ": " << History[i] << "\n";
			}
			continue;
		} else if (Input == "clear") {
			#ifdef _WIN32
			(void)system("cls");
			#else
			(void)system("clear");
			#endif
			continue;
		}

		// Add to history
		History.push_back(Input);
		if (History.size() > MaxHistorySize) {
			History.erase(History.begin());
		}

		try {
			// Parse and execute the query
			AstralDB::SQL::Parser Parser(Input);
			AstralDB::SQL::Bytecode Code =
			    AstralDB::SQL::BuildBytecode(&Logger, OptLevel, Interpreter.PrimaryDatabase());
			Interpreter.Execute(Code);

			if (Logger.IsVerbose()) {
				std::cout << "Executed bytecode:\n" << AstralDB::SQL::DisassemblePretty(Code) << "\n";
			}
		} catch (const std::exception& e) {
			AstralDB::Err::PrintCliError(std::cerr, e.what());
			Logger.Error("REPL error: " + std::string(e.what()));
		}
	}
}

int main(int Argc, char** Argv) {
	struct JobSystemGuard {
		JobSystemGuard() { AstralDB::JobSystem::Instance().Initialize(); }
		~JobSystemGuard() { AstralDB::JobSystem::Instance().Shutdown(); }
	} JobGuard;
	bool Verbose = false;
	std::string LogFile = "astraldb.log";
	std::string CompileOut = "out.abc";
	bool MemoryOnly = false;
	AstralDB::SQL::OptimizationLevel OptLevel = AstralDB::SQL::OptimizationLevel::Basic;
	std::unique_ptr<AstralDB::Logger> Logger;

	try {
		std::filesystem::path CliDbPath;
		bool DbFromCli = false;
		std::optional<std::string> CliUser;
		std::optional<std::string> CliPassword;
		std::optional<std::filesystem::path> CliAuditFile;
		CliBytecodeOptions BcOpts;
		ApplyCliGlobalFlags(Argc, Argv, Verbose, LogFile, OptLevel, MemoryOnly, CompileOut, &CliDbPath,
		                    &DbFromCli, &CliUser, &CliPassword, &CliAuditFile, &BcOpts);

		Logger = std::make_unique<AstralDB::Logger>(LogFile, Verbose);
		AstralDB::SQL::SetParserDiagnostics(Verbose);
		InstallQueryCheckpointSignalHandlers();

		std::filesystem::path SessionDbPath =
		    DbFromCli ? CliDbPath : MakeSessionDatabasePath(MemoryOnly);
		EphemeralSessionDatabase EphemeralDb{};
		if(MemoryOnly) {
			EphemeralDb.Path = SessionDbPath;
			EphemeralDb.Remove = true;
			Logger->Info("Session database (ephemeral path): " + SessionDbPath.string());
		}

		for(int I = 1; I < Argc; ++I) {
			std::string Arg(Argv[I]);
			if(Arg == "-h" || Arg == "--help") {
				std::cout << "Usage: astraldb [options]\n";
				std::cout << "Options:\n";
				std::cout << "  -h, --help              Display help\n";
				std::cout << "  -v, --version           Show version\n";
				std::cout << "  -q, --query QUERY       Execute provided query\n";
				std::cout << "  -r, --repl              Run in REPL mode\n";
				std::cout << "  -c, --check FILE        Check input query file only\n";
				std::cout << "  -V, --verbose           Enable verbose output\n";
				std::cout << "  -fb, --from-bytecode FILE  Run input bytecode\n";
				std::cout << "  -cc, --compile FILE     Compile query to bytecode\n";
				std::cout << "  -o, --output OUT        Bytecode output path (with -cc; default out.abc)\n";
				std::cout << "  -l, --log-file FILE     Save debug logs to file\n";
				std::cout << "      --audit-file FILE   Append security audit events to FILE\n";
				std::cout << "  -s FILE                 Evaluate, compile, and run query file\n";
				std::cout << "  --time-sql FILE        Run query file; print parse / execute wall times (stderr)\n";
				std::cout << "      --time-sql-warmup N  With --time-sql-runs: untimed execute passes\n";
				std::cout << "      --time-sql-runs N    Compile once; time N executes (median reported)\n";
				std::cout << "      --time-sql-setup FILE  Run setup once (untimed); --time-sql FILE is timed query only\n";
				std::cout << "      --time-sql-durable     Quiesce async WAL fsync after timed query; report wal_quiesce_ms\n";
				std::cout << "      --time-sql-suite FILE  After --time-sql-setup: timed queries (lines: LABEL\\tPATH)\n";
				std::cout << "      --checkpoint-on-signal   Save encrypted .query.ckpt on SIGINT (POSIX) / Ctrl+C\n";
				std::cout << "      --resume-checkpoint [PATH]  Resume from query checkpoint (default: DB.query.ckpt)\n";
				std::cout << "  -m, --mmap              Session DB under OS temp (no astral.db in cwd)\n";
				std::cout << "      --database PATH     Use PATH as persistence file (instead of cwd astral.db / temp)\n";
				std::cout << "  -U, --user NAME         Authenticate as NAME for this process (see -P / env)\n";
				std::cout << "  -P, --password SECRET   Password for -U (omit and use ASTRALDB_PASSWORD instead)\n";
				std::cout << "      --export-bundle PATH [--export-format csv|json|tsv]\n";
				std::cout << "      --import-bundle PATH [--import-format csv|json|tsv]\n";
				std::cout << "      --convert SRC DST --from csv|json|tsv --to csv|json|tsv\n";
				std::cout << "  -O0                     Disable optimizations\n";
				std::cout << "  -O1                     Basic optimizations (default)\n";
				std::cout << "  -O2                     Advanced optimizations\n";
				std::cout << "  -O3                     Aggressive optimizations\n";
				std::cout << "  -O4                     Maximum optimizations\n";
				PrintBytecodeHelpLines();
				return 0;
			}
			if(Arg == "-v" || Arg == "--version") {
				std::cout << "AstralDB " << ASTRALDB_VERSION << "\n";
				return 0;
			}
			if(Arg == "-V" || Arg == "--verbose" || Arg == "-m" || Arg == "--mmap")
				continue;
			if(Arg == "-l" || Arg == "--log-file") {
				if(I + 1 < Argc)
					++I;
				continue;
			}
			if(Arg == "-o" || Arg == "--output") {
				if(I + 1 < Argc)
					++I;
				continue;
			}
			if(Arg == "--database") {
				if(I + 1 < Argc)
					++I;
				continue;
			}
			if(Arg == "-U" || Arg == "--user") {
				if(I + 1 < Argc)
					++I;
				continue;
			}
			if(Arg == "-P" || Arg == "--password") {
				if(I + 1 < Argc)
					++I;
				continue;
			}
			if(Arg == "--audit-file") {
				if(I + 1 < Argc)
					++I;
				continue;
			}
			if(Arg == "--time-sql-warmup" || Arg == "--time-sql-runs" || Arg == "--time-sql-setup") {
				if(I + 1 < Argc)
					++I;
				continue;
			}
			if(Arg == "--time-sql-durable")
				continue;
			if(Arg == "-O0" || Arg == "-O1" || Arg == "-O2" || Arg == "-O3" || Arg == "-O4")
				continue;
			if(CliArgIs(Arg, {"--trace-bytecode", "-tb", "--compile-pool", "-cp"}))
				continue;
			if(CliArgIs(Arg, {"--debug-steps", "-ds", "--breakpoint", "-bp", "--proc-catalog", "-pc"})) {
				if(I + 1 < Argc)
					++I;
				continue;
			}
			if(CliArgIs(Arg, {"--inspect-bytecode", "-ib", "--inspect-bc"})) {
				if(I + 1 < Argc)
					return RunBytecodeInspect(Argv[++I]);
				throw std::runtime_error("No file after --inspect-bytecode / -ib");
			}
			if(CliArgIs(Arg, {"--bytecode-aspect", "-ba"})) {
				if(I + 2 < Argc) {
					const std::filesystem::path Abc = Argv[++I];
					const std::string Aspect = Argv[++I];
					return RunBytecodeAspect(Abc, Aspect);
				}
				throw std::runtime_error("--bytecode-aspect / -ba needs FILE and ASPECT");
			}
			if(CliArgIs(Arg, {"--disasm-bytecode", "-db", "--dasm-bc"})) {
				if(I + 1 < Argc)
					return RunBytecodeDisasm(Argv[++I]);
				throw std::runtime_error("No file after --disasm-bytecode / -db");
			}
			if(CliArgIs(Arg, {"--validate-bytecode", "-vb"})) {
				if(I + 1 < Argc)
					return RunBytecodeValidate(Argv[++I]);
				throw std::runtime_error("No file after --validate-bytecode / -vb");
			}
			if(CliArgIs(Arg, {"--debug-bytecode", "-dbg", "-dbc"})) {
				if(I + 1 < Argc) {
					const auto Loaded = AstralDB::SQL::LoadAbcFile(Argv[++I]);
					BcOpts.TraceExecution = true;
					AstralDB::SQL::BytecodeInterpreter Interpreter(Logger.get());
					Interpreter.DatabasePath(SessionDbPath);
					Interpreter.EnsurePrimaryDatabaseOpened();
					ApplyCliSessionAuth(Interpreter.PrimaryDatabase(), CliUser, CliPassword);
					ApplyCliAuditLog(Interpreter.PrimaryDatabase(), CliAuditFile);
					const std::vector<std::string> *Pool =
					    Loaded.StringPool.empty() ? nullptr : &Loaded.StringPool;
					ExecuteBytecodeWithOptions(Interpreter, Loaded.Instructions, Pool, BcOpts, Logger.get());
					return 0;
				}
				throw std::runtime_error("No file after --debug-bytecode / -dbg");
			}

			if(CliArgIs(Arg, {"--trig-relations", "-tg"})) {
				auto Catalog = AstralDB::LoadTriggerCatalog(ResolveTrigCatalog(SessionDbPath));
				if(Catalog.CatalogPath.empty())
					Catalog.CatalogPath = ResolveTrigCatalog(SessionDbPath);
				AstralDB::SQL::RebuildTriggerRelations(Catalog);
				std::cout << AstralDB::SQL::FormatTriggerRelations(Catalog);
				return 0;
			}
			if(CliArgIs(Arg, {"--trig-list", "-tl"})) {
				const auto Catalog = AstralDB::LoadTriggerCatalog(ResolveTrigCatalog(SessionDbPath));
				if(Catalog.Triggers.empty()) {
					std::cout << "(no triggers registered)\n";
					return 0;
				}
				for(const auto &E : Catalog.Triggers) {
					std::cout << E.Name << "\t" << E.TableName << "\t"
					          << AstralDB::TriggerTimingTag(E.Timing) << "\t"
					          << AstralDB::TriggerEventTag(E.Event) << "\t"
					          << (E.Enabled ? "enabled" : "disabled") << "\n";
				}
				return 0;
			}
			if(CliArgIs(Arg, {"--trig-info", "-ti"})) {
				if(I + 1 < Argc) {
					const std::string Name = Argv[++I];
					const auto Catalog = AstralDB::LoadTriggerCatalog(ResolveTrigCatalog(SessionDbPath));
					const auto Entry = AstralDB::FindTrigger(Catalog, Name);
					if(!Entry)
						throw std::runtime_error("Trigger not found: " + Name);
					std::cout << AstralDB::SQL::FormatTriggerEntrySummary(*Entry);
					return 0;
				}
				throw std::runtime_error("No name after --trig-info / -ti");
			}
			if(CliArgIs(Arg, {"--trig-fires", "-tf"})) {
				const auto Catalog = AstralDB::LoadTriggerCatalog(ResolveTrigCatalog(SessionDbPath));
				std::cout << AstralDB::SQL::FormatTriggerFireLog(Catalog);
				return 0;
			}
			if(CliArgIs(Arg, {"--proc-relations", "-pg", "--proc-graph"})) {
				auto Catalog = AstralDB::SQL::LoadProcedureCatalog(
				    ResolveProcCatalog(SessionDbPath, BcOpts.ProcCatalog));
				if(Catalog.CatalogPath.empty())
					Catalog.CatalogPath = ResolveProcCatalog(SessionDbPath, BcOpts.ProcCatalog);
				AstralDB::SQL::RebuildProcedureRelations(Catalog);
				std::cout << AstralDB::SQL::FormatProcedureRelations(Catalog);
				return 0;
			}
			if(CliArgIs(Arg, {"--proc-list", "-pl"})) {
				const auto Catalog = AstralDB::SQL::LoadProcedureCatalog(
				    ResolveProcCatalog(SessionDbPath, BcOpts.ProcCatalog));
				if(Catalog.Procedures.empty()) {
					std::cout << "(no procedures registered)\n";
					return 0;
				}
				for(const auto &P : Catalog.Procedures) {
					std::cout << P.Name << "\tabc=" << P.AbcPath.string();
					if(!P.SourceDialect.empty())
						std::cout << "\tdialect=" << P.SourceDialect;
					if(P.BytecodeMeta.HasExceptionHandlers)
						std::cout << "\texceptions=" << P.BytecodeMeta.ExceptionHandlerCount;
					if(!P.DependsOn.empty()) {
						std::cout << "\tdepends=";
						for(std::size_t D = 0; D < P.DependsOn.size(); ++D) {
							if(D)
								std::cout << ",";
							std::cout << P.DependsOn[D];
						}
					}
					if(!P.Description.empty())
						std::cout << "\t" << P.Description;
					std::cout << "\n";
				}
				return 0;
			}
			if(CliArgIs(Arg, {"--proc-register", "-pr"})) {
				if(I + 2 < Argc) {
					const std::string Name = Argv[++I];
					const std::filesystem::path Abc = Argv[++I];
					auto Catalog = AstralDB::SQL::LoadProcedureCatalog(
					    ResolveProcCatalog(SessionDbPath, BcOpts.ProcCatalog));
					if(Catalog.CatalogPath.empty())
						Catalog.CatalogPath = ResolveProcCatalog(SessionDbPath, BcOpts.ProcCatalog);
					AstralDB::SQL::RegisterProcedure(Catalog, Name, Abc);
					AstralDB::SQL::SaveProcedureCatalog(Catalog);
					std::cout << "Registered procedure \"" << Name << "\" -> " << Abc.string() << "\n";
					return 0;
				}
				throw std::runtime_error("--proc-register / -pr needs NAME and FILE");
			}
			if(CliArgIs(Arg, {"--proc-unregister", "-pu"})) {
				if(I + 1 < Argc) {
					const std::string Name = Argv[++I];
					auto Catalog = AstralDB::SQL::LoadProcedureCatalog(
					    ResolveProcCatalog(SessionDbPath, BcOpts.ProcCatalog));
					if(!AstralDB::SQL::UnregisterProcedure(Catalog, Name))
						throw std::runtime_error("Procedure not found: " + Name);
					AstralDB::SQL::SaveProcedureCatalog(Catalog);
					std::cout << "Unregistered procedure \"" << Name << "\"\n";
					return 0;
				}
				throw std::runtime_error("No name after --proc-unregister / -pu");
			}
			if(CliArgIs(Arg, {"--proc-info", "-pi"})) {
				if(I + 1 < Argc) {
					const std::string Name = Argv[++I];
					const auto Catalog = AstralDB::SQL::LoadProcedureCatalog(
					    ResolveProcCatalog(SessionDbPath, BcOpts.ProcCatalog));
					const auto Entry = AstralDB::SQL::FindProcedure(Catalog, Name);
					if(!Entry)
						throw std::runtime_error("Procedure not found: " + Name);
					std::cout << AstralDB::SQL::FormatProcedureEntrySummary(*Entry);
					const auto Loaded = AstralDB::SQL::LoadProcedureBytecode(*Entry, Catalog);
					std::cout << AstralDB::SQL::FormatBytecodeAnalysis(
					    AstralDB::SQL::AnalyzeBytecode(Loaded.Instructions));
					return 0;
				}
				throw std::runtime_error("No name after --proc-info / -pi");
			}
			if(CliArgIs(Arg, {"--proc-call", "-px"})) {
				if(I + 1 < Argc) {
					const std::string Name = Argv[++I];
					const auto Catalog = AstralDB::SQL::LoadProcedureCatalog(
					    ResolveProcCatalog(SessionDbPath, BcOpts.ProcCatalog));
					const auto Entry = AstralDB::SQL::FindProcedure(Catalog, Name);
					if(!Entry)
						throw std::runtime_error("Procedure not found: " + Name);
					const auto Loaded = AstralDB::SQL::LoadProcedureBytecode(*Entry, Catalog);
					AstralDB::SQL::BytecodeInterpreter Interpreter(Logger.get());
					Interpreter.DatabasePath(SessionDbPath);
					Interpreter.EnsurePrimaryDatabaseOpened();
					ApplyCliSessionAuth(Interpreter.PrimaryDatabase(), CliUser, CliPassword);
					ApplyCliAuditLog(Interpreter.PrimaryDatabase(), CliAuditFile);
					const std::vector<std::string> *Pool =
					    Loaded.StringPool.empty() ? nullptr : &Loaded.StringPool;
					ExecuteBytecodeWithOptions(Interpreter, Loaded.Instructions, Pool, BcOpts, Logger.get());
					if(Verbose)
						std::cout << AstralDB::SQL::DisassemblePretty(Loaded.Instructions) << "\n";
					return 0;
				}
				throw std::runtime_error("No name after --proc-call / -px");
			}
			if(Arg == "--export-bundle") {
				if(I + 1 >= Argc)
					throw std::runtime_error("No path after --export-bundle");
				const std::filesystem::path OutP = Argv[++I];
				std::string Fmt = "JSON";
				if(I + 2 < Argc && std::string(Argv[I + 1]) == "--export-format") {
					I += 2;
					Fmt = Argv[I];
				}
				AstralDB::Database Db(SessionDbPath, Logger.get());
				ApplyCliSessionAuth(&Db, CliUser, CliPassword);
				ApplyCliAuditLog(&Db, CliAuditFile);
				if(!Db.ExportBundle(OutP, Fmt))
					throw std::runtime_error("export-bundle failed");
				std::cout << "Exported database to " << OutP.string() << "\n";
				return 0;
			}
			if(Arg == "--import-bundle") {
				if(I + 1 >= Argc)
					throw std::runtime_error("No path after --import-bundle");
				const std::filesystem::path InP = Argv[++I];
				std::string Fmt = "JSON";
				if(I + 2 < Argc && std::string(Argv[I + 1]) == "--import-format") {
					I += 2;
					Fmt = Argv[I];
				}
				AstralDB::Database Db(SessionDbPath, Logger.get());
				ApplyCliSessionAuth(&Db, CliUser, CliPassword);
				ApplyCliAuditLog(&Db, CliAuditFile);
				if(!Db.ImportBundle(InP, Fmt))
					throw std::runtime_error("import-bundle failed");
				std::cout << "Imported bundle into " << SessionDbPath.string() << "\n";
				return 0;
			}
			if(Arg == "--convert") {
				if(I + 2 >= Argc)
					throw std::runtime_error("--convert needs SRC and DST paths");
				const std::filesystem::path Src = Argv[++I];
				const std::filesystem::path Dst = Argv[++I];
				std::string Sf = "CSV";
				std::string Df = "TSV";
				if(I + 2 < Argc && std::string(Argv[I + 1]) == "--from") {
					I += 2;
					Sf = Argv[I];
				}
				if(I + 2 < Argc && std::string(Argv[I + 1]) == "--to") {
					I += 2;
					Df = Argv[I];
				}
				if(!AstralDB::Database::ConvertTabularFiles(Src, Dst, Sf, Df))
					throw std::runtime_error("--convert failed");
				std::cout << "Converted " << Src.string() << " -> " << Dst.string() << "\n";
				return 0;
			}
			if(Arg == "-q" || Arg == "--query") {
				if(I + 1 < Argc) {
					std::string Query(Argv[++I]);
					AstralDB::SQL::BytecodeInterpreter Interpreter(Logger.get());
					Interpreter.DatabasePath(SessionDbPath);
					Interpreter.EnsurePrimaryDatabaseOpened();
					ApplyCliSessionAuth(Interpreter.PrimaryDatabase(), CliUser, CliPassword);
					ApplyCliAuditLog(Interpreter.PrimaryDatabase(), CliAuditFile);
					AstralDB::SQL::RunSqlScript(Interpreter, Query, Logger.get(), OptLevel);
					return 0;
				}
				throw std::runtime_error("No query provided after -q/--query");
			}
			if(Arg == "-c" || Arg == "--check") {
				if(I + 1 < Argc) {
					std::string Query = ReadFile(Argv[++I]);
					[[maybe_unused]] AstralDB::SQL::Parser Parser(Query);
					std::cout << "Query syntax OK\n";
					return 0;
				}
				throw std::runtime_error("No file provided after -c/--check");
			}
			if(Arg == "-s") {
				if(I + 1 < Argc) {
					std::string Query = ReadFile(Argv[++I]);
					AstralDB::SQL::BytecodeInterpreter Interpreter(Logger.get());
					Interpreter.DatabasePath(SessionDbPath);
					Interpreter.EnsurePrimaryDatabaseOpened();
					ApplyCliSessionAuth(Interpreter.PrimaryDatabase(), CliUser, CliPassword);
					ApplyCliAuditLog(Interpreter.PrimaryDatabase(), CliAuditFile);
					AstralDB::SQL::Parser Parser(Query);
					AstralDB::SQL::Bytecode Code =
					    AstralDB::SQL::BuildBytecode(Logger.get(), OptLevel, Interpreter.PrimaryDatabase());
					MaybeResumeQueryCheckpoint(Interpreter, Code);
					Interpreter.Execute(Code);
					if(Interpreter.PrimaryDatabase())
						AstralDB::DrainJoinFactStarPrecomputeJobs(*Interpreter.PrimaryDatabase());
					if(Verbose)
						std::cout << "Executed bytecode:\n" << AstralDB::SQL::DisassemblePretty(Code) << "\n";
					return 0;
				}
				throw std::runtime_error("No file provided after -s");
			}
			if(Arg == "--time-sql") {
				if(I + 1 < Argc) {
					using Clock = std::chrono::steady_clock;
					using Ms = std::chrono::duration<double, std::milli>;
					const std::string Query = ReadFile(Argv[++I]);
					const int Warmup = CliTimeSqlWarmup;
					const int Runs = CliTimeSqlRuns;
					const bool BenchMode = (Warmup > 0 || Runs > 1);
					const bool SplitSetup = CliTimeSqlSetupPath.has_value();

					AstralDB::SQL::BytecodeInterpreter CompileInterpreter(Logger.get());
					if(CompileInterpreter.SessionConfig().ProfileDumpPath)
						AstralDB::SQL::QueryProfiler::Instance().Reset();
					CompileInterpreter.DatabasePath(SessionDbPath);
					CompileInterpreter.EnsurePrimaryDatabaseOpened();
					ApplyCliSessionAuth(CompileInterpreter.PrimaryDatabase(), CliUser, CliPassword);
					ApplyCliAuditLog(CompileInterpreter.PrimaryDatabase(), CliAuditFile);
					std::optional<AstralDB::SQL::Bytecode> SetupCode;
					std::optional<AstralDB::SQL::Bytecode> QueryCode;
					double SetupCompileMs = 0.0;
					double QueryCompileMs = 0.0;
					AstralDB::Database *SchemaDb = CompileInterpreter.PrimaryDatabase();
					if(SplitSetup) {
						const auto TSetup0 = Clock::now();
						AstralDB::SQL::Parser SetupParser(ReadFile(*CliTimeSqlSetupPath));
						SetupCode =
						    AstralDB::SQL::BuildBytecode(Logger.get(), OptLevel, SchemaDb);
						SetupCompileMs = Ms(Clock::now() - TSetup0).count();
					} else {
						const auto TQuery0 = Clock::now();
						AstralDB::SQL::Parser Parser(Query);
						QueryCode =
						    AstralDB::SQL::BuildBytecode(Logger.get(), OptLevel, SchemaDb);
						QueryCompileMs = Ms(Clock::now() - TQuery0).count();
					}
					CompileInterpreter.ResetExecutionSession(std::nullopt, false);

					auto CompileQueryOverlapped = [&](AstralDB::SQL::BytecodeInterpreter &Interp) -> const AstralDB::SQL::Bytecode & {
						if(QueryCode)
							return *QueryCode;
						KickWalIoOverlap(Interp.PrimaryDatabase());
						const auto TQuery0 = Clock::now();
						AstralDB::SQL::Parser Parser(Query);
						QueryCode =
						    AstralDB::SQL::BuildBytecode(Logger.get(), OptLevel, Interp.MutatingDatabase());
						QueryCompileMs = Ms(Clock::now() - TQuery0).count();
						return *QueryCode;
					};

					auto RunExecuteOnPath = [&](AstralDB::SQL::BytecodeInterpreter &Interpreter,
					                              const std::filesystem::path &DbPath,
					                              const AstralDB::SQL::Bytecode &RunCode, double &OutExecMs,
					                              const bool TimeExecute, const bool ResetSession,
					                              const bool WipeOnDisk) {
						if(ResetSession)
							Interpreter.ResetExecutionSession(DbPath, WipeOnDisk);
						else {
							Interpreter.ResetVmState();
							Interpreter.ResetTimeSqlStats();
							if(!Interpreter.PrimaryDatabase()) {
								Interpreter.DatabasePath(DbPath);
								Interpreter.EnsurePrimaryDatabaseOpened();
							}
						}
						ApplyCliSessionAuth(Interpreter.MutatingDatabase(), CliUser, CliPassword);
						ApplyCliAuditLog(Interpreter.MutatingDatabase(), CliAuditFile);
						if(!TimeExecute) {
							MaybeResumeQueryCheckpoint(Interpreter, RunCode);
							Interpreter.Execute(RunCode);
							OutExecMs = 0.0;
							return;
						}
						const auto TExec0 = Clock::now();
						MaybeResumeQueryCheckpoint(Interpreter, RunCode);
						Interpreter.Execute(RunCode);
						const auto TExec1 = Clock::now();
						OutExecMs = Ms(TExec1 - TExec0).count();
					};

					auto MakeRunDbPath = [&]() -> std::filesystem::path {
						if(DbFromCli)
							return SessionDbPath;
						if(MemoryOnly)
							return MakeSessionDatabasePath(true);
						return std::filesystem::path("astral.db");
					};

					const bool PersistCliDb = DbFromCli && !SplitSetup;

					if(!BenchMode) {
						std::vector<EphemeralSessionDatabase> EphemeralHold;
						const std::filesystem::path DbPath = MakeRunDbPath();
						if(MemoryOnly && !DbFromCli) {
							EphemeralSessionDatabase Ep;
							Ep.Path = DbPath;
							Ep.Remove = true;
							EphemeralHold.push_back(std::move(Ep));
						}
						AstralDB::SQL::BytecodeInterpreter Interpreter(Logger.get());
						double ExecMs = 0.0;
						if(SetupCode) {
							double Discard = 0.0;
							RunExecuteOnPath(Interpreter, DbPath, *SetupCode, Discard, false, true, true);
							if(Interpreter.PrimaryDatabase())
								AstralDB::DrainJoinFactStarPrecomputeJobs(*Interpreter.PrimaryDatabase());
						}
						const AstralDB::SQL::Bytecode &Code = CompileQueryOverlapped(Interpreter);
						RunExecuteOnPath(Interpreter, DbPath, Code, ExecMs, true,
						                 SetupCode.has_value() ? false : !PersistCliDb,
						                 SetupCode.has_value() ? false : !PersistCliDb);
						double WalQuiesceMs = 0.0;
						if(CliTimeSqlDurable)
							WalQuiesceMs = QuiesceWalIoMs(Interpreter.PrimaryDatabase());
						CommitDurableDatabase(Interpreter);
						const double CompileMs = SetupCompileMs + QueryCompileMs;
						auto Stats = Interpreter.LastTimeSqlStats();
						AstralDB::SQL::ValidateTimeSqlIntegrity(Stats, ExecMs);
						{
							std::ostringstream O;
							O << std::fixed << std::setprecision(3);
							O << "[time-sql] parse+compile_ms=" << CompileMs << " execute_ms=" << ExecMs
							  << " total_ms=" << (CompileMs + ExecMs) << " scanned_rows=" << Stats.RowsScanned
							  << " result_rows=" << Stats.ResultRows;
							if(Stats.FastPathFlags != 0)
								O << " fast_path_flags=" << Stats.FastPathFlags;
							AstralDB::SQL::AppendShapeTelemetryFields(O, Stats);
							if(Stats.MinRowsScannedExpected > 0)
								O << " min_scanned_expected=" << Stats.MinRowsScannedExpected;
							if(Stats.IntegrityFailed)
								O << " integrity_fail=1 msg=" << Stats.IntegrityMessage;
							AppendWalTimingFields(O, WalQuiesceMs, SetupCompileMs, QueryCompileMs);
							O << "\n";
							PrintTimeSqlLine(Logger.get(), O.str());
						}
						FinalizeTimeSqlProfile(Interpreter, SetupCompileMs + QueryCompileMs, ExecMs);
						return Stats.IntegrityFailed ? 2 : 0;
					}

					std::vector<EphemeralSessionDatabase> EphemeralHold;
					AstralDB::SQL::BytecodeInterpreter BenchInterpreter(Logger.get());
					bool EphemeralBound = false;
					auto BindEphemeral = [&](const std::filesystem::path &DbPath) {
						if(MemoryOnly && !DbFromCli && !EphemeralBound) {
							EphemeralSessionDatabase Ep;
							Ep.Path = DbPath;
							Ep.Remove = true;
							EphemeralHold.push_back(std::move(Ep));
							EphemeralBound = true;
						}
					};
					const std::filesystem::path BenchDbPath = MakeRunDbPath();
					BindEphemeral(BenchDbPath);
					if(SetupCode) {
						double Discard = 0.0;
						RunExecuteOnPath(BenchInterpreter, BenchDbPath, *SetupCode, Discard, false, true, true);
						if(BenchInterpreter.PrimaryDatabase())
							AstralDB::DrainJoinFactStarPrecomputeJobs(*BenchInterpreter.PrimaryDatabase());
					}
					const AstralDB::SQL::Bytecode &Code = CompileQueryOverlapped(BenchInterpreter);
					auto RunBenchExecute = [&](double &OutExecMs, const bool TimeExecute) {
						if(SetupCode)
							RunExecuteOnPath(BenchInterpreter, BenchDbPath, Code, OutExecMs, TimeExecute, false, false);
						else
							RunExecuteOnPath(BenchInterpreter, BenchDbPath, Code, OutExecMs, TimeExecute, !PersistCliDb,
							                 !PersistCliDb);
					};

					for(int W = 0; W < Warmup; ++W) {
						double Discard = 0.0;
						RunBenchExecute(Discard, false);
					}
					std::vector<double> Samples;
					Samples.reserve(static_cast<size_t>(Runs));
					for(int R = 0; R < Runs; ++R) {
						AstralDB::SQL::QueryProfiler::Instance().ClearRegionTimings();
						double ExecMs = 0.0;
						RunBenchExecute(ExecMs, true);
						Samples.push_back(ExecMs);
					}
					std::sort(Samples.begin(), Samples.end());
					const double ExecMedian = Samples[Samples.size() / 2];
					const double ExecMin = Samples.front();
					const double ExecMax = Samples.back();
					auto Stats = BenchInterpreter.LastTimeSqlStats();
					AstralDB::SQL::ValidateTimeSqlIntegrity(Stats, ExecMedian);
					double WalQuiesceMs = 0.0;
					if(CliTimeSqlDurable)
						WalQuiesceMs = QuiesceWalIoMs(BenchInterpreter.PrimaryDatabase());
					CommitDurableDatabase(BenchInterpreter);
					const double CompileMs = SetupCompileMs + QueryCompileMs;
					{
						std::ostringstream O;
						O << std::fixed << std::setprecision(3);
						O << "[time-sql] parse+compile_ms=" << CompileMs << " execute_median_ms=" << ExecMedian
						  << " execute_min_ms=" << ExecMin << " execute_max_ms=" << ExecMax
						  << " execute_ms=" << ExecMedian << " total_ms=" << (CompileMs + ExecMedian)
						  << " runs=" << Runs << " scanned_rows=" << Stats.RowsScanned
						  << " result_rows=" << Stats.ResultRows;
						if(Stats.FastPathFlags != 0)
							O << " fast_path_flags=" << Stats.FastPathFlags;
						AstralDB::SQL::AppendShapeTelemetryFields(O, Stats);
						if(Stats.MinRowsScannedExpected > 0)
							O << " min_scanned_expected=" << Stats.MinRowsScannedExpected;
						if(Stats.IntegrityFailed)
							O << " integrity_fail=1 msg=" << Stats.IntegrityMessage;
						AppendWalTimingFields(O, WalQuiesceMs, SetupCompileMs, QueryCompileMs);
						O << "\n";
						PrintTimeSqlLine(Logger.get(), O.str());
					}
					FinalizeTimeSqlProfile(BenchInterpreter, CompileMs, ExecMedian);
					return Stats.IntegrityFailed ? 2 : 0;
				}
				throw std::runtime_error("No file provided after --time-sql");
			}
			if(Arg == "--time-sql-suite") {
				if(I + 1 >= Argc)
					throw std::runtime_error("No file provided after --time-sql-suite");
				if(!CliTimeSqlSetupPath.has_value())
					throw std::runtime_error("--time-sql-suite requires --time-sql-setup FILE");
				using Clock = std::chrono::steady_clock;
				using Ms = std::chrono::duration<double, std::milli>;
				const std::string SuiteText = ReadFile(Argv[++I]);
				std::vector<std::pair<std::string, std::filesystem::path>> Entries;
				std::istringstream SuiteStream(SuiteText);
				for(std::string Line; std::getline(SuiteStream, Line);) {
					while(!Line.empty() && (Line.back() == '\r' || Line.back() == '\n' || Line.back() == ' '))
						Line.pop_back();
					if(Line.empty() || Line[0] == '#')
						continue;
					const std::size_t Tab = Line.find('\t');
					if(Tab == std::string::npos)
						continue;
					Entries.emplace_back(Line.substr(0, Tab), std::filesystem::path(Line.substr(Tab + 1)));
				}
				if(Entries.empty())
					throw std::runtime_error("--time-sql-suite manifest is empty");
				std::vector<EphemeralSessionDatabase> EphemeralHold;
				const std::filesystem::path DbPath =
				    MemoryOnly && !DbFromCli ? MakeSessionDatabasePath(true) : SessionDbPath;
				if(MemoryOnly && !DbFromCli) {
					EphemeralSessionDatabase Ep;
					Ep.Path = DbPath;
					Ep.Remove = true;
					EphemeralHold.push_back(std::move(Ep));
				}
				AstralDB::SQL::BytecodeInterpreter Interpreter(Logger.get());
				Interpreter.DatabasePath(DbPath);
				Interpreter.EnsurePrimaryDatabaseOpened();
				ApplyCliSessionAuth(Interpreter.PrimaryDatabase(), CliUser, CliPassword);
				ApplyCliAuditLog(Interpreter.PrimaryDatabase(), CliAuditFile);
				AstralDB::SQL::Parser SetupParser(ReadFile(*CliTimeSqlSetupPath));
				const AstralDB::SQL::Bytecode SetupCode =
				    AstralDB::SQL::BuildBytecode(Logger.get(), OptLevel, Interpreter.PrimaryDatabase());
				Interpreter.ResetExecutionSession(DbPath, true);
				ApplyCliSessionAuth(Interpreter.MutatingDatabase(), CliUser, CliPassword);
				ApplyCliAuditLog(Interpreter.MutatingDatabase(), CliAuditFile);
				Interpreter.Execute(SetupCode);
				if(Interpreter.PrimaryDatabase())
					AstralDB::DrainJoinFactStarPrecomputeJobs(*Interpreter.PrimaryDatabase());
				KickWalIoOverlap(Interpreter.PrimaryDatabase());
				int FailRc = 0;
				for(const auto &[Label, QueryPath] : Entries) {
					const auto TParse0 = Clock::now();
					AstralDB::SQL::Parser Parser(ReadFile(QueryPath.string()));
					AstralDB::SQL::Bytecode Code =
					    AstralDB::SQL::BuildBytecode(Logger.get(), OptLevel, Interpreter.MutatingDatabase());
					const auto TParse1 = Clock::now();
					const double CompileMs = Ms(TParse1 - TParse0).count();
					Interpreter.ResetVmState();
					Interpreter.ResetTimeSqlStats();
					const auto TExec0 = Clock::now();
					MaybeResumeQueryCheckpoint(Interpreter, Code);
					Interpreter.Execute(Code);
					const auto TExec1 = Clock::now();
					const double ExecMs = Ms(TExec1 - TExec0).count();
					double WalQuiesceMs = 0.0;
					if(CliTimeSqlDurable)
						WalQuiesceMs = QuiesceWalIoMs(Interpreter.PrimaryDatabase());
					CommitDurableDatabase(Interpreter);
					auto Stats = Interpreter.LastTimeSqlStats();
					AstralDB::SQL::ValidateTimeSqlIntegrity(Stats, ExecMs);
					{
						std::ostringstream O;
						O << std::fixed << std::setprecision(3);
						O << "[time-sql] query=" << Label << " parse+compile_ms=" << CompileMs
						  << " query_compile_ms=" << CompileMs << " execute_ms=" << ExecMs
						  << " total_ms=" << (CompileMs + ExecMs)
						  << " scanned_rows=" << Stats.RowsScanned << " result_rows=" << Stats.ResultRows;
						if(Stats.FastPathFlags != 0)
							O << " fast_path_flags=" << Stats.FastPathFlags;
						AstralDB::SQL::AppendShapeTelemetryFields(O, Stats);
						if(Stats.IntegrityFailed)
							O << " integrity_fail=1 msg=" << Stats.IntegrityMessage;
						AppendWalTimingFields(O, WalQuiesceMs, 0.0, CompileMs);
						O << "\n";
						PrintTimeSqlLine(Logger.get(), O.str());
					}
					if(Stats.IntegrityFailed)
						FailRc = 2;
				}
				return FailRc;
			}
			if(Arg == "-r" || Arg == "--repl") {
				RunREPL(*Logger, SessionDbPath, OptLevel, CliUser, CliPassword, CliAuditFile);
				return 0;
			}
			if(Arg == "-fb" || Arg == "--from-bytecode") {
				if(I + 1 < Argc) {
					const auto Loaded = AstralDB::SQL::LoadAbcFile(Argv[++I]);
					AstralDB::SQL::BytecodeInterpreter Interpreter(Logger.get());
					Interpreter.DatabasePath(SessionDbPath);
					Interpreter.EnsurePrimaryDatabaseOpened();
					ApplyCliSessionAuth(Interpreter.PrimaryDatabase(), CliUser, CliPassword);
					ApplyCliAuditLog(Interpreter.PrimaryDatabase(), CliAuditFile);
					const std::vector<std::string> *Pool =
					    Loaded.StringPool.empty() ? nullptr : &Loaded.StringPool;
					ExecuteBytecodeWithOptions(Interpreter, Loaded.Instructions, Pool, BcOpts, Logger.get());
					if(Verbose)
						std::cout << "Executed bytecode:\n"
						          << AstralDB::SQL::DisassemblePretty(Loaded.Instructions) << "\n";
					return 0;
				}
				throw std::runtime_error("No file provided after -fb/--from-bytecode");
			}
			if(Arg == "-cc" || Arg == "--compile") {
				if(I + 1 < Argc) {
					std::string Query = ReadFile(Argv[++I]);
					AstralDB::SQL::Parser Parser(Query);
					if(BcOpts.CompileWithStringPool) {
						const auto Compiled =
						    AstralDB::SQL::BuildCompiledBytecode(Logger.get(), OptLevel);
						AstralDB::SQL::SaveAbcFile(CompileOut, Compiled);
					} else {
						AstralDB::SQL::Bytecode Code = AstralDB::SQL::BuildBytecode(Logger.get(), OptLevel);
						AstralDB::SQL::SaveAbcFile(CompileOut, Code);
					}
					std::cout << "Bytecode written to " << CompileOut << "\n";
					return 0;
				}
				throw std::runtime_error("No file provided after -cc/--compile");
			}
			if(Arg[0] != '-') {
				std::string Query = ReadFile(Arg);
				AstralDB::SQL::BytecodeInterpreter Interpreter(Logger.get());
				Interpreter.DatabasePath(SessionDbPath);
				Interpreter.EnsurePrimaryDatabaseOpened();
				ApplyCliSessionAuth(Interpreter.PrimaryDatabase(), CliUser, CliPassword);
				ApplyCliAuditLog(Interpreter.PrimaryDatabase(), CliAuditFile);
				AstralDB::SQL::Parser Parser(Query);
				AstralDB::SQL::Bytecode Code =
				    AstralDB::SQL::BuildBytecode(Logger.get(), OptLevel, Interpreter.PrimaryDatabase());
				MaybeResumeQueryCheckpoint(Interpreter, Code);
				Interpreter.Execute(Code);
				if(Verbose)
					std::cout << "Executed bytecode:\n" << AstralDB::SQL::DisassemblePretty(Code) << "\n";
				return 0;
			}
		}
	} catch(const std::exception& e) {
		AstralDB::Err::PrintCliError(std::cerr, e.what());
		if(Logger)
			Logger->Error("Fatal error: " + std::string(e.what()));
		return -1;
	}

	return 0;
}
