#include <SQL/SQL.hxx>
#include <SQL/BytecodeInterpreter.hxx>
#include <SQL/Bytecode.hxx>
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

// Helper function to serialize bytecode
void SerializeBytecode(const AstralDB::SQL::Bytecode& Code, const std::string& Path) {
	std::ofstream Out(Path, std::ios::binary);
	if (!Out) {
		throw std::runtime_error(
		    AstralDB::Err::Prefixed("CLI", "Cannot open bytecode output file for write: " + Path));
	}

	// Write version header
	const uint32_t Version = 1;
	Out.write(reinterpret_cast<const char*>(&Version), sizeof(Version));

	// Write number of instructions
	const uint32_t NumInstructions = static_cast<uint32_t>(Code.size());
	Out.write(reinterpret_cast<const char*>(&NumInstructions), sizeof(NumInstructions));

	// Write each instruction
	for (const auto& Inst : Code) {
		// Write opcode
		const uint8_t Opcode = static_cast<uint8_t>(Inst.Opcode);
		Out.write(reinterpret_cast<const char*>(&Opcode), sizeof(Opcode));

		// Write number of operands
		const uint32_t NumOperands = static_cast<uint32_t>(Inst.Operands.size());
		Out.write(reinterpret_cast<const char*>(&NumOperands), sizeof(NumOperands));

		// Write each operand
		for (const auto& Operand : Inst.Operands) {
			std::visit([&Out](const auto& Value) {
				using T = std::decay_t<decltype(Value)>;
				if constexpr (std::is_same_v<T, int64_t>) {
					const uint8_t Type = 0;
					Out.write(reinterpret_cast<const char*>(&Type), sizeof(Type));
					Out.write(reinterpret_cast<const char*>(&Value), sizeof(Value));
				} else if constexpr (std::is_same_v<T, std::string>) {
					const uint8_t Type = 1;
					Out.write(reinterpret_cast<const char*>(&Type), sizeof(Type));
					const uint32_t Length = static_cast<uint32_t>(Value.length());
					Out.write(reinterpret_cast<const char*>(&Length), sizeof(Length));
					Out.write(Value.c_str(), Length);
				}
			}, Operand);
		}
	}
}

// Helper function to deserialize bytecode
AstralDB::SQL::Bytecode DeserializeBytecode(const std::string& Path) {
	std::ifstream In(Path, std::ios::binary);
	if (!In) {
		throw std::runtime_error(
		    AstralDB::Err::Prefixed("CLI", "Cannot open bytecode input file: " + Path));
	}

	// Read version header
	uint32_t Version;
	In.read(reinterpret_cast<char*>(&Version), sizeof(Version));
	if (Version != 1) {
		throw std::runtime_error(AstralDB::Err::Prefixed(
		    "CLI", "Unsupported bytecode file version " + std::to_string(Version) + " (expected 1)."));
	}

	// Read number of instructions
	uint32_t NumInstructions;
	In.read(reinterpret_cast<char*>(&NumInstructions), sizeof(NumInstructions));

	AstralDB::SQL::Bytecode Code;
	Code.reserve(NumInstructions);

	// Read each instruction
	for (uint32_t i = 0; i < NumInstructions; ++i) {
		// Read opcode
		uint8_t Opcode;
		In.read(reinterpret_cast<char*>(&Opcode), sizeof(Opcode));

		// Read number of operands
		uint32_t NumOperands;
		In.read(reinterpret_cast<char*>(&NumOperands), sizeof(NumOperands));

		// Create instruction
		AstralDB::SQL::Instruction Inst;
		Inst.Opcode = static_cast<AstralDB::SQL::Opcode>(Opcode);
		Inst.Operands.reserve(NumOperands);

		// Read each operand
		for (uint32_t j = 0; j < NumOperands; ++j) {
			uint8_t Type;
			In.read(reinterpret_cast<char*>(&Type), sizeof(Type));

			if (Type == 0) { // int64_t
				int64_t Value;
				In.read(reinterpret_cast<char*>(&Value), sizeof(Value));
				Inst.Operands.push_back(Value);
			} else if (Type == 1) { // string
				uint32_t Length;
				In.read(reinterpret_cast<char*>(&Length), sizeof(Length));
				std::string Value(Length, '\0');
				In.read(&Value[0], Length);
				Inst.Operands.push_back(Value);
			} else {
				throw std::runtime_error("Unknown operand type: " + std::to_string(Type));
			}
		}

		Code.push_back(std::move(Inst));
	}

	return Code;
}

static constexpr const char* AstralDbVersionString = "1.0";

static void ApplyCliGlobalFlags(int Argc, char** Argv, bool& Verbose, std::string& LogFile,
    AstralDB::SQL::OptimizationLevel& OptLevel, bool& MemoryOnly, std::string& CompileOut,
    std::filesystem::path *DatabasePathOpt, bool *DatabasePathProvided,
    std::optional<std::string> *CliUserOut, std::optional<std::string> *CliPasswordOut,
    std::optional<std::filesystem::path> *AuditFileOut) {
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
		else if(Arg == "--audit-file" && AuditFileOut) {
			if(I + 1 < Argc)
				*AuditFileOut = std::filesystem::path(Argv[++I]);
			else
				throw std::runtime_error("No path provided after --audit-file");
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
				std::cout << "Executed bytecode:\n" << AstralDB::SQL::Disassemble(Code) << "\n";
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
		ApplyCliGlobalFlags(Argc, Argv, Verbose, LogFile, OptLevel, MemoryOnly, CompileOut, &CliDbPath,
		                    &DbFromCli, &CliUser, &CliPassword, &CliAuditFile);

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
			if(Arg == "-O0" || Arg == "-O1" || Arg == "-O2" || Arg == "-O3")
				continue;
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
						std::cout << "Executed bytecode:\n" << AstralDB::SQL::Disassemble(Code) << "\n";
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
						std::cout << "Executed bytecode:\n" << AstralDB::SQL::Disassemble(Code) << "\n";
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
					AstralDB::SQL::Bytecode Code = DeserializeBytecode(Argv[++I]);
					AstralDB::SQL::BytecodeInterpreter Interpreter(Logger.get());
					Interpreter.DatabasePath(SessionDbPath);
					Interpreter.EnsurePrimaryDatabaseOpened();
					ApplyCliSessionAuth(Interpreter.PrimaryDatabase(), CliUser, CliPassword);
					ApplyCliAuditLog(Interpreter.PrimaryDatabase(), CliAuditFile);
					Interpreter.Execute(Code);
					if(Verbose)
						std::cout << "Executed bytecode:\n" << AstralDB::SQL::Disassemble(Code) << "\n";
					return 0;
				}
				throw std::runtime_error("No file provided after -fb/--from-bytecode");
			}
			if(Arg == "-cc" || Arg == "--compile") {
				if(I + 1 < Argc) {
					std::string Query = ReadFile(Argv[++I]);
					AstralDB::SQL::Parser Parser(Query);
					AstralDB::SQL::Bytecode Code = AstralDB::SQL::BuildBytecode(Logger.get(), OptLevel);
					SerializeBytecode(Code, CompileOut);
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
					std::cout << "Executed bytecode:\n" << AstralDB::SQL::Disassemble(Code) << "\n";
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
