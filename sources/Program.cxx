#include <SQL/SQL.hxx>
#include <SQL/BytecodeInterpreter.hxx>
#include <SQL/Bytecode.hxx>
#include <SQL/BytecodeFormat.hxx>
#include <SQL/BytecodeInspect.hxx>
#include <SQL/BytecodeDebug.hxx>
#include <SQL/BytecodeProcedures.hxx>
#include <SQL/BytecodeTriggers.hxx>
#include <IO/Logger.hxx>
#include <IO/Error.hxx>
#include <Database/Database.hxx>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <vector>

// Helper function to read file contents
std::string ReadFile(const std::string& Path) {
	std::ifstream File(Path, std::ios::binary);
	if (!File) {
		throw std::runtime_error(
		    AstralDB::Err::Prefixed("CLI", "Cannot open query file (check path and permissions): " + Path));
	}
	std::string Contents((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
	if (Contents.empty()) {
		throw std::runtime_error(
		    AstralDB::Err::Prefixed("CLI", "Query file is empty: " + Path));
	}
	if (Contents.size() >= 3 && static_cast<unsigned char>(Contents[0]) == 0xEF && static_cast<unsigned char>(Contents[1]) == 0xBB &&
	    static_cast<unsigned char>(Contents[2]) == 0xBF)
		Contents.erase(0, 3);
	return Contents;
}

static constexpr const char* AstralDbVersionString = "2.0-rc1";

struct CliBytecodeOptions {
	bool TraceExecution = false;
	std::size_t DebugMaxSteps = 0;
	std::vector<std::size_t> Breakpoints;
	std::optional<std::filesystem::path> ProcCatalog;
	std::optional<std::string> BytecodeAspect;
	bool CompileWithStringPool = false;
};

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
	std::cout << "  SQL: CREATE PROCEDURE n AS (…); CALL n; DROP PROCEDURE n;\n";
	std::cout << "  Triggers (catalog + astraldb_triggers_cache/*.abc):\n";
	std::cout << "  --trig-list (-tl)                 List triggers\n";
	std::cout << "  --trig-info NAME (-ti)            Trigger metadata\n";
	std::cout << "  --trig-relations (-tg)            Table → trigger index\n";
	std::cout << "  --trig-fires (-tf)                Recent trigger fire log\n";
	std::cout << "  SQL: CREATE TRIGGER …; ALTER TRIGGER … ENABLE|DISABLE; DROP TRIGGER …;\n";
}


static std::filesystem::path ResolveTrigCatalog(const std::filesystem::path &SessionDb) {
	return AstralDB::SQL::DefaultTriggerCatalogPath(SessionDb);
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
		} else if(BcOpts && CliArgIs(Arg, {"--proc-catalog", "-pc"})) {
			if(I + 1 < Argc)
				BcOpts->ProcCatalog = std::filesystem::path(Argv[++I]);
			else
				throw std::runtime_error("No path after --proc-catalog / -pc");
		}
	}
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
	std::cout << "AstralDB REPL v" << AstralDbVersionString << "\n";
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
			system("cls");
			#else
			system("clear");
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
				std::cout << "AstralDB " << AstralDbVersionString << "\n";
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
			if(Arg == "--time-sql") {
				if(I + 1 < Argc)
					++I;
				continue;
			}
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
				auto Catalog = AstralDB::SQL::LoadTriggerCatalog(ResolveTrigCatalog(SessionDbPath));
				if(Catalog.CatalogPath.empty())
					Catalog.CatalogPath = ResolveTrigCatalog(SessionDbPath);
				AstralDB::SQL::RebuildTriggerRelations(Catalog);
				std::cout << AstralDB::SQL::FormatTriggerRelations(Catalog);
				return 0;
			}
			if(CliArgIs(Arg, {"--trig-list", "-tl"})) {
				const auto Catalog = AstralDB::SQL::LoadTriggerCatalog(ResolveTrigCatalog(SessionDbPath));
				if(Catalog.Triggers.empty()) {
					std::cout << "(no triggers registered)\n";
					return 0;
				}
				for(const auto &E : Catalog.Triggers) {
					std::cout << E.Name << "\t" << E.TableName << "\t"
					          << AstralDB::SQL::TriggerTimingTag(E.Timing) << "\t"
					          << AstralDB::SQL::TriggerEventTag(E.Event) << "\t"
					          << (E.Enabled ? "enabled" : "disabled") << "\n";
				}
				return 0;
			}
			if(CliArgIs(Arg, {"--trig-info", "-ti"})) {
				if(I + 1 < Argc) {
					const std::string Name = Argv[++I];
					const auto Catalog = AstralDB::SQL::LoadTriggerCatalog(ResolveTrigCatalog(SessionDbPath));
					const auto Entry = AstralDB::SQL::FindTrigger(Catalog, Name);
					if(!Entry)
						throw std::runtime_error("Trigger not found: " + Name);
					std::cout << AstralDB::SQL::FormatTriggerEntrySummary(*Entry);
					return 0;
				}
				throw std::runtime_error("No name after --trig-info / -ti");
			}
			if(CliArgIs(Arg, {"--trig-fires", "-tf"})) {
				const auto Catalog = AstralDB::SQL::LoadTriggerCatalog(ResolveTrigCatalog(SessionDbPath));
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
					AstralDB::SQL::Parser Parser(Query);
					AstralDB::SQL::Bytecode Code =
					    AstralDB::SQL::BuildBytecode(Logger.get(), OptLevel, Interpreter.PrimaryDatabase());
					Interpreter.Execute(Code);
					if(Verbose)
						std::cout << "Executed bytecode:\n" << AstralDB::SQL::DisassemblePretty(Code) << "\n";
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
					Interpreter.Execute(Code);
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
					const auto TParse0 = Clock::now();
					AstralDB::SQL::BytecodeInterpreter Interpreter(Logger.get());
					Interpreter.DatabasePath(SessionDbPath);
					Interpreter.EnsurePrimaryDatabaseOpened();
					ApplyCliSessionAuth(Interpreter.PrimaryDatabase(), CliUser, CliPassword);
					ApplyCliAuditLog(Interpreter.PrimaryDatabase(), CliAuditFile);
					AstralDB::SQL::Parser Parser(Query);
					AstralDB::SQL::Bytecode Code =
					    AstralDB::SQL::BuildBytecode(Logger.get(), OptLevel, Interpreter.PrimaryDatabase());
					const auto TParse1 = Clock::now();
					const auto TExec0 = Clock::now();
					Interpreter.Execute(Code);
					const auto TExec1 = Clock::now();
					std::cerr << "[time-sql] parse+compile_ms=" << std::fixed << std::setprecision(3)
					          << Ms(TParse1 - TParse0).count() << " execute_ms=" << Ms(TExec1 - TExec0).count()
					          << " total_ms=" << Ms(TExec1 - TParse0).count() << "\n";
					return 0;
				}
				throw std::runtime_error("No file provided after --time-sql");
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
