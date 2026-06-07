#pragma once

#include <cstddef>
#include <cstdint>

namespace AstralDB {
namespace MathSciAutogradMicrokernels {

/** Fused elementwise multiply then ReLU: \p Dst[i] = max(0, \p A[i] * \p B[i]). */
void FusedMulReluF32(float *Dst, const float *A, const float *B, size_t N);

/** Fused chain rule multiply: \p Dst[i] = \p Local[i] * \p Upstream[i]. */
void FusedChainMulF32(float *Dst, const float *Local, const float *Upstream, size_t N);

/** Fused add then scale: \p Dst[i] = (\p A[i] + \p B[i]) * \p Scale. */
void FusedAddScaleF32(float *Dst, const float *A, const float *B, float Scale, size_t N);

/** Fused multiply-add: \p Dst[i] = \p A[i] * \p B[i] + \p C[i]. */
void FusedMulAddF32(float *Dst, const float *A, const float *B, const float *C, size_t N);

/** Fused multiply then sigmoid. */
void FusedMulSigmoidF32(float *Dst, const float *A, const float *B, size_t N);

/** Fused multiply then tanh. */
void FusedMulTanhF32(float *Dst, const float *A, const float *B, size_t N);

/** Fused ReLU then scale: \p Dst[i] = \p Scale * max(0, \p A[i]). */
void FusedReluScaleF32(float *Dst, const float *A, float Scale, size_t N);

/** Fused multiply, ReLU, scale: \p Dst[i] = \p Scale * max(0, \p A[i] * \p B[i]). */
void FusedMulReluScaleF32(float *Dst, const float *A, const float *B, float Scale, size_t N);

/** Pack ReLU gate bits (\p Src[i] > 0) into \p OutBits (ceil(\p N/8) bytes). */
void PackReluMaskF32(const float *Src, std::uint8_t *OutBits, size_t N);

/** Apply upstream grad through a packed ReLU mask. */
void ReluMaskBackwardF32(float *GradIn, const std::uint8_t *MaskBits, const float *Upstream, size_t N);

/** Backward for fused Mul+ReLU: \p GradMul[i] = \p Upstream[i] * (\p PreAct[i] > 0). */
void FusedMulReluBackwardMulF32(float *GradMul, const float *PreAct, const float *Upstream, size_t N);

/** Backward lhs for fused Mul+ReLU given \p GradMul. */
void FusedMulReluBackwardLhsF32(float *GradLhs, const float *Rhs, const float *GradMul, size_t N);

/** Backward rhs for fused Mul+ReLU given \p GradMul. */
void FusedMulReluBackwardRhsF32(float *GradRhs, const float *Lhs, const float *GradMul, size_t N);

} // namespace MathSciAutogradMicrokernels
} // namespace AstralDB
