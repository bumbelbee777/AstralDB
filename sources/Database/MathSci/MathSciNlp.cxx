#include <Database/MathSci/MathSciNlp.hxx>

#include <Database/Types/AdvancedTypes.hxx>
#include <Database/MathSci/MathSciEmbeddings.hxx>
#include <Database/Index/TextSearch.hxx>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace AstralDB {
namespace MathSciNlp {

std::vector<std::string> Tokenize(std::string_view Text) { return TextSearch::TokenizeQuery(Text); }

std::vector<std::string> NgramsFromTokens(const std::vector<std::string> &Tokens, int N) {
	std::vector<std::string> Out;
	if(N <= 0 || Tokens.empty())
		return Out;
	if(static_cast<std::size_t>(N) > Tokens.size())
		N = static_cast<int>(Tokens.size());
	for(std::size_t I = 0; I + static_cast<std::size_t>(N) <= Tokens.size(); ++I) {
		std::ostringstream O;
		for(int J = 0; J < N; ++J) {
			if(J)
				O << ' ';
			O << Tokens[I + static_cast<std::size_t>(J)];
		}
		Out.push_back(std::move(O).str());
	}
	return Out;
}

std::vector<std::string> NgramsFromText(std::string_view Text, int N) {
	return NgramsFromTokens(Tokenize(Text), N);
}

double JaccardTokenLists(const std::vector<std::string> &A, const std::vector<std::string> &B) {
	std::unordered_set<std::string> Sa(A.begin(), A.end());
	std::unordered_set<std::string> Sb(B.begin(), B.end());
	if(Sa.empty() && Sb.empty())
		return 1.0;
	std::size_t Inter = 0;
	for(const auto &T : Sa) {
		if(Sb.contains(T))
			++Inter;
	}
	const std::size_t Union = Sa.size() + Sb.size() - Inter;
	return Union == 0 ? 0.0 : static_cast<double>(Inter) / static_cast<double>(Union);
}

double JaccardSimilarity(std::string_view A, std::string_view B) {
	if(A.size() >= 2 && A[0] == 'L' && A[1] == '[') {
		const auto La = AdvancedTypes::ParseListCell(A);
		const auto Lb = AdvancedTypes::ParseListCell(B);
		if(La && Lb)
			return JaccardTokenLists(*La, *Lb);
	}
	return JaccardTokenLists(Tokenize(A), Tokenize(B));
}

std::size_t EditDistance(std::string_view A, std::string_view B) {
	if(A.size() > MathSciEmbeddings::MaxEditDistLen || B.size() > MathSciEmbeddings::MaxEditDistLen)
		return MathSciEmbeddings::MaxEditDistLen;
	const std::size_t Na = A.size();
	const std::size_t Nb = B.size();
	std::vector<std::size_t> Prev(Nb + 1), Cur(Nb + 1);
	for(std::size_t J = 0; J <= Nb; ++J)
		Prev[J] = J;
	for(std::size_t I = 1; I <= Na; ++I) {
		Cur[0] = I;
		for(std::size_t J = 1; J <= Nb; ++J) {
			const std::size_t Cost = (A[I - 1] == B[J - 1]) ? 0 : 1;
			Cur[J] = std::min({Prev[J] + 1, Cur[J - 1] + 1, Prev[J - 1] + Cost});
		}
		Prev.swap(Cur);
	}
	return Prev[Nb];
}

std::string Stem(std::string_view Word) {
	std::string Out(Word);
	while(Out.size() > 4) {
		if(Out.ends_with("ing") && Out.size() > 5) {
			Out.resize(Out.size() - 3);
			continue;
		}
		if(Out.ends_with("ed") && Out.size() > 4) {
			Out.resize(Out.size() - 2);
			continue;
		}
		if(Out.ends_with("ly") && Out.size() > 4) {
			Out.resize(Out.size() - 2);
			continue;
		}
		if(Out.ends_with("es") && Out.size() > 4) {
			Out.resize(Out.size() - 2);
			continue;
		}
		if(Out.ends_with("s") && !Out.ends_with("ss") && Out.size() > 3) {
			Out.pop_back();
			continue;
		}
		break;
	}
	return Out;
}

std::optional<std::string> TokenizeCellFromReal(std::string_view Text) {
	const auto Tokens = Tokenize(Text);
	return AdvancedTypes::FormatListCell(Tokens);
}

std::optional<std::string> NgramsCellFromReal(const std::string &TextOrList, double N) {
	if(N < 1)
		return std::nullopt;
	const int Ni = static_cast<int>(N);
	std::vector<std::string> Grams;
	if(TextOrList.size() >= 2 && TextOrList[0] == 'L' && TextOrList[1] == '[') {
		const auto L = AdvancedTypes::ParseListCell(TextOrList);
		if(!L)
			return std::nullopt;
		Grams = NgramsFromTokens(*L, Ni);
	} else
		Grams = NgramsFromText(TextOrList, Ni);
	return AdvancedTypes::FormatListCell(Grams);
}

std::optional<std::string> JaccardFromReal(const std::string &A, const std::string &B) {
	std::ostringstream O;
	O << JaccardSimilarity(A, B);
	return std::move(O).str();
}

std::optional<std::string> EditDistFromReal(std::string_view A, std::string_view B) {
	return std::to_string(EditDistance(A, B));
}

std::optional<std::string> StemCellFromReal(std::string_view Word) { return Stem(Word); }

} // namespace MathSciNlp
} // namespace AstralDB
