#include <IO/SIMD.hxx>

#include <algorithm>
#include <cstdlib>

namespace AstralDB {
namespace Simd {
namespace {

KernelConfig DefaultKernelConfig() {
#if defined(__AVX2__)
	return {8, 4, 4, 384};
#elif defined(__ARM_NEON) || defined(__aarch64__)
	return {4, 4, 4, 192};
#elif defined(__SSE2__)
	return {4, 4, 4, 160};
#else
	return {1, 1, 2, 96};
#endif
}

size_t ParseEnvSizeT(const char *Name, size_t Fallback) {
	auto ParseDigits = [Fallback](const char *Raw) -> size_t {
		if(!Raw || !*Raw)
			return Fallback;
		char *End = nullptr;
		const unsigned long long Parsed = std::strtoull(Raw, &End, 10);
		if(End == Raw)
			return Fallback;
		return static_cast<size_t>(Parsed);
	};
#if defined(_MSC_VER)
	char *Buf = nullptr;
	size_t Len = 0;
	if(_dupenv_s(&Buf, &Len, Name) != 0 || !Buf)
		return Fallback;
	const size_t Out = ParseDigits(Buf);
	free(Buf);
	return Out;
#else
	return ParseDigits(std::getenv(Name));
#endif
}

#if defined(__AVX2__)
using VecF32 = __m256;
static constexpr size_t SIMD_WIDTH_F32 = 8;

inline VecF32 VecZeroF32() { return _mm256_setzero_ps(); }
inline VecF32 VecLoadF32(const float *Ptr) { return _mm256_loadu_ps(Ptr); }
inline void VecStoreF32(float *Ptr, VecF32 V) { _mm256_storeu_ps(Ptr, V); }
inline VecF32 VecAddF32(VecF32 A, VecF32 B) { return _mm256_add_ps(A, B); }
inline VecF32 VecSubF32(VecF32 A, VecF32 B) { return _mm256_sub_ps(A, B); }
inline VecF32 VecMulF32(VecF32 A, VecF32 B) { return _mm256_mul_ps(A, B); }
inline VecF32 VecSet1F32(float S) { return _mm256_set1_ps(S); }
inline VecF32 VecFmaddF32(VecF32 A, VecF32 B, VecF32 C) {
#if defined(__FMA__)
	return _mm256_fmadd_ps(A, B, C);
#else
	return VecAddF32(VecMulF32(A, B), C);
#endif
}
inline float VecHorizontalAddF32(VecF32 V) {
	alignas(32) float Buf[8];
	VecStoreF32(Buf, V);
	return Buf[0] + Buf[1] + Buf[2] + Buf[3] + Buf[4] + Buf[5] + Buf[6] + Buf[7];
}
#elif defined(__SSE2__)
using VecF32 = __m128;
static constexpr size_t SIMD_WIDTH_F32 = 4;

inline VecF32 VecZeroF32() { return _mm_setzero_ps(); }
inline VecF32 VecLoadF32(const float *Ptr) { return _mm_loadu_ps(Ptr); }
inline void VecStoreF32(float *Ptr, VecF32 V) { _mm_storeu_ps(Ptr, V); }
inline VecF32 VecAddF32(VecF32 A, VecF32 B) { return _mm_add_ps(A, B); }
inline VecF32 VecSubF32(VecF32 A, VecF32 B) { return _mm_sub_ps(A, B); }
inline VecF32 VecMulF32(VecF32 A, VecF32 B) { return _mm_mul_ps(A, B); }
inline VecF32 VecSet1F32(float S) { return _mm_set1_ps(S); }
inline VecF32 VecFmaddF32(VecF32 A, VecF32 B, VecF32 C) { return VecAddF32(VecMulF32(A, B), C); }
inline float VecHorizontalAddF32(VecF32 V) {
	alignas(16) float Buf[4];
	VecStoreF32(Buf, V);
	return Buf[0] + Buf[1] + Buf[2] + Buf[3];
}
#elif defined(__ARM_NEON) || defined(__aarch64__)
using VecF32 = float32x4_t;
static constexpr size_t SIMD_WIDTH_F32 = 4;

inline VecF32 VecZeroF32() { return vdupq_n_f32(0.f); }
inline VecF32 VecLoadF32(const float *Ptr) { return vld1q_f32(Ptr); }
inline void VecStoreF32(float *Ptr, VecF32 V) { vst1q_f32(Ptr, V); }
inline VecF32 VecAddF32(VecF32 A, VecF32 B) { return vaddq_f32(A, B); }
inline VecF32 VecSubF32(VecF32 A, VecF32 B) { return vsubq_f32(A, B); }
inline VecF32 VecMulF32(VecF32 A, VecF32 B) { return vmulq_f32(A, B); }
inline VecF32 VecSet1F32(float S) { return vdupq_n_f32(S); }
inline VecF32 VecFmaddF32(VecF32 A, VecF32 B, VecF32 C) { return vmlaq_f32(C, A, B); }
inline float VecHorizontalAddF32(VecF32 V) {
	return vgetq_lane_f32(V, 0) + vgetq_lane_f32(V, 1) + vgetq_lane_f32(V, 2) + vgetq_lane_f32(V, 3);
}
#endif

[[maybe_unused]] void DotScalar(const float *A, const float *B, size_t Count, float &Acc) {
	for(size_t I = 0; I < Count; ++I)
		Acc += A[I] * B[I];
}

[[maybe_unused]] void AddScalar(float *Dst, const float *A, const float *B, size_t Count) {
	for(size_t I = 0; I < Count; ++I)
		Dst[I] = A[I] + B[I];
}

[[maybe_unused]] void MulAccumulateScalar(float *Dst, const float *A, float Scalar, size_t Count) {
	for(size_t I = 0; I < Count; ++I)
		Dst[I] += A[I] * Scalar;
}

[[maybe_unused]] void SubScalar(float *Dst, const float *A, const float *B, size_t Count) {
	for(size_t I = 0; I < Count; ++I)
		Dst[I] = A[I] - B[I];
}

[[maybe_unused]] void MulScalar(float *Dst, const float *A, const float *B, size_t Count) {
	for(size_t I = 0; I < Count; ++I)
		Dst[I] = A[I] * B[I];
}

[[maybe_unused]] void ScaleScalar(float *Dst, const float *A, float Scalar, size_t Count) {
	for(size_t I = 0; I < Count; ++I)
		Dst[I] = A[I] * Scalar;
}

#if defined(__AVX2__) || defined(__SSE2__) || defined(__ARM_NEON) || defined(__aarch64__)
void AddKernelF32(float *Dst, const float *A, const float *B, size_t Count) {
	const size_t UNROLL = 4;
	const size_t STEP = SIMD_WIDTH_F32 * UNROLL;
	size_t I = 0;
	for(; I + STEP <= Count; I += STEP) {
		VecStoreF32(Dst + I + 0 * SIMD_WIDTH_F32, VecAddF32(VecLoadF32(A + I + 0 * SIMD_WIDTH_F32),
		                                                     VecLoadF32(B + I + 0 * SIMD_WIDTH_F32)));
		VecStoreF32(Dst + I + 1 * SIMD_WIDTH_F32, VecAddF32(VecLoadF32(A + I + 1 * SIMD_WIDTH_F32),
		                                                     VecLoadF32(B + I + 1 * SIMD_WIDTH_F32)));
		VecStoreF32(Dst + I + 2 * SIMD_WIDTH_F32, VecAddF32(VecLoadF32(A + I + 2 * SIMD_WIDTH_F32),
		                                                     VecLoadF32(B + I + 2 * SIMD_WIDTH_F32)));
		VecStoreF32(Dst + I + 3 * SIMD_WIDTH_F32, VecAddF32(VecLoadF32(A + I + 3 * SIMD_WIDTH_F32),
		                                                     VecLoadF32(B + I + 3 * SIMD_WIDTH_F32)));
	}
	for(; I + SIMD_WIDTH_F32 <= Count; I += SIMD_WIDTH_F32)
		VecStoreF32(Dst + I, VecAddF32(VecLoadF32(A + I), VecLoadF32(B + I)));
	for(; I < Count; ++I)
		Dst[I] = A[I] + B[I];
}

void SubKernelF32(float *Dst, const float *A, const float *B, size_t Count) {
	const size_t UNROLL = 4;
	const size_t STEP = SIMD_WIDTH_F32 * UNROLL;
	size_t I = 0;
	for(; I + STEP <= Count; I += STEP) {
		VecStoreF32(Dst + I + 0 * SIMD_WIDTH_F32, VecSubF32(VecLoadF32(A + I + 0 * SIMD_WIDTH_F32),
		                                                     VecLoadF32(B + I + 0 * SIMD_WIDTH_F32)));
		VecStoreF32(Dst + I + 1 * SIMD_WIDTH_F32, VecSubF32(VecLoadF32(A + I + 1 * SIMD_WIDTH_F32),
		                                                     VecLoadF32(B + I + 1 * SIMD_WIDTH_F32)));
		VecStoreF32(Dst + I + 2 * SIMD_WIDTH_F32, VecSubF32(VecLoadF32(A + I + 2 * SIMD_WIDTH_F32),
		                                                     VecLoadF32(B + I + 2 * SIMD_WIDTH_F32)));
		VecStoreF32(Dst + I + 3 * SIMD_WIDTH_F32, VecSubF32(VecLoadF32(A + I + 3 * SIMD_WIDTH_F32),
		                                                     VecLoadF32(B + I + 3 * SIMD_WIDTH_F32)));
	}
	for(; I + SIMD_WIDTH_F32 <= Count; I += SIMD_WIDTH_F32)
		VecStoreF32(Dst + I, VecSubF32(VecLoadF32(A + I), VecLoadF32(B + I)));
	for(; I < Count; ++I)
		Dst[I] = A[I] - B[I];
}

void MulKernelF32(float *Dst, const float *A, const float *B, size_t Count) {
	const size_t UNROLL = 4;
	const size_t STEP = SIMD_WIDTH_F32 * UNROLL;
	size_t I = 0;
	for(; I + STEP <= Count; I += STEP) {
		VecStoreF32(Dst + I + 0 * SIMD_WIDTH_F32, VecMulF32(VecLoadF32(A + I + 0 * SIMD_WIDTH_F32),
		                                                     VecLoadF32(B + I + 0 * SIMD_WIDTH_F32)));
		VecStoreF32(Dst + I + 1 * SIMD_WIDTH_F32, VecMulF32(VecLoadF32(A + I + 1 * SIMD_WIDTH_F32),
		                                                     VecLoadF32(B + I + 1 * SIMD_WIDTH_F32)));
		VecStoreF32(Dst + I + 2 * SIMD_WIDTH_F32, VecMulF32(VecLoadF32(A + I + 2 * SIMD_WIDTH_F32),
		                                                     VecLoadF32(B + I + 2 * SIMD_WIDTH_F32)));
		VecStoreF32(Dst + I + 3 * SIMD_WIDTH_F32, VecMulF32(VecLoadF32(A + I + 3 * SIMD_WIDTH_F32),
		                                                     VecLoadF32(B + I + 3 * SIMD_WIDTH_F32)));
	}
	for(; I + SIMD_WIDTH_F32 <= Count; I += SIMD_WIDTH_F32)
		VecStoreF32(Dst + I, VecMulF32(VecLoadF32(A + I), VecLoadF32(B + I)));
	for(; I < Count; ++I)
		Dst[I] = A[I] * B[I];
}

void ScaleKernelF32(float *Dst, const float *A, float Scalar, size_t Count) {
	const size_t UNROLL = 4;
	const size_t STEP = SIMD_WIDTH_F32 * UNROLL;
	const VecF32 S = VecSet1F32(Scalar);
	size_t I = 0;
	for(; I + STEP <= Count; I += STEP) {
		VecStoreF32(Dst + I + 0 * SIMD_WIDTH_F32, VecMulF32(VecLoadF32(A + I + 0 * SIMD_WIDTH_F32), S));
		VecStoreF32(Dst + I + 1 * SIMD_WIDTH_F32, VecMulF32(VecLoadF32(A + I + 1 * SIMD_WIDTH_F32), S));
		VecStoreF32(Dst + I + 2 * SIMD_WIDTH_F32, VecMulF32(VecLoadF32(A + I + 2 * SIMD_WIDTH_F32), S));
		VecStoreF32(Dst + I + 3 * SIMD_WIDTH_F32, VecMulF32(VecLoadF32(A + I + 3 * SIMD_WIDTH_F32), S));
	}
	for(; I + SIMD_WIDTH_F32 <= Count; I += SIMD_WIDTH_F32)
		VecStoreF32(Dst + I, VecMulF32(VecLoadF32(A + I), S));
	for(; I < Count; ++I)
		Dst[I] = A[I] * Scalar;
}

void MulAccumulateKernelF32(float *Dst, const float *A, float Scalar, size_t Count) {
	const size_t UNROLL = 4;
	const size_t STEP = SIMD_WIDTH_F32 * UNROLL;
	const VecF32 S = VecSet1F32(Scalar);
	size_t I = 0;
	for(; I + STEP <= Count; I += STEP) {
		VecStoreF32(Dst + I + 0 * SIMD_WIDTH_F32, VecFmaddF32(VecLoadF32(A + I + 0 * SIMD_WIDTH_F32), S,
		                                                       VecLoadF32(Dst + I + 0 * SIMD_WIDTH_F32)));
		VecStoreF32(Dst + I + 1 * SIMD_WIDTH_F32, VecFmaddF32(VecLoadF32(A + I + 1 * SIMD_WIDTH_F32), S,
		                                                       VecLoadF32(Dst + I + 1 * SIMD_WIDTH_F32)));
		VecStoreF32(Dst + I + 2 * SIMD_WIDTH_F32, VecFmaddF32(VecLoadF32(A + I + 2 * SIMD_WIDTH_F32), S,
		                                                       VecLoadF32(Dst + I + 2 * SIMD_WIDTH_F32)));
		VecStoreF32(Dst + I + 3 * SIMD_WIDTH_F32, VecFmaddF32(VecLoadF32(A + I + 3 * SIMD_WIDTH_F32), S,
		                                                       VecLoadF32(Dst + I + 3 * SIMD_WIDTH_F32)));
	}
	for(; I + SIMD_WIDTH_F32 <= Count; I += SIMD_WIDTH_F32)
		VecStoreF32(Dst + I, VecFmaddF32(VecLoadF32(A + I), S, VecLoadF32(Dst + I)));
	for(; I < Count; ++I)
		Dst[I] += A[I] * Scalar;
}

float DotKernelF32(const float *A, const float *B, size_t Count) {
	const size_t UNROLL = 4;
	const size_t STEP = SIMD_WIDTH_F32 * UNROLL;
	size_t I = 0;
	VecF32 Sum0 = VecZeroF32();
	VecF32 Sum1 = VecZeroF32();
	VecF32 Sum2 = VecZeroF32();
	VecF32 Sum3 = VecZeroF32();
	for(; I + STEP <= Count; I += STEP) {
		Sum0 = VecFmaddF32(VecLoadF32(A + I + 0 * SIMD_WIDTH_F32), VecLoadF32(B + I + 0 * SIMD_WIDTH_F32), Sum0);
		Sum1 = VecFmaddF32(VecLoadF32(A + I + 1 * SIMD_WIDTH_F32), VecLoadF32(B + I + 1 * SIMD_WIDTH_F32), Sum1);
		Sum2 = VecFmaddF32(VecLoadF32(A + I + 2 * SIMD_WIDTH_F32), VecLoadF32(B + I + 2 * SIMD_WIDTH_F32), Sum2);
		Sum3 = VecFmaddF32(VecLoadF32(A + I + 3 * SIMD_WIDTH_F32), VecLoadF32(B + I + 3 * SIMD_WIDTH_F32), Sum3);
	}
	VecF32 Sum = VecAddF32(VecAddF32(Sum0, Sum1), VecAddF32(Sum2, Sum3));
	for(; I + SIMD_WIDTH_F32 <= Count; I += SIMD_WIDTH_F32)
		Sum = VecFmaddF32(VecLoadF32(A + I), VecLoadF32(B + I), Sum);
	float Acc = VecHorizontalAddF32(Sum);
	for(; I < Count; ++I)
		Acc += A[I] * B[I];
	return Acc;
}

float L2SquaredKernelF32(const float *A, const float *B, size_t Count) {
	const size_t UNROLL = 4;
	const size_t STEP = SIMD_WIDTH_F32 * UNROLL;
	size_t I = 0;
	VecF32 Sum0 = VecZeroF32();
	VecF32 Sum1 = VecZeroF32();
	VecF32 Sum2 = VecZeroF32();
	VecF32 Sum3 = VecZeroF32();
	for(; I + STEP <= Count; I += STEP) {
		const VecF32 D0 = VecSubF32(VecLoadF32(A + I + 0 * SIMD_WIDTH_F32), VecLoadF32(B + I + 0 * SIMD_WIDTH_F32));
		const VecF32 D1 = VecSubF32(VecLoadF32(A + I + 1 * SIMD_WIDTH_F32), VecLoadF32(B + I + 1 * SIMD_WIDTH_F32));
		const VecF32 D2 = VecSubF32(VecLoadF32(A + I + 2 * SIMD_WIDTH_F32), VecLoadF32(B + I + 2 * SIMD_WIDTH_F32));
		const VecF32 D3 = VecSubF32(VecLoadF32(A + I + 3 * SIMD_WIDTH_F32), VecLoadF32(B + I + 3 * SIMD_WIDTH_F32));
		Sum0 = VecFmaddF32(D0, D0, Sum0);
		Sum1 = VecFmaddF32(D1, D1, Sum1);
		Sum2 = VecFmaddF32(D2, D2, Sum2);
		Sum3 = VecFmaddF32(D3, D3, Sum3);
	}
	VecF32 Sum = VecAddF32(VecAddF32(Sum0, Sum1), VecAddF32(Sum2, Sum3));
	for(; I + SIMD_WIDTH_F32 <= Count; I += SIMD_WIDTH_F32) {
		const VecF32 D = VecSubF32(VecLoadF32(A + I), VecLoadF32(B + I));
		Sum = VecFmaddF32(D, D, Sum);
	}
	float Acc = VecHorizontalAddF32(Sum);
	for(; I < Count; ++I) {
		const float D = A[I] - B[I];
		Acc += D * D;
	}
	return Acc;
}

void GemvMicroKernelF32(const float *Matrix, const float *Vector, float *Out, size_t LeadingDim, size_t Kc, size_t Mr) {
	float AccScalars[4] = {0.f, 0.f, 0.f, 0.f};
	for(size_t R = 0; R < Mr; ++R)
		AccScalars[R] = Out[R];

	VecF32 Acc0 = VecZeroF32();
	VecF32 Acc1 = VecZeroF32();
	VecF32 Acc2 = VecZeroF32();
	VecF32 Acc3 = VecZeroF32();
	size_t K = 0;
	for(; K + SIMD_WIDTH_F32 <= Kc; K += SIMD_WIDTH_F32) {
		const VecF32 X = VecLoadF32(Vector + K);
		if(Mr > 0) Acc0 = VecFmaddF32(VecLoadF32(Matrix + 0 * LeadingDim + K), X, Acc0);
		if(Mr > 1) Acc1 = VecFmaddF32(VecLoadF32(Matrix + 1 * LeadingDim + K), X, Acc1);
		if(Mr > 2) Acc2 = VecFmaddF32(VecLoadF32(Matrix + 2 * LeadingDim + K), X, Acc2);
		if(Mr > 3) Acc3 = VecFmaddF32(VecLoadF32(Matrix + 3 * LeadingDim + K), X, Acc3);
	}

	if(Mr > 0) AccScalars[0] += VecHorizontalAddF32(Acc0);
	if(Mr > 1) AccScalars[1] += VecHorizontalAddF32(Acc1);
	if(Mr > 2) AccScalars[2] += VecHorizontalAddF32(Acc2);
	if(Mr > 3) AccScalars[3] += VecHorizontalAddF32(Acc3);

	for(; K < Kc; ++K) {
		const float X = Vector[K];
		for(size_t R = 0; R < Mr; ++R)
			AccScalars[R] += Matrix[R * LeadingDim + K] * X;
	}

	for(size_t R = 0; R < Mr; ++R)
		Out[R] = AccScalars[R];
}
#endif

[[maybe_unused]] void ComplexMulAccumulateScalar(const float *ARe, const float *AIm, const float *BRe,
                                                 const float *BIm, float *OutRe, float *OutIm, size_t Count) {
	for(size_t I = 0; I < Count; ++I) {
		const float Re = ARe[I] * BRe[I] - AIm[I] * BIm[I];
		const float Im = ARe[I] * BIm[I] + AIm[I] * BRe[I];
		OutRe[I] += Re;
		OutIm[I] += Im;
	}
}

} // namespace

KernelConfig ActiveKernelConfig() {
	KernelConfig Out = DefaultKernelConfig();
	Out.VectorWidthF32 = ParseEnvSizeT("ASTRALDB_SIMD_VEC_WIDTH", Out.VectorWidthF32);
	Out.DotUnroll = ParseEnvSizeT("ASTRALDB_SIMD_DOT_UNROLL", Out.DotUnroll);
	Out.GemvMr = ParseEnvSizeT("ASTRALDB_SIMD_GEMV_MR", Out.GemvMr);
	Out.GemvKc = ParseEnvSizeT("ASTRALDB_SIMD_GEMV_KC", Out.GemvKc);
	if(Out.VectorWidthF32 == 0)
		Out.VectorWidthF32 = 1;
	if(Out.DotUnroll == 0)
		Out.DotUnroll = 1;
	if(Out.GemvMr == 0)
		Out.GemvMr = 1;
	if(Out.GemvMr > 4)
		Out.GemvMr = 4;
	if(Out.GemvKc == 0)
		Out.GemvKc = 32;
	return Out;
}

float DotProductF32(const float *A, const float *B, size_t Count) {
	float Acc = 0.f;
#if defined(__AVX2__) || defined(__SSE2__) || defined(__ARM_NEON) || defined(__aarch64__)
	Acc = DotKernelF32(A, B, Count);
#else
	DotScalar(A, B, Count, Acc);
#endif
	return Acc;
}

float L2SquaredF32(const float *A, const float *B, size_t Count) {
#if defined(__AVX2__) || defined(__SSE2__) || defined(__ARM_NEON) || defined(__aarch64__)
	return L2SquaredKernelF32(A, B, Count);
#else
	float Acc = 0.f;
	for(size_t I = 0; I < Count; ++I) {
		const float D = A[I] - B[I];
		Acc += D * D;
	}
	return Acc;
#endif
}

void SubF32(float *Dst, const float *A, const float *B, size_t Count) {
#if defined(__AVX2__) || defined(__SSE2__) || defined(__ARM_NEON) || defined(__aarch64__)
	SubKernelF32(Dst, A, B, Count);
#else
	SubScalar(Dst, A, B, Count);
#endif
}

void MulF32(float *Dst, const float *A, const float *B, size_t Count) {
#if defined(__AVX2__) || defined(__SSE2__) || defined(__ARM_NEON) || defined(__aarch64__)
	MulKernelF32(Dst, A, B, Count);
#else
	MulScalar(Dst, A, B, Count);
#endif
}

void ScaleF32(float *Dst, const float *A, float Scalar, size_t Count) {
#if defined(__AVX2__) || defined(__SSE2__) || defined(__ARM_NEON) || defined(__aarch64__)
	ScaleKernelF32(Dst, A, Scalar, Count);
#else
	ScaleScalar(Dst, A, Scalar, Count);
#endif
}

void AddF32(float *Dst, const float *A, const float *B, size_t Count) {
#if defined(__AVX2__) || defined(__SSE2__) || defined(__ARM_NEON) || defined(__aarch64__)
	AddKernelF32(Dst, A, B, Count);
#else
	AddScalar(Dst, A, B, Count);
#endif
}

void MulAccumulateF32(float *Dst, const float *A, float Scalar, size_t Count) {
#if defined(__AVX2__) || defined(__SSE2__) || defined(__ARM_NEON) || defined(__aarch64__)
	MulAccumulateKernelF32(Dst, A, Scalar, Count);
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
	if(Rows == 0) return;
	Memset(Out, 0, Rows * sizeof(float));
	if(Cols == 0) return;

	const KernelConfig Cfg = ActiveKernelConfig();
	const size_t MR = Cfg.GemvMr;
	const size_t KC = Cfg.GemvKc;
	for(size_t C0 = 0; C0 < Cols; C0 += KC) {
		const size_t CurK = std::min(KC, Cols - C0);
		const float *VecBlock = Vector + C0;
		for(size_t R0 = 0; R0 < Rows; R0 += MR) {
			const size_t CurM = std::min(MR, Rows - R0);
			const float *MatBlock = MatrixRowMajor + R0 * Cols + C0;
#if defined(__AVX2__) || defined(__SSE2__) || defined(__ARM_NEON) || defined(__aarch64__)
			GemvMicroKernelF32(MatBlock, VecBlock, Out + R0, Cols, CurK, CurM);
#else
			for(size_t R = 0; R < CurM; ++R) {
				float Acc = Out[R0 + R];
				const float *Row = MatBlock + R * Cols;
				for(size_t K = 0; K < CurK; ++K)
					Acc += Row[K] * VecBlock[K];
				Out[R0 + R] = Acc;
			}
#endif
		}
	}
}

void ComplexDotHermitianInterleavedF32(const float *A, const float *B, size_t Slots, float &OutRe, float &OutIm) {
	float AccRe = 0.f;
	float AccIm = 0.f;
	size_t I = 0;
	for(; I + 3 < Slots; I += 4) {
		const float Ar0 = A[2 * (I + 0)];
		const float Ai0 = A[2 * (I + 0) + 1];
		const float Br0 = B[2 * (I + 0)];
		const float Bi0 = B[2 * (I + 0) + 1];
		const float Ar1 = A[2 * (I + 1)];
		const float Ai1 = A[2 * (I + 1) + 1];
		const float Br1 = B[2 * (I + 1)];
		const float Bi1 = B[2 * (I + 1) + 1];
		const float Ar2 = A[2 * (I + 2)];
		const float Ai2 = A[2 * (I + 2) + 1];
		const float Br2 = B[2 * (I + 2)];
		const float Bi2 = B[2 * (I + 2) + 1];
		const float Ar3 = A[2 * (I + 3)];
		const float Ai3 = A[2 * (I + 3) + 1];
		const float Br3 = B[2 * (I + 3)];
		const float Bi3 = B[2 * (I + 3) + 1];
		AccRe += Ar0 * Br0 + Ai0 * Bi0;
		AccIm += Ar0 * Bi0 - Ai0 * Br0;
		AccRe += Ar1 * Br1 + Ai1 * Bi1;
		AccIm += Ar1 * Bi1 - Ai1 * Br1;
		AccRe += Ar2 * Br2 + Ai2 * Bi2;
		AccIm += Ar2 * Bi2 - Ai2 * Br2;
		AccRe += Ar3 * Br3 + Ai3 * Bi3;
		AccIm += Ar3 * Bi3 - Ai3 * Br3;
	}
	for(; I < Slots; ++I) {
		const float Ar = A[2 * I];
		const float Ai = A[2 * I + 1];
		const float Br = B[2 * I];
		const float Bi = B[2 * I + 1];
		AccRe += Ar * Br + Ai * Bi;
		AccIm += Ar * Bi - Ai * Br;
	}
	OutRe += AccRe;
	OutIm += AccIm;
}

void ComplexAddInterleavedF32(float *Dst, const float *A, const float *B, size_t Slots) {
	const size_t N = Slots * 2;
#if defined(__AVX2__)
	size_t I = 0;
	for(; I + 7 < N; I += 8) {
		__m256 Va = _mm256_loadu_ps(A + I);
		__m256 Vb = _mm256_loadu_ps(B + I);
		_mm256_storeu_ps(Dst + I, _mm256_add_ps(Va, Vb));
	}
	for(; I < N; ++I)
		Dst[I] = A[I] + B[I];
#elif defined(__SSE2__)
	size_t I = 0;
	for(; I + 7 < N; I += 8) {
		__m128 Va = _mm_loadu_ps(A + I);
		__m128 Vb = _mm_loadu_ps(B + I);
		_mm_storeu_ps(Dst + I, _mm_add_ps(Va, Vb));
		Va = _mm_loadu_ps(A + I + 4);
		Vb = _mm_loadu_ps(B + I + 4);
		_mm_storeu_ps(Dst + I + 4, _mm_add_ps(Va, Vb));
	}
	for(; I < N; ++I)
		Dst[I] = A[I] + B[I];
#else
	for(size_t I = 0; I < N; ++I)
		Dst[I] = A[I] + B[I];
#endif
}

void ComplexScaleInterleavedF32(float *Dst, const float *A, float ScaleRe, float ScaleIm, size_t Slots) {
	for(size_t I = 0; I < Slots; ++I) {
		const float Ar = A[2 * I];
		const float Ai = A[2 * I + 1];
		Dst[2 * I] = Ar * ScaleRe - Ai * ScaleIm;
		Dst[2 * I + 1] = Ar * ScaleIm + Ai * ScaleRe;
	}
}

void ComplexNormSqInterleavedF32(const float *A, size_t Slots, float &OutRe, float &OutIm) {
	float AccRe = 0.f;
	float AccIm = 0.f;
	size_t I = 0;
	for(; I + 3 < Slots; I += 4) {
		const float Ar0 = A[2 * (I + 0)];
		const float Ai0 = A[2 * (I + 0) + 1];
		const float Ar1 = A[2 * (I + 1)];
		const float Ai1 = A[2 * (I + 1) + 1];
		const float Ar2 = A[2 * (I + 2)];
		const float Ai2 = A[2 * (I + 2) + 1];
		const float Ar3 = A[2 * (I + 3)];
		const float Ai3 = A[2 * (I + 3) + 1];
		AccRe += Ar0 * Ar0 + Ai0 * Ai0;
		AccRe += Ar1 * Ar1 + Ai1 * Ai1;
		AccRe += Ar2 * Ar2 + Ai2 * Ai2;
		AccRe += Ar3 * Ar3 + Ai3 * Ai3;
	}
	for(; I < Slots; ++I) {
		const float Ar = A[2 * I];
		const float Ai = A[2 * I + 1];
		AccRe += Ar * Ar + Ai * Ai;
	}
	OutRe = AccRe;
	OutIm = AccIm;
}

void ComplexMatVecInterleavedF32(const float *Mat, const float *Vec, float *Out, size_t Rows, size_t Cols) {
	if(Rows == 0) return;
	Memset(Out, 0, Rows * 2 * sizeof(float));
	if(Cols == 0) return;

	const KernelConfig Cfg = ActiveKernelConfig();
	const size_t KC = Cfg.GemvKc;
	for(size_t R = 0; R < Rows; ++R) {
		float AccRe = 0.f;
		float AccIm = 0.f;
		const float *Row = Mat + (R * Cols) * 2;
		for(size_t C0 = 0; C0 < Cols; C0 += KC) {
			const size_t CurK = std::min(KC, Cols - C0);
			const float *RowBlock = Row + C0 * 2;
			const float *VecBlock = Vec + C0 * 2;
			for(size_t K = 0; K < CurK; ++K) {
				const float Ar = RowBlock[2 * K];
				const float Ai = RowBlock[2 * K + 1];
				const float Br = VecBlock[2 * K];
				const float Bi = VecBlock[2 * K + 1];
				AccRe += Ar * Br - Ai * Bi;
				AccIm += Ar * Bi + Ai * Br;
			}
		}
		Out[2 * R] = AccRe;
		Out[2 * R + 1] = AccIm;
	}
}

} // namespace Simd
} // namespace AstralDB
