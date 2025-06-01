#include <SQL/SQL.hxx>
#include <SQL/BytecodeInterpreter.hxx>
#include <SQL/Bytecode.hxx>
#include <IO/Logger.hxx>
#include <fstream>
#include <sstream>
#include <iostream>

int main(int Argc, char **Argv) {
	bool Verbose = false;
	std::string LogFile = "astraldb.log";
	for(int I = 1; I < Argc; ++I) {
		std::string Arg(Argv[I]);
		if(Arg == "-V" || Arg == "--verbose") {
			Verbose = true;
		} else if(Arg == "-l" || Arg == "--log-file") {
			if(I + 1 < Argc) {
				LogFile = Argv[++I];
			}
		}
	}
	AstralDB::Logger Logger(LogFile, Verbose);

	for(int I = 1; I < Argc; ++I) {
		std::string Arg(Argv[I]);
		if(Arg == "-h" || Arg == "--help") {
			std::cout << "Usage: astraldb [options]\n";
			std::cout << "-h, --help\t\tDisplay help\n";
			std::cout << "-v, --version\t\tShow version\n";
			std::cout << "-q, --query \"QUERY\"\tExecutes provided query\n";
			std::cout << "-r, --repl\t\tRun in REPL mode\n";
			std::cout << "-c, --check FILE\tCheck input query file only\n";
			std::cout << "-V, --verbose\t\tEnable verbose output\n";
			std::cout << "-fb, --from-bytecode FILE\tRun input bytecode\n";
			std::cout << "-cc, --compile FILE\tCompile query to bytecode\n";
			std::cout << "-l, --log-file FILE\tSave logs/audits to file\n";
			std::cout << "-s FILE\t\tEvaluate, compile, and run query file\n";
			std::cout << "-m, --mmap\t\tStore database in memory only\n";
			return 0;
		} else if(Arg == "-v" || Arg == "--version") {
			std::cout << "AstralDB version 0.0.1\n";
			return 0;
		} else if(Arg == "-q" || Arg == "--query") {
			if(I + 1 < Argc) {
				std::string_view Query(Argv[++I]);
				AstralDB::SQL::Parser Parser(Query);
				Parser.DumpAST();
				return 0;
			} else {
				std::cout << "AstralDB: No query provided after -q/--query\n";
				return -1;
			}
		} else if(Arg == "-c" || Arg == "--check") {
			if(I + 1 < Argc) {
				std::ifstream QueryFile(Argv[++I], std::ios::binary);
				if(!QueryFile) {
					std::cout << "AstralDB: File " << Argv[I] << " does not exist\n";
					return -1;
				}
				std::string QueryTemp((std::istreambuf_iterator<char>(QueryFile)), std::istreambuf_iterator<char>());
				if(QueryTemp.empty()) {
					std::cout << "AstralDB: File " << Argv[I] << " is empty\n";
					return -1;
				}
				AstralDB::SQL::Parser Parser(QueryTemp);
				std::cout << "Query syntax OK\n";
				return 0;
			} else {
				std::cout << "AstralDB: No file provided after -c/--check\n";
				return -1;
			}
		} else if(Arg == "-s") {
			if(I + 1 < Argc) {
				std::ifstream QueryFile(Argv[++I], std::ios::binary);
				if(!QueryFile) {
					std::cout << "AstralDB: File " << Argv[I] << " does not exist\n";
					return -1;
				}
				std::string QueryTemp((std::istreambuf_iterator<char>(QueryFile)), std::istreambuf_iterator<char>());
				if(QueryTemp.empty()) {
					std::cout << "AstralDB: File " << Argv[I] << " is empty\n";
					return -1;
				}
				AstralDB::SQL::Parser Parser(QueryTemp);
				Parser.DumpAST();
				AstralDB::SQL::Bytecode Code = AstralDB::SQL::BuildBytecode();
				AstralDB::SQL::BytecodeInterpreter().Execute(Code);
				std::cout << "Executed bytecode:\n" << AstralDB::SQL::Disassemble(Code) << "\n";
				return 0;
			} else {
				std::cout << "AstralDB: No file provided after -s\n";
				return -1;
			}
		} else if(Arg == "-r" || Arg == "--repl") {
			std::cout << "AstralDB REPL mode (not implemented yet)\n";
			return 0;
		} else if(Arg == "-fb" || Arg == "--from-bytecode") {
			if(I + 1 < Argc) {
				std::ifstream BytecodeFile(Argv[++I], std::ios::binary);
				if(!BytecodeFile) {
					std::cout << "AstralDB: Bytecode file " << Argv[I] << " does not exist\n";
					return -1;
				}
				std::stringstream Buffer;
				Buffer << BytecodeFile.rdbuf();
				std::string BytecodeStr = Buffer.str();
				// TODO: Proper bytecode deserialization
				std::cout << "[Warning] Bytecode deserialization is not implemented.\n";
				// AstralDB::SQL::Bytecode Code = ...
				// AstralDB::SQL::BytecodeInterpreter().Execute(Code);
				return 0;
			} else {
				std::cout << "AstralDB: No file provided after -fb/--from-bytecode\n";
				return -1;
			}
		} else if(Arg == "-cc" || Arg == "--compile") {
			if(I + 1 < Argc) {
				std::ifstream QueryFile(Argv[++I], std::ios::binary);
				if(!QueryFile) {
					std::cout << "AstralDB: File " << Argv[I] << " does not exist\n";
					return -1;
				}
				std::string QueryTemp((std::istreambuf_iterator<char>(QueryFile)), std::istreambuf_iterator<char>());
				if(QueryTemp.empty()) {
					std::cout << "AstralDB: File " << Argv[I] << " is empty\n";
					return -1;
				}
				AstralDB::SQL::Parser Parser(QueryTemp);
				AstralDB::SQL::Bytecode Code = AstralDB::SQL::BuildBytecode();
				std::ofstream Out("out.abc", std::ios::binary);
				if(!Out) {
					std::cout << "AstralDB: Could not open output file out.abc\n";
					return -1;
				}
				// TODO: Proper bytecode serialization
				Out << AstralDB::SQL::Disassemble(Code);
				std::cout << "Bytecode written to out.abc (disassembled text, not binary)\n";
				return 0;
			} else {
				std::cout << "AstralDB: No file provided after -cc/--compile\n";
				return -1;
			}
		} else if(Arg == "-V" || Arg == "--verbose") {
			Verbose = true;
			Logger.SetVerbose(true);
			std::cout << "AstralDB: Verbose mode enabled\n";
			Logger.Info("Verbose mode enabled");
		} else if(Arg == "-l" || Arg == "--log-file") {
			if(I + 1 < Argc) {
				LogFile = Argv[I];
				Logger.Info("Logging to file " + LogFile);
			} else {
				std::cout << "AstralDB: No file provided after -l/--log-file\n";
				Logger.Error("No file provided after -l/--log-file");
				return -1;
			}
		} else if(Arg == "-m" || Arg == "--mmap") {
			std::cout << "AstralDB: In-memory mode enabled (not implemented)\n";
			// Feature not implemented, just print message
		} else if(Arg[0] != '-') {
			// Assume it's a query file
			std::ifstream QueryFile(Arg.c_str(), std::ios::binary);
			if(!QueryFile) {
				std::cout << "AstralDB: File " << Arg << " does not exist\n";
				return -1;
			}
			std::string QueryTemp((std::istreambuf_iterator<char>(QueryFile)), std::istreambuf_iterator<char>());
			if(QueryTemp.empty()) {
				std::cout << "AstralDB: File " << Arg << " is empty\n";
				return -1;
			}
			AstralDB::SQL::Parser Parser(QueryTemp);
			Parser.DumpAST();
			return 0;
		}
	}
	return 0;
}
