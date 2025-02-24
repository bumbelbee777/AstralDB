#include <SQL/SQL.hxx>
#include <iostream>
#include <fstream>

int main(int argc, char **argv) {
    if(argc < 1 || argv == nullptr)
        std::cout << "AstralDB: No query file(s) provided\n";
    else {
        for(int i = 1; i < argc; ++i) {
            std::ifstream QueryFile(argv[i], std::ios::binary);
            if(!QueryFile) std::cout << "AstralDB: File " << argv[i] << " does not exist\n";
            else {
                std::string QueryTemp((std::istreambuf_iterator<char>(QueryFile)),
                                  std::istreambuf_iterator<char>());
                std::string_view Query(QueryTemp);
                if(Query.empty()) std::cout << "AstralDB: File " << argv[i] << " is empty\n";
                AstralDB::SQL::Parser Parser(Query);
                return 0;
            }
        }
    }
    return -1;
}
