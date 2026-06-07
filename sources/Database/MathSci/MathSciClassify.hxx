#pragma once

#include <optional>
#include <string>
#include <vector>

namespace AstralDB {
namespace MathSciClassify {

double DotVecF32(const float *A, const float *B, std::size_t N);

std::optional<std::string> LinearLabelFromReal(const std::vector<double> &Weights, const std::vector<double> &Features,
                                               double Bias, double Threshold = 0.0);
std::optional<std::string> LogisticProbFromReal(const std::vector<double> &Weights, const std::vector<double> &Features);
std::optional<std::string> ArgmaxFromReal(const std::vector<double> &Values);
std::optional<std::string> OneVsRestFromReal(const std::vector<double> &Features,
                                             const std::vector<std::vector<double>> &WeightRows);

} // namespace MathSciClassify
} // namespace AstralDB
