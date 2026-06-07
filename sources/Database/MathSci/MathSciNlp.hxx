#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {
namespace MathSciNlp {

std::vector<std::string> Tokenize(std::string_view Text);
std::vector<std::string> NgramsFromText(std::string_view Text, int N);
std::vector<std::string> NgramsFromTokens(const std::vector<std::string> &Tokens, int N);
double JaccardSimilarity(std::string_view A, std::string_view B);
double JaccardTokenLists(const std::vector<std::string> &A, const std::vector<std::string> &B);
std::size_t EditDistance(std::string_view A, std::string_view B);
std::string Stem(std::string_view Word);

std::optional<std::string> TokenizeCellFromReal(std::string_view Text);
std::optional<std::string> NgramsCellFromReal(const std::string &TextOrList, double N);
std::optional<std::string> JaccardFromReal(const std::string &A, const std::string &B);
std::optional<std::string> EditDistFromReal(std::string_view A, std::string_view B);
std::optional<std::string> StemCellFromReal(std::string_view Word);

} // namespace MathSciNlp
} // namespace AstralDB
