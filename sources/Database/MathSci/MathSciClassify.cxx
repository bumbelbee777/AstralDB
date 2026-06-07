#include <Database/MathSci/MathSciClassify.hxx>

#include <Database/Types/AdvancedTypes.hxx>
#include <Database/MathSci/MathSciComplex.hxx>

#include <cmath>
#include <sstream>
#include <vector>

namespace AstralDB {
namespace MathSciClassify {

double DotVecF32(const float *A, const float *B, std::size_t N) {
	return MathSciComplex::DotRealF32(A, B, N);
}

namespace {

MathSciComplex::NumericVec PromoteToComplex(const MathSciComplex::NumericVec &V) {
	if(V.IsComplex())
		return V;
	MathSciComplex::NumericVec Out;
	Out.Kind = MathSciComplex::NumericKind::Complex;
	Out.Values.reserve(V.Dim() * 2);
	for(float X : V.Values) {
		Out.Values.push_back(X);
		Out.Values.push_back(0.f);
	}
	return Out;
}

std::optional<double> FeatureScore(const MathSciComplex::NumericVec &Weights, const MathSciComplex::NumericVec &Features) {
	if(Weights.IsComplex() || Features.IsComplex()) {
		const auto W = PromoteToComplex(Weights);
		const auto X = PromoteToComplex(Features);
		if(W.Dim() != X.Dim())
			return std::nullopt;
		const auto Dot = MathSciComplex::DotComplex(W, X);
		return Dot ? std::optional<double>(static_cast<double>(Dot->first)) : std::nullopt;
	}
	if(Weights.Dim() != Features.Dim())
		return std::nullopt;
	const auto Dot = MathSciComplex::DotReal(Weights, Features);
	return Dot ? std::optional<double>(static_cast<double>(*Dot)) : std::nullopt;
}

} // namespace

std::optional<std::string> LinearLabelFromReal(const std::vector<double> &Weights, const std::vector<double> &Features,
                                               double Bias, double Threshold) {
	const auto W = MathSciComplex::ParseNumericVec(AdvancedTypes::FormatVectorCell(Weights));
	const auto X = MathSciComplex::ParseNumericVec(AdvancedTypes::FormatVectorCell(Features));
	if(!W || !X)
		return std::nullopt;
	const auto Score = FeatureScore(*W, *X);
	if(!Score)
		return std::nullopt;
	return *Score + Bias >= Threshold ? std::optional<std::string>("1") : std::optional<std::string>("0");
}

std::optional<std::string> LogisticProbFromReal(const std::vector<double> &Weights, const std::vector<double> &Features) {
	const auto W = MathSciComplex::ParseNumericVec(AdvancedTypes::FormatVectorCell(Weights));
	const auto X = MathSciComplex::ParseNumericVec(AdvancedTypes::FormatVectorCell(Features));
	if(!W || !X)
		return std::nullopt;
	const auto Score = FeatureScore(*W, *X);
	if(!Score)
		return std::nullopt;
	const double P = 1.0 / (1.0 + std::exp(-(*Score)));
	std::ostringstream O;
	O.precision(12);
	O << P;
	return std::move(O).str();
}

std::optional<std::string> ArgmaxFromReal(const std::vector<double> &Values) {
	if(Values.empty())
		return std::nullopt;
	std::size_t Best = 0;
	for(std::size_t I = 1; I < Values.size(); ++I) {
		if(Values[I] > Values[Best])
			Best = I;
	}
	return std::to_string(Best);
}

std::optional<std::string> OneVsRestFromReal(const std::vector<double> &Features,
                                             const std::vector<std::vector<double>> &WeightRows) {
	if(Features.empty() || WeightRows.empty())
		return std::nullopt;
	const auto X = MathSciComplex::ParseNumericVec(AdvancedTypes::FormatVectorCell(Features));
	if(!X)
		return std::nullopt;
	double BestScore = -1e300;
	std::size_t Best = 0;
	for(std::size_t C = 0; C < WeightRows.size(); ++C) {
		const auto W = MathSciComplex::ParseNumericVec(AdvancedTypes::FormatVectorCell(WeightRows[C]));
		if(!W)
			return std::nullopt;
		const auto Score = FeatureScore(*W, *X);
		if(!Score)
			return std::nullopt;
		if(*Score > BestScore) {
			BestScore = *Score;
			Best = C;
		}
	}
	return std::to_string(Best);
}

} // namespace MathSciClassify
} // namespace AstralDB
