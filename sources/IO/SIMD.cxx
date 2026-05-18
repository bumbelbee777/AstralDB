#include <IO/SIMD.hxx>

#include <cmath>

namespace AstralDB {
namespace Simd {
namespace {

#if !defined(__AVX2__) && !defined(__ARM_NEON) && !defined(__aarch64__)

void DotScalar(const float *A, const float *B, size_t Count, float &Acc) {
	for(size_t I = 0; I < Count; ++I)
		Acc += A[I] * B[I];
}

void AddScalar(float *Dst, const float *A, const float *B, size_t Count) {
	for(size_t I = 0; I < Count; ++I)
		Dst[I] = A[I] + B[I];
}

void MulAccumulateScalar(float *Dst, const float *A, float Scalar, size_t Count) {
	for(size_t I = 0; I < Count; ++I)
		Dst[I] += A[I] * Scalar;
}

void SubScalar(float *Dst, const float *A, const float *B, size_t Count) {
	for(size_t I = 0; I < Count; ++I)
		Dst[I] = A[I] - B[I];
}

void MulScalar(float *Dst, const float *A, const float *B, size_t Count) {
	for(size_t I = 0; I < Count; ++I)
		Dst[I] = A[I] * B[I];
}

void ScaleScalar(float *Dst, const float *A, float Scalar, size_t Count) {
	for(size_t I = 0; I < Count; ++I)
		Dst[I] = A[I] * Scalar;
}

void ComplexMulAccumulateScalar(const float *ARe, const float *AIm, const float *BRe, const float *BIm, float *OutRe,
                              float *OutIm, size_t Count) {
	for(size_t I = 0; I < Count; ++I) {
		const float Re = ARe[I] * BRe[I] - AIm[I] * BIm[I];
		const float Im = ARe[I] * BIm[I] + AIm[I] * BRe[I];
		OutRe[I] += Re;
		OutIm[I] += Im;
	}
}

#endif // scalar-only fallbacks

} // namespace

float DotProductF32(const float *A, const float *B, size_t Count) {
	float Acc = 0.f;
#if defined(__AVX2__)
	size_t I = 0;
	__m256 Sum = _mm256_setzero_ps();
	for(; I + 7 < Count; I += 8) {
		__m256 Va = _mm256_loadu_ps(A + I);
		__m256 Vb = _mm256_loadu_ps(B + I);
		Sum = _mm256_add_ps(Sum, _mm256_mul_ps(Va, Vb));
	}
	float Buf[8];
	_mm256_storeu_ps(Buf, Sum);
	for(int J = 0; J < 8; ++J)
		Acc += Buf[J];
	for(; I < Count; ++I)
		Acc += A[I] * B[I];
#elif defined(__ARM_NEON) || defined(__aarch64__)
	size_t I = 0;
	float32x4_t Sum = vdupq_n_f32(0.f);
	for(; I + 3 < Count; I += 4) {
		float32x4_t Va = vld1q_f32(A + I);
		float32x4_t Vb = vld1q_f32(B + I);
		Sum = vmlaq_f32(Sum, Va, Vb);
	}
	Acc = vgetq_lane_f32(Sum, 0) + vgetq_lane_f32(Sum, 1) + vgetq_lane_f32(Sum, 2) + vgetq_lane_f32(Sum, 3);
	for(; I < Count; ++I)
		Acc += A[I] * B[I];
#else
	DotScalar(A, B, Count, Acc);
#endif
	return Acc;
}

void SubF32(float *Dst, const float *A, const float *B, size_t Count) {
#if defined(__AVX2__)
	size_t I = 0;
	for(; I + 7 < Count; I += 8) {
		__m256 Va = _mm256_loadu_ps(A + I);
		__m256 Vb = _mm256_loadu_ps(B + I);
		_mm256_storeu_ps(Dst + I, _mm256_sub_ps(Va, Vb));
	}
	for(; I < Count; ++I)
		Dst[I] = A[I] - B[I];
#elif defined(__ARM_NEON) || defined(__aarch64__)
	size_t I = 0;
	for(; I + 3 < Count; I += 4) {
		float32x4_t Va = vld1q_f32(A + I);
		float32x4_t Vb = vld1q_f32(B + I);
		vst1q_f32(Dst + I, vsubq_f32(Va, Vb));
	}
	for(; I < Count; ++I)
		Dst[I] = A[I] - B[I];
#else
	SubScalar(Dst, A, B, Count);
#endif
}

void MulF32(float *Dst, const float *A, const float *B, size_t Count) {
#if defined(__AVX2__)
	size_t I = 0;
	for(; I + 7 < Count; I += 8) {
		__m256 Va = _mm256_loadu_ps(A + I);
		__m256 Vb = _mm256_loadu_ps(B + I);
		_mm256_storeu_ps(Dst + I, _mm256_mul_ps(Va, Vb));
	}
	for(; I < Count; ++I)
		Dst[I] = A[I] * B[I];
#elif defined(__ARM_NEON) || defined(__aarch64__)
	size_t I = 0;
	for(; I + 3 < Count; I += 4) {
		float32x4_t Va = vld1q_f32(A + I);
		float32x4_t Vb = vld1q_f32(B + I);
		vst1q_f32(Dst + I, vmulq_f32(Va, Vb));
	}
	for(; I < Count; ++I)
		Dst[I] = A[I] * B[I];
#else
	MulScalar(Dst, A, B, Count);
#endif
}

void ScaleF32(float *Dst, const float *A, float Scalar, size_t Count) {
#if defined(__AVX2__)
	__m256 S = _mm256_set1_ps(Scalar);
	size_t I = 0;
	for(; I + 7 < Count; I += 8) {
		__m256 Va = _mm256_loadu_ps(A + I);
		_mm256_storeu_ps(Dst + I, _mm256_mul_ps(Va, S));
	}
	for(; I < Count; ++I)
		Dst[I] = A[I] * Scalar;
#elif defined(__ARM_NEON) || defined(__aarch64__)
	float32x4_t S = vdupq_n_f32(Scalar);
	size_t I = 0;
	for(; I + 3 < Count; I += 4) {
		float32x4_t Va = vld1q_f32(A + I);
		vst1q_f32(Dst + I, vmulq_f32(Va, S));
	}
	for(; I < Count; ++I)
		Dst[I] = A[I] * Scalar;
#else
	ScaleScalar(Dst, A, Scalar, Count);
#endif
}

void AddF32(float *Dst, const float *A, const float *B, size_t Count) {
#if defined(__AVX2__)
	size_t I = 0;
	for(; I + 7 < Count; I += 8) {
		__m256 Va = _mm256_loadu_ps(A + I);
		__m256 Vb = _mm256_loadu_ps(B + I);
		_mm256_storeu_ps(Dst + I, _mm256_add_ps(Va, Vb));
	}
	for(; I < Count; ++I)
		Dst[I] = A[I] + B[I];
#elif defined(__ARM_NEON) || defined(__aarch64__)
	size_t I = 0;
	for(; I + 3 < Count; I += 4) {
		float32x4_t Va = vld1q_f32(A + I);
		float32x4_t Vb = vld1q_f32(B + I);
		vst1q_f32(Dst + I, vaddq_f32(Va, Vb));
	}
	for(; I < Count; ++I)
		Dst[I] = A[I] + B[I];
#else
	AddScalar(Dst, A, B, Count);
#endif
}

void MulAccumulateF32(float *Dst, const float *A, float Scalar, size_t Count) {
#if defined(__AVX2__)
	__m256 S = _mm256_set1_ps(Scalar);
	size_t I = 0;
	for(; I + 7 < Count; I += 8) {
		__m256 Va = _mm256_loadu_ps(A + I);
		__m256 Vd = _mm256_loadu_ps(Dst + I);
		_mm256_storeu_ps(Dst + I, _mm256_add_ps(Vd, _mm256_mul_ps(Va, S)));
	}
	for(; I < Count; ++I)
		Dst[I] += A[I] * Scalar;
#elif defined(__ARM_NEON) || defined(__aarch64__)
	float32x4_t S = vdupq_n_f32(Scalar);
	size_t I = 0;
	for(; I + 3 < Count; I += 4) {
		float32x4_t Va = vld1q_f32(A + I);
		float32x4_t Vd = vld1q_f32(Dst + I);
		vst1q_f32(Dst + I, vmlaq_f32(Vd, Va, S));
	}
	for(; I < Count; ++I)
		Dst[I] += A[I] * Scalar;
#else
	MulAccumulateScalar(Dst, A, Scalar, Count);
#endif
}

void ComplexMulF32(const float *ARe, const float *AIm, const float *BRe, const float *BIm, float *OutRe, float *OutIm) {
	const float Re = ARe[0] * BRe[0] - AIm[0] * BIm[0];
	const float Im = ARe[0] * BIm[0] + AIm[0] * BRe[0];
	OutRe[0] = Re;
	OutIm[0] = Im;
}

void ComplexMulAccumulateF32(const float *ARe, const float *AIm, const float *BRe, const float *BIm, float *OutRe,
                             float *OutIm, size_t Count) {
#if defined(__AVX2__)
	size_t I = 0;
	for(; I + 7 < Count; I += 8) {
		__m256 Ar = _mm256_loadu_ps(ARe + I);
		__m256 Ai = _mm256_loadu_ps(AIm + I);
		__m256 Br = _mm256_loadu_ps(BRe + I);
		__m256 Bi = _mm256_loadu_ps(BIm + I);
		__m256 Or = _mm256_loadu_ps(OutRe + I);
		__m256 Oi = _mm256_loadu_ps(OutIm + I);
		Or = _mm256_add_ps(Or, _mm256_sub_ps(_mm256_mul_ps(Ar, Br), _mm256_mul_ps(Ai, Bi)));
		Oi = _mm256_add_ps(Oi, _mm256_add_ps(_mm256_mul_ps(Ar, Bi), _mm256_mul_ps(Ai, Br)));
		_mm256_storeu_ps(OutRe + I, Or);
		_mm256_storeu_ps(OutIm + I, Oi);
	}
	for(; I < Count; ++I) {
		const float Re = ARe[I] * BRe[I] - AIm[I] * BIm[I];
		const float Im = ARe[I] * BIm[I] + AIm[I] * BRe[I];
		OutRe[I] += Re;
		OutIm[I] += Im;
	}
#elif defined(__ARM_NEON) || defined(__aarch64__)
	size_t I = 0;
	for(; I + 3 < Count; I += 4) {
		float32x4_t Ar = vld1q_f32(ARe + I);
		float32x4_t Ai = vld1q_f32(AIm + I);
		float32x4_t Br = vld1q_f32(BRe + I);
		float32x4_t Bi = vld1q_f32(BIm + I);
		float32x4_t Or = vld1q_f32(OutRe + I);
		float32x4_t Oi = vld1q_f32(OutIm + I);
		Or = vaddq_f32(Or, vsubq_f32(vmulq_f32(Ar, Br), vmulq_f32(Ai, Bi)));
		Oi = vaddq_f32(Oi, vaddq_f32(vmulq_f32(Ar, Bi), vmulq_f32(Ai, Br)));
		vst1q_f32(OutRe + I, Or);
		vst1q_f32(OutIm + I, Oi);
	}
	for(; I < Count; ++I) {
		const float Re = ARe[I] * BRe[I] - AIm[I] * BIm[I];
		const float Im = ARe[I] * BIm[I] + AIm[I] * BRe[I];
		OutRe[I] += Re;
		OutIm[I] += Im;
	}
#else
	ComplexMulAccumulateScalar(ARe, AIm, BRe, BIm, OutRe, OutIm, Count);
#endif
}

void MatrixVectorMulF32(const float *MatrixRowMajor, const float *Vector, float *Out, size_t Rows, size_t Cols) {
	for(size_t R = 0; R < Rows; ++R) {
		const float *Row = MatrixRowMajor + R * Cols;
		Out[R] = DotProductF32(Row, Vector, Cols);
	}
}

} // namespace Simd
} // namespace AstralDB
