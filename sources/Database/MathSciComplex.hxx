#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AstralDB {
namespace MathSciComplex {

constexpr std::size_t MaxNumericLen = 1u << 20;

enum class NumericKind : std::uint8_t { Real = 0, Complex = 1 };

/** Real: \p Values.size() == dim. Complex: interleaved re,im with \p Slots() == dim. */
struct NumericVec {
	NumericKind Kind = NumericKind::Real;
	std::vector<float> Values;

	bool IsComplex() const { return Kind == NumericKind::Complex; }
	std::size_t Dim() const { return IsComplex() ? Values.size() / 2 : Values.size(); }
	std::size_t FloatCount() const { return Values.size(); }
};

struct NumericMat {
	NumericKind Kind = NumericKind::Real;
	std::size_t Rows = 0;
	std::size_t Cols = 0;
	std::vector<float> Flat;
};

std::optional<NumericVec> ParseNumericVec(std::string_view Cell);
std::optional<NumericMat> ParseNumericMat(std::string_view Cell);
std::string FormatNumericVec(const NumericVec &V);
std::string FormatComplexScalar(float Re, float Im);

float DotRealF32(const float *A, const float *B, std::size_t N);
void DotComplexHermitianF32(const float *A, const float *B, std::size_t Slots, float &OutRe, float &OutIm);
float NormRealF32(const float *A, std::size_t N);
void NormComplexF32(const float *A, std::size_t Slots, float &OutRe, float &OutIm);

NumericVec Add(const NumericVec &A, const NumericVec &B);
std::optional<float> DotReal(const NumericVec &A, const NumericVec &B);
std::optional<std::pair<float, float>> DotComplex(const NumericVec &A, const NumericVec &B);
std::optional<float> NormReal(const NumericVec &A);
std::optional<std::pair<float, float>> NormComplex(const NumericVec &A);
std::optional<NumericVec> MatVec(const NumericMat &M, const NumericVec &V);
std::optional<double> CosineSim(const NumericVec &A, const NumericVec &B);
NumericVec MeanPool(const std::vector<NumericVec> &Rows);
NumericVec Scale(const NumericVec &V, float Scalar);
NumericVec ScaleComplex(const NumericVec &V, float ScaleRe, float ScaleIm);

std::optional<std::string> DotCellFromReal(const std::string &A, const std::string &B);
std::optional<std::string> AddCellFromReal(const std::string &A, const std::string &B);
std::optional<std::string> NormCellFromReal(const std::string &A);
std::optional<std::string> MatVecCellFromReal(const std::string &M, const std::string &V);
std::optional<std::string> CosineSimCellFromReal(const std::string &A, const std::string &B);

} // namespace MathSciComplex
} // namespace AstralDB
