#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace AstralDB {
namespace MathSciAutograd {

constexpr size_t MaxAutogradLen = 1u << 20;

/** Interleaved complex buffer [re0, im0, re1, im1, …], length \p N complex slots. */
struct ComplexSeq {
	std::vector<float> Interleaved;
	size_t Slots() const { return Interleaved.size() / 2; }
};

std::optional<ComplexSeq> ParseComplexSeq(std::string_view Cell);
std::vector<double> FormatComplexSeqInterleaved(const ComplexSeq &C);

/** Diagonal Hessian contribution: second_deriv ⊙ upstream (elementwise). */
std::vector<float> AdHessianDiagF32(const float *SecondDeriv, const float *Upstream, size_t N);

std::vector<float> AdHessianReluDiagF32(const float *X, const float *Upstream, size_t N);
std::vector<float> AdHessianSigmoidDiagF32(const float *Y, const float *Upstream, size_t N);
std::vector<float> AdHessianSquareDiagF32(const float *X, const float *Upstream, size_t N);
std::vector<float> AdHessianTanhDiagF32(const float *Y, const float *Upstream, size_t N);

/** Wirtinger ∂L/∂z and ∂L/∂z̄ from Cartesian (∂L/∂x, ∂L/∂y) interleaved upstream. */
ComplexSeq WirtingerDzFromCartesianF32(const ComplexSeq &CartesianGrad);
ComplexSeq WirtingerDzBarFromCartesianF32(const ComplexSeq &CartesianGrad);

/** Complex elementwise multiply: w = a ⊙ b; upstream is ∂L/∂w (interleaved). */
ComplexSeq AdWirtingerMulLhsF32(const ComplexSeq &A, const ComplexSeq &B, const ComplexSeq &Upstream);
ComplexSeq AdWirtingerMulRhsF32(const ComplexSeq &A, const ComplexSeq &B, const ComplexSeq &Upstream);

/** For f = |z|² = z·z̄: Wirtinger grads w.r.t. z (returns ∂L/∂z as interleaved). */
ComplexSeq AdWirtingerAbs2F32(const ComplexSeq &Z, const ComplexSeq &Upstream);

/** Elementwise Wirtinger chain: local_jacobian ⊙ upstream (complex). */
ComplexSeq AdWirtingerChainF32(const ComplexSeq &LocalJac, const ComplexSeq &Upstream);

std::vector<double> AdHessianDiagFromReal(const std::vector<double> &SecondDeriv, const std::vector<double> &Upstream);
std::vector<double> AdHessianReluFromReal(const std::vector<double> &X, const std::vector<double> &Upstream);
std::vector<double> AdHessianSigmoidFromReal(const std::vector<double> &Y, const std::vector<double> &Upstream);
std::vector<double> AdHessianSquareFromReal(const std::vector<double> &X, const std::vector<double> &Upstream);
std::vector<double> AdHessianTanhFromReal(const std::vector<double> &Y, const std::vector<double> &Upstream);

std::optional<std::vector<double>> AdWirtingerMulLhsFromCells(std::string_view A, std::string_view B,
                                                              std::string_view Upstream);
std::optional<std::vector<double>> AdWirtingerMulRhsFromCells(std::string_view A, std::string_view B,
                                                              std::string_view Upstream);
std::optional<std::vector<double>> AdWirtingerAbs2FromCells(std::string_view Z, std::string_view Upstream);
std::optional<std::vector<double>> AdWirtingerChainFromCells(std::string_view Local, std::string_view Upstream);
std::optional<std::vector<double>> WirtingerDzFromCell(std::string_view CartesianGrad);
std::optional<std::vector<double>> WirtingerDzBarFromCell(std::string_view CartesianGrad);

} // namespace MathSciAutograd
} // namespace AstralDB
