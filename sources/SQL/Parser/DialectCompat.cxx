#include <SQL/Parser/DialectCompat.hxx>

#include <cctype>

namespace AstralDB::SQL {

void FoldAsciiUpper(std::string &S) {
	for(char &C : S)
		C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
}

} // namespace AstralDB::SQL
