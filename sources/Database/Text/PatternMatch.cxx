#include <Database/Text/PatternMatch.hxx>

#include <DS/Regex.hxx>
#include <cctype>
#include <vector>

namespace AstralDB {
namespace {

void FoldAsciiUpper(std::string &S) {
	for(char &C : S)
		C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
}

} // namespace

bool SqlLikeAscii(const std::string &Str, const std::string &Pat) {
	const size_t n = Str.size(), m = Pat.size();
	std::vector<std::vector<char>> Dp(m + 1, std::vector<char>(n + 1, 0));
	Dp[0][0] = 1;
	for(size_t I = 1; I <= m; ++I) {
		const char Pc = Pat[I - 1];
		for(size_t J = 0; J <= n; ++J) {
			if(Pc == '%')
				Dp[I][J] = Dp[I - 1][J] || (J > 0 && Dp[I][J - 1]);
			else if(Pc == '_')
				Dp[I][J] = J > 0 && Dp[I - 1][J - 1];
			else
				Dp[I][J] = J > 0 && Dp[I - 1][J - 1] && Str[J - 1] == Pc;
		}
	}
	return Dp[m][n];
}

bool SqlLikeAsciiCaseInsensitive(const std::string &Str, const std::string &Pat) {
	std::string Fs = Str;
	std::string Fp = Pat;
	FoldAsciiUpper(Fs);
	FoldAsciiUpper(Fp);
	return SqlLikeAscii(Fs, Fp);
}

bool SqlGlobMatch(const std::string &Str, const std::string &Pat) {
	const size_t n = Str.size(), m = Pat.size();
	std::vector<std::vector<char>> Dp(m + 1, std::vector<char>(n + 1, 0));
	Dp[0][0] = 1;
	for(size_t I = 1; I <= m; ++I) {
		const char Pc = Pat[I - 1];
		for(size_t J = 0; J <= n; ++J) {
			if(Pc == '*')
				Dp[I][J] = Dp[I - 1][J] || (J > 0 && Dp[I][J - 1]);
			else if(Pc == '?')
				Dp[I][J] = J > 0 && Dp[I - 1][J - 1];
			else
				Dp[I][J] = J > 0 && Dp[I - 1][J - 1] && Str[J - 1] == Pc;
		}
	}
	return Dp[m][n];
}

bool ColumnNameGlobMatch(std::string_view Name, std::string_view Pat) {
	return SqlGlobMatch(std::string(Name), std::string(Pat));
}

bool SqlRegexpMatch(const std::string &Str, const std::string &Pat, bool CaseInsensitive) {
	DS::Regex::Flag Flags = DS::Regex::Flag::None;
	if(CaseInsensitive)
		Flags = Flags | DS::Regex::Flag::CaseInsensitive;
	return DS::Regex::SqlMatch(Str, Pat, Flags);
}

std::optional<std::string> SqlRegexpExtract(const std::string &Str, const std::string &Pat, int GroupIndex,
                                            bool CaseInsensitive) {
	DS::Regex::Flag Flags = DS::Regex::Flag::None;
	if(CaseInsensitive)
		Flags = Flags | DS::Regex::Flag::CaseInsensitive;
	return DS::Regex::SqlExtract(Str, Pat, GroupIndex, Flags);
}

} // namespace AstralDB
