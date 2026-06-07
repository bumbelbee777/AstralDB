#pragma once

#include <cstddef>
#include <vector>

namespace AstralDB {
namespace MathSciSignal {

/** In-place radix-2 Cooley–Tukey FFT on \p Re/\p Im (length must be a power of two, >= 2). */
void FftInPlaceF32(float *Re, float *Im, size_t N);

/** Inverse FFT; divides by \p N. */
void IfftInPlaceF32(float *Re, float *Im, size_t N);

/** DCT-II and inverse DCT-II (orthonormal scaling). */
void Dct2F32(const float *In, float *Out, size_t N);
void Idct2F32(const float *In, float *Out, size_t N);

/** Full linear 1-D convolution (length \p Na + \p Nb - 1). */
std::vector<float> Conv1dFullF32(const float *A, size_t Na, const float *B, size_t Nb);

/** Same-length 1-D convolution (first operand length). */
std::vector<float> Conv1dSameF32(const float *A, size_t Na, const float *B, size_t Nb);

/** Discrete 1-D Laplacian stencil [1, -2, 1] with zero edge padding. */
std::vector<float> Laplacian1dF32(const float *In, size_t N);

/** Elementwise autograd: ∂L/∂x for y = x + b (upstream same shape as x). */
std::vector<float> AdGradAddF32(const float *Upstream, size_t N);

/** ∂L/∂a for z = a ⊙ b. */
std::vector<float> AdGradMulLhsF32(const float *A, const float *B, const float *Upstream, size_t N);

/** ∂L/∂b for z = a ⊙ b. */
std::vector<float> AdGradMulRhsF32(const float *A, const float *B, const float *Upstream, size_t N);

/** ∂L/∂x for y = max(0, x). */
std::vector<float> AdGradReluF32(const float *X, const float *Upstream, size_t N);

/** ∂L/∂x for y = σ(x) given upstream ∂L/∂y (y may be passed instead of x for stability). */
std::vector<float> AdGradSigmoidF32(const float *Y, const float *Upstream, size_t N);

/** ∂L/∂x for y = tanh(x) given tanh output \p Y and upstream ∂L/∂y. */
std::vector<float> AdGradTanhF32(const float *Y, const float *Upstream, size_t N);

/** ∂L/∂x for y = W x; \p Upstream is ∂L/∂y with length \p OutDim. */
std::vector<float> AdGradMatVecInputF32(const float *W, size_t OutDim, size_t InDim, const float *Upstream);

/** ∂L/∂W flattened row-major for y = W x. */
std::vector<float> AdGradMatVecWeightF32(const float *X, size_t InDim, const float *Upstream, size_t OutDim);

/** ∂L/∂pred for MSE(pred, target) with \p N elements. */
std::vector<float> AdGradMsePredF32(const float *Pred, const float *Target, size_t N);

/** ∂L/∂input for valid conv1d; \p Upstream matches full conv output length. */
std::vector<float> AdGradConv1dInputF32(const float *Input, size_t NIn, const float *Kernel, size_t Nk,
                                        const float *Upstream, size_t NUp);

/** ∂L/∂kernel. */
std::vector<float> AdGradConv1dKernelF32(const float *Input, size_t NIn, const float *Kernel, size_t Nk,
                                         const float *Upstream, size_t NUp);

/** Chain rule: upstream ⊙ local_jacobian (elementwise). */
std::vector<float> AdChainF32(const float *LocalGrad, const float *Upstream, size_t N);

/** Pad \p In to the next power of two; imaginary part zeroed. */
std::vector<float> FftInterleavedReIm(const std::vector<double> &In);

/** Real part after IFFT from interleaved [re, im, …] (length must be power-of-two complex slots). */
std::vector<double> IfftRealFromInterleaved(const std::vector<double> &Interleaved);

std::vector<double> Dct2FromReal(const std::vector<double> &In);
std::vector<double> Idct2FromReal(const std::vector<double> &In);
std::vector<double> Conv1dFullFromReal(const std::vector<double> &A, const std::vector<double> &B);
std::vector<double> Conv1dSameFromReal(const std::vector<double> &A, const std::vector<double> &B);
std::vector<double> Laplacian1dFromReal(const std::vector<double> &In);

} // namespace MathSciSignal
} // namespace AstralDB
