#include <Database/Dialect/DualTable.hxx>

#include <cctype>
#include <string>

namespace AstralDB {
namespace {

void FoldAsciiUpper(std::string &S) {
	for(char &C : S)
		C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
}

} // namespace

bool IsDialectDualTable(std::string_view Name) {
	std::string U(Name);
	FoldAsciiUpper(U);
	std::string Internal(kDialectDualInternal);
	FoldAsciiUpper(Internal);
	return U == "DUAL" || U == Internal;
}

} // namespace AstralDB
