#include <SQL/SQL.hxx>
#include <iostream>
#include <fstream>

int main(int Argc, char **Argv) {
	if(Argc == 1 || Argv == nullptr) {
		std::cout << "AstralDB: No query file(s) provided. Use -h or --help for usage.\n";
		return -1;
	}
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
			std::cout << "AstralDB version 1.0.0\n";
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
				// TODO: Compile and run
				return 0;
			} else {
				std::cout << "AstralDB: No file provided after -s\n";
				return -1;
			}
		} else if(Arg == "-r" || Arg == "--repl") {
			std::cout << "AstralDB REPL mode (not implemented yet)\n";
			return 0;
		} else if(Arg == "-fb" || Arg == "--from-bytecode") {
			std::cout << "AstralDB: Bytecode execution not implemented yet\n";
			return 0;
		} else if(Arg == "-cc" || Arg == "--compile") {
			std::cout << "AstralDB: Query compilation not implemented yet\n";
			return 0;
		} else if(Arg == "-V" || Arg == "--verbose") {
			std::cout << "AstralDB: Verbose mode enabled\n";
			// Set verbose flag (not implemented)
		} else if(Arg == "-l" || Arg == "--log-file") {
			if(I + 1 < Argc) {
				std::cout << "AstralDB: Logging to file " << Argv[++I] << " (not implemented)\n";
			} else {
				std::cout << "AstralDB: No file provided after -l/--log-file\n";
				return -1;
			}
		} else if(Arg == "-m" || Arg == "--mmap") {
			std::cout << "AstralDB: In-memory mode enabled (not implemented)\n";
			// Set in-memory flag (not implemented)
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
