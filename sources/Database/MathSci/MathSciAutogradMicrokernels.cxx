#include <Database/MathSci/MathSciAutogradMicrokernels.hxx>

#include <IO/SIMD.hxx>

#include <cmath>
#include <cstdint>

namespace AstralDB {
namespace MathSciAutogradMicrokernels {

void FusedMulReluF32(float *Dst, const float *A, const float *B, size_t N) {
	Simd::MulF32(Dst, A, B, N);
	Simd::ReluF32(Dst, Dst, N);
}

void FusedChainMulF32(float *Dst, const float *Local, const float *Upstream, size_t N) {
	Simd::MulF32(Dst, Local, Upstream, N);
}

void FusedAddScaleF32(float *Dst, const float *A, const float *B, float Scale, size_t N) {
	Simd::AddF32(Dst, A, B, N);
	Simd::ScaleF32(Dst, Dst, Scale, N);
}

void FusedMulAddF32(float *Dst, const float *A, const float *B, const float *C, size_t N) {
	Simd::MulF32(Dst, A, B, N);
	Simd::AddF32(Dst, Dst, C, N);
}

void FusedMulSigmoidF32(float *Dst, const float *A, const float *B, size_t N) {
	for(size_t I = 0; I < N; ++I) {
		const float X = A[I] * B[I];
		Dst[I] = 1.f / (1.f + std::exp(-X));
	}
}

void FusedMulTanhF32(float *Dst, const float *A, const float *B, size_t N) {
	for(size_t I = 0; I < N; ++I)
		Dst[I] = std::tanh(A[I] * B[I]);
}

void FusedReluScaleF32(float *Dst, const float *A, float Scale, size_t N) {
	Simd::ReluF32(Dst, A, N);
	Simd::ScaleF32(Dst, Dst, Scale, N);
}

void FusedMulReluScaleF32(float *Dst, const float *A, const float *B, float Scale, size_t N) {
	FusedMulReluF32(Dst, A, B, N);
	Simd::ScaleF32(Dst, Dst, Scale, N);
}

void PackReluMaskF32(const float *Src, std::uint8_t *OutBits, size_t N) {
	const size_t Bytes = (N + 7) / 8;
	for(size_t B = 0; B < Bytes; ++B)
		OutBits[B] = 0;
	for(size_t I = 0; I < N; ++I) {
		if(Src[I] > 0.f)
			OutBits[I >> 3] |= static_cast<std::uint8_t>(1u << (I & 7u));
	}
}

void ReluMaskBackwardF32(float *GradIn, const std::uint8_t *MaskBits, const float *Upstream, size_t N) {
	for(size_t I = 0; I < N; ++I) {
		const bool On = (MaskBits[I >> 3] >> (I & 7u)) & 1u;
		GradIn[I] = On ? Upstream[I] : 0.f;
	}
}

void FusedMulReluBackwardMulF32(float *GradMul, const float *PreAct, const float *Upstream, size_t N) {
	for(size_t I = 0; I < N; ++I)
		GradMul[I] = PreAct[I] > 0.f ? Upstream[I] : 0.f;
}

void FusedMulReluBackwardLhsF32(float *GradLhs, const float *Rhs, const float *GradMul, size_t N) {
	Simd::MulF32(GradLhs, Rhs, GradMul, N);
}

void FusedMulReluBackwardRhsF32(float *GradRhs, const float *Lhs, const float *GradMul, size_t N) {
	Simd::MulF32(GradRhs, Lhs, GradMul, N);
}

} // namespace MathSciAutogradMicrokernels
} // namespace AstralDB
