#include <IO/SIMD.hxx>
#include <Database/Storage/SimdTiling.hxx>

#include <IO/MemoryGuard.hxx>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace AstralDB {
namespace Simd {
namespace {

KernelConfig DefaultKernelConfig() {
#if defined(__AVX512F__)
	return {16, 4, 4, 512};
#elif defined(__AVX2__)
	return {8, 4, 4, 384};
#elif defined(__ARM_FEATURE_SVE)
	return {static_cast<size_t>(svcntw()), 4, 4, 384};
#elif defined(__ARM_NEON) || defined(__aarch64__)
	return {4, 4, 4, 192};
#elif defined(__SSE4_1__) || defined(__SSE4_2__) || defined(__SSE2__)
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

#if defined(__AVX512F__)
using VecF32 = __m512;
static constexpr size_t SIMD_WIDTH_F32 = 16;

inline VecF32 VecZeroF32() { return _mm512_setzero_ps(); }
inline VecF32 VecLoadF32(const float *Ptr) { return _mm512_loadu_ps(Ptr); }
inline void VecStoreF32(float *Ptr, VecF32 V) { _mm512_storeu_ps(Ptr, V); }
inline VecF32 VecAddF32(VecF32 A, VecF32 B) { return _mm512_add_ps(A, B); }
inline VecF32 VecSubF32(VecF32 A, VecF32 B) { return _mm512_sub_ps(A, B); }
inline VecF32 VecMulF32(VecF32 A, VecF32 B) { return _mm512_mul_ps(A, B); }
inline VecF32 VecMaxF32(VecF32 A, VecF32 B) { return _mm512_max_ps(A, B); }
inline VecF32 VecMinF32(VecF32 A, VecF32 B) { return _mm512_min_ps(A, B); }
inline VecF32 VecSet1F32(float S) { return _mm512_set1_ps(S); }
inline VecF32 VecFmaddF32(VecF32 A, VecF32 B, VecF32 C) { return _mm512_fmadd_ps(A, B, C); }
inline float VecHorizontalAddF32(VecF32 V) { return _mm512_reduce_add_ps(V); }
#elif defined(__AVX2__)
using VecF32 = __m256;
static constexpr size_t SIMD_WIDTH_F32 = 8;

inline VecF32 VecZeroF32() { return _mm256_setzero_ps(); }
inline VecF32 VecLoadF32(const float *Ptr) { return _mm256_loadu_ps(Ptr); }
inline void VecStoreF32(float *Ptr, VecF32 V) { _mm256_storeu_ps(Ptr, V); }
inline VecF32 VecAddF32(VecF32 A, VecF32 B) { return _mm256_add_ps(A, B); }
inline VecF32 VecSubF32(VecF32 A, VecF32 B) { return _mm256_sub_ps(A, B); }
inline VecF32 VecMulF32(VecF32 A, VecF32 B) { return _mm256_mul_ps(A, B); }
inline VecF32 VecMaxF32(VecF32 A, VecF32 B) { return _mm256_max_ps(A, B); }
inline VecF32 VecMinF32(VecF32 A, VecF32 B) { return _mm256_min_ps(A, B); }
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
#elif defined(__ARM_NEON) || defined(__aarch64__)
using VecF32 = float32x4_t;
static constexpr size_t SIMD_WIDTH_F32 = 4;

inline VecF32 VecZeroF32() { return vdupq_n_f32(0.f); }
inline VecF32 VecLoadF32(const float *Ptr) { return vld1q_f32(Ptr); }
inline void VecStoreF32(float *Ptr, VecF32 V) { vst1q_f32(Ptr, V); }
inline VecF32 VecAddF32(VecF32 A, VecF32 B) { return vaddq_f32(A, B); }
inline VecF32 VecSubF32(VecF32 A, VecF32 B) { return vsubq_f32(A, B); }
inline VecF32 VecMulF32(VecF32 A, VecF32 B) { return vmulq_f32(A, B); }
inline VecF32 VecMaxF32(VecF32 A, VecF32 B) { return vmaxq_f32(A, B); }
inline VecF32 VecMinF32(VecF32 A, VecF32 B) { return vminq_f32(A, B); }
inline VecF32 VecSet1F32(float S) { return vdupq_n_f32(S); }
inline VecF32 VecFmaddF32(VecF32 A, VecF32 B, VecF32 C) { return vmlaq_f32(C, A, B); }
inline float VecHorizontalAddF32(VecF32 V) {
	const float32x2_t Pair = vadd_f32(vget_low_f32(V), vget_high_f32(V));
	return vget_lane_f32(vpadd_f32(Pair, Pair), 0);
}
#elif defined(__SSE4_1__) || defined(__SSE4_2__) || defined(__SSE2__)
using VecF32 = __m128;
static constexpr size_t SIMD_WIDTH_F32 = 4;

inline VecF32 VecZeroF32() { return _mm_setzero_ps(); }
inline VecF32 VecLoadF32(const float *Ptr) { return _mm_loadu_ps(Ptr); }
inline void VecStoreF32(float *Ptr, VecF32 V) { _mm_storeu_ps(Ptr, V); }
inline VecF32 VecAddF32(VecF32 A, VecF32 B) { return _mm_add_ps(A, B); }
inline VecF32 VecSubF32(VecF32 A, VecF32 B) { return _mm_sub_ps(A, B); }
inline VecF32 VecMulF32(VecF32 A, VecF32 B) { return _mm_mul_ps(A, B); }
inline VecF32 VecMaxF32(VecF32 A, VecF32 B) { return _mm_max_ps(A, B); }
inline VecF32 VecMinF32(VecF32 A, VecF32 B) { return _mm_min_ps(A, B); }
inline VecF32 VecSet1F32(float S) { return _mm_set1_ps(S); }
inline VecF32 VecFmaddF32(VecF32 A, VecF32 B, VecF32 C) { return _mm_add_ps(_mm_mul_ps(A, B), C); }
inline float VecHorizontalAddF32(VecF32 V) {
#if defined(__SSE4_1__) || defined(__SSE4_2__)
	__m128 T = _mm_hadd_ps(V, V);
	T = _mm_hadd_ps(T, T);
	return _mm_cvtss_f32(T);
#else
	alignas(16) float Buf[4];
	VecStoreF32(Buf, V);
	return Buf[0] + Buf[1] + Buf[2] + Buf[3];
#endif
}
#endif

#if defined(__AVX512F__) || defined(__AVX2__) || defined(__SSE4_1__) || defined(__SSE4_2__) || defined(__SSE2__) || \
    defined(__ARM_NEON) || defined(__aarch64__)
#define ASTRALDB_HAS_SIMD_F32 1
#endif

#if defined(__AVX512F__)
using VecF64 = __m512d;
static constexpr size_t SIMD_WIDTH_F64 = 8;

inline VecF64 VecZeroF64() { return _mm512_setzero_pd(); }
inline VecF64 VecLoadF64(const double *Ptr) { return _mm512_loadu_pd(Ptr); }
inline void VecStoreF64(double *Ptr, VecF64 V) { _mm512_storeu_pd(Ptr, V); }
inline VecF64 VecAddF64(VecF64 A, VecF64 B) { return _mm512_add_pd(A, B); }
inline VecF64 VecMulF64(VecF64 A, VecF64 B) { return _mm512_mul_pd(A, B); }
inline VecF64 VecMinF64(VecF64 A, VecF64 B) { return _mm512_min_pd(A, B); }
inline VecF64 VecMaxF64(VecF64 A, VecF64 B) { return _mm512_max_pd(A, B); }
inline VecF64 VecSet1F64(double S) { return _mm512_set1_pd(S); }
inline VecF64 VecFmaddF64(VecF64 A, VecF64 B, VecF64 C) { return _mm512_fmadd_pd(A, B, C); }
inline double VecHorizontalAddF64(VecF64 V) { return _mm512_reduce_add_pd(V); }
#elif defined(__AVX2__)
using VecF64 = __m256d;
static constexpr size_t SIMD_WIDTH_F64 = 4;

inline VecF64 VecZeroF64() { return _mm256_setzero_pd(); }
inline VecF64 VecLoadF64(const double *Ptr) { return _mm256_loadu_pd(Ptr); }
inline void VecStoreF64(double *Ptr, VecF64 V) { _mm256_storeu_pd(Ptr, V); }
inline VecF64 VecAddF64(VecF64 A, VecF64 B) { return _mm256_add_pd(A, B); }
inline VecF64 VecMulF64(VecF64 A, VecF64 B) { return _mm256_mul_pd(A, B); }
inline VecF64 VecMinF64(VecF64 A, VecF64 B) { return _mm256_min_pd(A, B); }
inline VecF64 VecMaxF64(VecF64 A, VecF64 B) { return _mm256_max_pd(A, B); }
inline VecF64 VecSet1F64(double S) { return _mm256_set1_pd(S); }
inline VecF64 VecFmaddF64(VecF64 A, VecF64 B, VecF64 C) {
#if defined(__FMA__)
	return _mm256_fmadd_pd(A, B, C);
#else
	return VecAddF64(VecMulF64(A, B), C);
#endif
}
inline double VecHorizontalAddF64(VecF64 V) {
	alignas(32) double Buf[4];
	VecStoreF64(Buf, V);
	return Buf[0] + Buf[1] + Buf[2] + Buf[3];
}
#elif defined(__ARM_NEON) || defined(__aarch64__)
using VecF64 = float64x2_t;
static constexpr size_t SIMD_WIDTH_F64 = 2;

inline VecF64 VecZeroF64() { return vdupq_n_f64(0.0); }
inline VecF64 VecLoadF64(const double *Ptr) { return vld1q_f64(Ptr); }
inline void VecStoreF64(double *Ptr, VecF64 V) { vst1q_f64(Ptr, V); }
inline VecF64 VecAddF64(VecF64 A, VecF64 B) { return vaddq_f64(A, B); }
inline VecF64 VecMulF64(VecF64 A, VecF64 B) { return vmulq_f64(A, B); }
inline VecF64 VecMinF64(VecF64 A, VecF64 B) { return vminq_f64(A, B); }
inline VecF64 VecMaxF64(VecF64 A, VecF64 B) { return vmaxq_f64(A, B); }
inline VecF64 VecSet1F64(double S) { return vdupq_n_f64(S); }
inline VecF64 VecFmaddF64(VecF64 A, VecF64 B, VecF64 C) { return vfmaq_f64(C, A, B); }
inline double VecHorizontalAddF64(VecF64 V) { return vgetq_lane_f64(V, 0) + vgetq_lane_f64(V, 1); }
#endif

#if defined(__AVX512F__) || defined(__AVX2__) || defined(__ARM_NEON) || defined(__aarch64__)
#define ASTRALDB_HAS_SIMD_F64 1
#endif

#if defined(__ARM_FEATURE_SVE)
float DotKernelSveF32(const float *A, const float *B, size_t Count) {
	size_t I = 0;
	svfloat32_t Sum = svdup_f32(0.f);
	svbool_t Pg = svwhilelt_b32(I, Count);
	while(svptest_any(svptrue_b32(), Pg)) {
		const svfloat32_t Va = svld1_f32(Pg, A + I);
		const svfloat32_t Vb = svld1_f32(Pg, B + I);
		Sum = svmad_f32_m(Pg, Va, Vb, Sum);
		I += svcntw();
		Pg = svwhilelt_b32(I, Count);
	}
	return svaddv_f32(svptrue_b32(), Sum);
}

float SumKernelSveF32(const float *A, size_t Count) {
	size_t I = 0;
	svfloat32_t Sum = svdup_f32(0.f);
	svbool_t Pg = svwhilelt_b32(I, Count);
	while(svptest_any(svptrue_b32(), Pg)) {
		Sum = svadd_f32_m(Pg, Sum, svld1_f32(Pg, A + I));
		I += svcntw();
		Pg = svwhilelt_b32(I, Count);
	}
	return svaddv_f32(svptrue_b32(), Sum);
}

double SumKernelSveF64(const double *A, size_t Count) {
	size_t I = 0;
	svfloat64_t Sum = svdup_f64(0.0);
	svbool_t Pg = svwhilelt_b64(I, Count);
	while(svptest_any(svptrue_b64(), Pg)) {
		Sum = svadd_f64_m(Pg, Sum, svld1_f64(Pg, A + I));
		I += svcntd();
		Pg = svwhilelt_b64(I, Count);
	}
	return svaddv_f64(svptrue_b64(), Sum);
}

int64_t SumKernelSveI64(const int64_t *A, size_t Count) {
	size_t I = 0;
	svint64_t Sum = svdup_s64(0);
	svbool_t Pg = svwhilelt_b64(I, Count);
	while(svptest_any(svptrue_b64(), Pg)) {
		Sum = svadd_s64_m(Pg, Sum, svld1_s64(Pg, A + I));
		I += svcntd();
		Pg = svwhilelt_b64(I, Count);
	}
	return svaddv_s64(svptrue_b64(), Sum);
}
#endif

#ifdef ASTRALDB_HAS_SIMD_F64
double SumKernelF64(const double *A, size_t Count) {
	const size_t UNROLL = 4;
	const size_t STEP = SIMD_WIDTH_F64 * UNROLL;
	size_t I = 0;
	VecF64 Sum0 = VecZeroF64();
	VecF64 Sum1 = VecZeroF64();
	VecF64 Sum2 = VecZeroF64();
	VecF64 Sum3 = VecZeroF64();
	for(; I + STEP <= Count; I += STEP) {
		Sum0 = VecAddF64(Sum0, VecLoadF64(A + I + 0 * SIMD_WIDTH_F64));
		Sum1 = VecAddF64(Sum1, VecLoadF64(A + I + 1 * SIMD_WIDTH_F64));
		Sum2 = VecAddF64(Sum2, VecLoadF64(A + I + 2 * SIMD_WIDTH_F64));
		Sum3 = VecAddF64(Sum3, VecLoadF64(A + I + 3 * SIMD_WIDTH_F64));
	}
	VecF64 Sum = VecAddF64(VecAddF64(Sum0, Sum1), VecAddF64(Sum2, Sum3));
	for(; I + SIMD_WIDTH_F64 <= Count; I += SIMD_WIDTH_F64)
		Sum = VecAddF64(Sum, VecLoadF64(A + I));
	double Acc = VecHorizontalAddF64(Sum);
	for(; I < Count; ++I)
		Acc += A[I];
	return Acc;
}

double DotKernelF64(const double *A, const double *B, size_t Count) {
	const size_t UNROLL = 4;
	const size_t STEP = SIMD_WIDTH_F64 * UNROLL;
	size_t I = 0;
	VecF64 Sum0 = VecZeroF64();
	VecF64 Sum1 = VecZeroF64();
	VecF64 Sum2 = VecZeroF64();
	VecF64 Sum3 = VecZeroF64();
	for(; I + STEP <= Count; I += STEP) {
		Sum0 = VecFmaddF64(VecLoadF64(A + I + 0 * SIMD_WIDTH_F64), VecLoadF64(B + I + 0 * SIMD_WIDTH_F64), Sum0);
		Sum1 = VecFmaddF64(VecLoadF64(A + I + 1 * SIMD_WIDTH_F64), VecLoadF64(B + I + 1 * SIMD_WIDTH_F64), Sum1);
		Sum2 = VecFmaddF64(VecLoadF64(A + I + 2 * SIMD_WIDTH_F64), VecLoadF64(B + I + 2 * SIMD_WIDTH_F64), Sum2);
		Sum3 = VecFmaddF64(VecLoadF64(A + I + 3 * SIMD_WIDTH_F64), VecLoadF64(B + I + 3 * SIMD_WIDTH_F64), Sum3);
	}
	VecF64 Sum = VecAddF64(VecAddF64(Sum0, Sum1), VecAddF64(Sum2, Sum3));
	for(; I + SIMD_WIDTH_F64 <= Count; I += SIMD_WIDTH_F64)
		Sum = VecFmaddF64(VecLoadF64(A + I), VecLoadF64(B + I), Sum);
	double Acc = VecHorizontalAddF64(Sum);
	for(; I < Count; ++I)
		Acc += A[I] * B[I];
	return Acc;
}

double MinKernelF64(const double *A, size_t Count) {
	if(Count == 0)
		return 0.0;
	if(Count < SIMD_WIDTH_F64) {
		double Acc = A[0];
		for(size_t J = 1; J < Count; ++J)
			Acc = std::min(Acc, A[J]);
		return Acc;
	}
	size_t I = 0;
	VecF64 M = VecLoadF64(A);
	I += SIMD_WIDTH_F64;
	for(; I + SIMD_WIDTH_F64 <= Count; I += SIMD_WIDTH_F64)
		M = VecMinF64(M, VecLoadF64(A + I));
	alignas(32) double Buf[8];
	VecStoreF64(Buf, M);
	double Acc = Buf[0];
	for(size_t J = 1; J < SIMD_WIDTH_F64; ++J)
		Acc = std::min(Acc, Buf[J]);
	for(; I < Count; ++I)
		Acc = std::min(Acc, A[I]);
	return Acc;
}

double MaxKernelF64(const double *A, size_t Count) {
	if(Count == 0)
		return 0.0;
	if(Count < SIMD_WIDTH_F64) {
		double Acc = A[0];
		for(size_t J = 1; J < Count; ++J)
			Acc = std::max(Acc, A[J]);
		return Acc;
	}
	size_t I = 0;
	VecF64 M = VecLoadF64(A);
	I += SIMD_WIDTH_F64;
	for(; I + SIMD_WIDTH_F64 <= Count; I += SIMD_WIDTH_F64)
		M = VecMaxF64(M, VecLoadF64(A + I));
	alignas(32) double Buf[8];
	VecStoreF64(Buf, M);
	double Acc = Buf[0];
	for(size_t J = 1; J < SIMD_WIDTH_F64; ++J)
		Acc = std::max(Acc, Buf[J]);
	for(; I < Count; ++I)
		Acc = std::max(Acc, A[I]);
	return Acc;
}

void AddKernelF64(double *Dst, const double *A, const double *B, size_t Count) {
	size_t I = 0;
	for(; I + SIMD_WIDTH_F64 <= Count; I += SIMD_WIDTH_F64)
		VecStoreF64(Dst + I, VecAddF64(VecLoadF64(A + I), VecLoadF64(B + I)));
	for(; I < Count; ++I)
		Dst[I] = A[I] + B[I];
}

void ScaleKernelF64(double *Dst, const double *A, double Scalar, size_t Count) {
	const VecF64 S = VecSet1F64(Scalar);
	size_t I = 0;
	for(; I + SIMD_WIDTH_F64 <= Count; I += SIMD_WIDTH_F64)
		VecStoreF64(Dst + I, VecMulF64(VecLoadF64(A + I), S));
	for(; I < Count; ++I)
		Dst[I] = A[I] * Scalar;
}
#endif

#if defined(__AVX512F__)
int64_t SumKernelI64(const int64_t *A, size_t Count) {
	size_t I = 0;
	__m512i Sum = _mm512_setzero_si512();
	for(; I + 8 <= Count; I += 8)
		Sum = _mm512_add_epi64(Sum, _mm512_loadu_si512(reinterpret_cast<const __m512i *>(A + I)));
	alignas(64) int64_t Parts[8];
	_mm512_storeu_si512(reinterpret_cast<__m512i *>(Parts), Sum);
	int64_t Acc = Parts[0] + Parts[1] + Parts[2] + Parts[3] + Parts[4] + Parts[5] + Parts[6] + Parts[7];
	for(; I < Count; ++I)
		Acc += A[I];
	return Acc;
}

int64_t MinKernelI64(const int64_t *A, size_t Count) {
	if(Count == 0)
		return 0;
	size_t I = 0;
	__m512i M = _mm512_loadu_si512(reinterpret_cast<const __m512i *>(A));
	I += 8;
	for(; I + 8 <= Count; I += 8)
		M = _mm512_min_epi64(M, _mm512_loadu_si512(reinterpret_cast<const __m512i *>(A + I)));
	alignas(64) int64_t Parts[8];
	_mm512_storeu_si512(reinterpret_cast<__m512i *>(Parts), M);
	int64_t Acc = Parts[0];
	for(int J = 1; J < 8; ++J)
		Acc = std::min(Acc, Parts[J]);
	for(; I < Count; ++I)
		Acc = std::min(Acc, A[I]);
	return Acc;
}

int64_t MaxKernelI64(const int64_t *A, size_t Count) {
	if(Count == 0)
		return 0;
	size_t I = 0;
	__m512i M = _mm512_loadu_si512(reinterpret_cast<const __m512i *>(A));
	I += 8;
	for(; I + 8 <= Count; I += 8)
		M = _mm512_max_epi64(M, _mm512_loadu_si512(reinterpret_cast<const __m512i *>(A + I)));
	alignas(64) int64_t Parts[8];
	_mm512_storeu_si512(reinterpret_cast<__m512i *>(Parts), M);
	int64_t Acc = Parts[0];
	for(int J = 1; J < 8; ++J)
		Acc = std::max(Acc, Parts[J]);
	for(; I < Count; ++I)
		Acc = std::max(Acc, A[I]);
	return Acc;
}
#elif defined(__AVX2__)
int64_t SumKernelI64(const int64_t *A, size_t Count) {
	__m256i Acc = _mm256_setzero_si256();
	size_t I = 0;
	for(; I + 4 <= Count; I += 4)
		Acc = _mm256_add_epi64(Acc, _mm256_set_epi64x(A[I + 3], A[I + 2], A[I + 1], A[I]));
	alignas(32) int64_t Parts[4];
	_mm256_store_si256(reinterpret_cast<__m256i *>(Parts), Acc);
	int64_t Sum = Parts[0] + Parts[1] + Parts[2] + Parts[3];
	for(; I < Count; ++I)
		Sum += A[I];
	return Sum;
}
#elif defined(__ARM_NEON) || defined(__aarch64__)
int64_t SumKernelI64(const int64_t *A, size_t Count) {
	int64x2_t Acc = vdupq_n_s64(0);
	size_t I = 0;
	for(; I + 2 <= Count; I += 2)
		Acc = vaddq_s64(Acc, vld1q_s64(A + I));
	int64_t Sum = vgetq_lane_s64(Acc, 0) + vgetq_lane_s64(Acc, 1);
	for(; I < Count; ++I)
		Sum += A[I];
	return Sum;
}

int64_t MinKernelI64(const int64_t *A, size_t Count) {
	if(Count == 0)
		return 0;
	size_t I = 0;
	int64x2_t M = vld1q_s64(A);
	I += 2;
	for(; I + 2 <= Count; I += 2) {
		const int64x2_t V = vld1q_s64(A + I);
		M = vbslq_s64(vcltq_s64(V, M), V, M);
	}
	int64_t Acc = std::min(vgetq_lane_s64(M, 0), vgetq_lane_s64(M, 1));
	for(; I < Count; ++I)
		Acc = std::min(Acc, A[I]);
	return Acc;
}

int64_t MaxKernelI64(const int64_t *A, size_t Count) {
	if(Count == 0)
		return 0;
	size_t I = 0;
	int64x2_t M = vld1q_s64(A);
	I += 2;
	for(; I + 2 <= Count; I += 2) {
		const int64x2_t V = vld1q_s64(A + I);
		M = vbslq_s64(vcgtq_s64(V, M), V, M);
	}
	int64_t Acc = std::max(vgetq_lane_s64(M, 0), vgetq_lane_s64(M, 1));
	for(; I < Count; ++I)
		Acc = std::max(Acc, A[I]);
	return Acc;
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

#ifdef ASTRALDB_HAS_SIMD_F32
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

float SumKernelF32(const float *A, size_t Count) {
	const size_t UNROLL = 4;
	const size_t STEP = SIMD_WIDTH_F32 * UNROLL;
	size_t I = 0;
	VecF32 Sum0 = VecZeroF32();
	VecF32 Sum1 = VecZeroF32();
	VecF32 Sum2 = VecZeroF32();
	VecF32 Sum3 = VecZeroF32();
	for(; I + STEP <= Count; I += STEP) {
		Sum0 = VecAddF32(Sum0, VecLoadF32(A + I + 0 * SIMD_WIDTH_F32));
		Sum1 = VecAddF32(Sum1, VecLoadF32(A + I + 1 * SIMD_WIDTH_F32));
		Sum2 = VecAddF32(Sum2, VecLoadF32(A + I + 2 * SIMD_WIDTH_F32));
		Sum3 = VecAddF32(Sum3, VecLoadF32(A + I + 3 * SIMD_WIDTH_F32));
	}
	VecF32 Sum = VecAddF32(VecAddF32(Sum0, Sum1), VecAddF32(Sum2, Sum3));
	for(; I + SIMD_WIDTH_F32 <= Count; I += SIMD_WIDTH_F32)
		Sum = VecAddF32(Sum, VecLoadF32(A + I));
	float Acc = VecHorizontalAddF32(Sum);
	for(; I < Count; ++I)
		Acc += A[I];
	return Acc;
}

float MaxKernelF32(const float *A, size_t Count) {
	if(Count == 0)
		return 0.f;
	if(Count < SIMD_WIDTH_F32) {
		float Acc = A[0];
		for(size_t I = 1; I < Count; ++I)
			Acc = std::max(Acc, A[I]);
		return Acc;
	}
	size_t I = 0;
	VecF32 M = VecLoadF32(A);
	I += SIMD_WIDTH_F32;
	for(; I + SIMD_WIDTH_F32 <= Count; I += SIMD_WIDTH_F32)
		M = VecMaxF32(M, VecLoadF32(A + I));
	alignas(64) float Buf[16];
	VecStoreF32(Buf, M);
	float Acc = Buf[0];
	for(size_t J = 1; J < SIMD_WIDTH_F32; ++J)
		Acc = std::max(Acc, Buf[J]);
	for(; I < Count; ++I)
		Acc = std::max(Acc, A[I]);
	return Acc;
}

float MinKernelF32(const float *A, size_t Count) {
	if(Count == 0)
		return 0.f;
	if(Count < SIMD_WIDTH_F32) {
		float Acc = A[0];
		for(size_t I = 1; I < Count; ++I)
			Acc = std::min(Acc, A[I]);
		return Acc;
	}
	size_t I = 0;
	VecF32 M = VecLoadF32(A);
	I += SIMD_WIDTH_F32;
	for(; I + SIMD_WIDTH_F32 <= Count; I += SIMD_WIDTH_F32)
		M = VecMinF32(M, VecLoadF32(A + I));
	alignas(64) float Buf[16];
	VecStoreF32(Buf, M);
	float Acc = Buf[0];
	for(size_t J = 1; J < SIMD_WIDTH_F32; ++J)
		Acc = std::min(Acc, Buf[J]);
	for(; I < Count; ++I)
		Acc = std::min(Acc, A[I]);
	return Acc;
}

void ReluKernelF32(float *Dst, const float *A, size_t Count) {
	const VecF32 Zero = VecZeroF32();
	const size_t UNROLL = 4;
	const size_t STEP = SIMD_WIDTH_F32 * UNROLL;
	size_t I = 0;
	for(; I + STEP <= Count; I += STEP) {
		VecStoreF32(Dst + I + 0 * SIMD_WIDTH_F32,
		            VecMaxF32(VecLoadF32(A + I + 0 * SIMD_WIDTH_F32), Zero));
		VecStoreF32(Dst + I + 1 * SIMD_WIDTH_F32,
		            VecMaxF32(VecLoadF32(A + I + 1 * SIMD_WIDTH_F32), Zero));
		VecStoreF32(Dst + I + 2 * SIMD_WIDTH_F32,
		            VecMaxF32(VecLoadF32(A + I + 2 * SIMD_WIDTH_F32), Zero));
		VecStoreF32(Dst + I + 3 * SIMD_WIDTH_F32,
		            VecMaxF32(VecLoadF32(A + I + 3 * SIMD_WIDTH_F32), Zero));
	}
	for(; I + SIMD_WIDTH_F32 <= Count; I += SIMD_WIDTH_F32)
		VecStoreF32(Dst + I, VecMaxF32(VecLoadF32(A + I), Zero));
	for(; I < Count; ++I)
		Dst[I] = A[I] > 0.f ? A[I] : 0.f;
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
	SimdTiling::ApplyToKernelConfig(Out, WorkloadClass::MathSciNumeric);
	Out.GemvMr = ParseEnvSizeT("ASTRALDB_SIMD_GEMV_MR", Out.GemvMr);
	Out.GemvKc = ParseEnvSizeT("ASTRALDB_SIMD_GEMV_KC", Out.GemvKc);
	if(Out.VectorWidthF32 == 0)
		Out.VectorWidthF32 = 1;
	if(Out.DotUnroll == 0)
		Out.DotUnroll = 1;
	if(Out.GemvMr == 0)
		Out.GemvMr = 1;
	if(Out.GemvMr > 8)
		Out.GemvMr = 8;
	if(Out.GemvKc == 0)
		Out.GemvKc = 32;
	return Out;
}

float DotProductF32(const float *A, const float *B, size_t Count) {
#if defined(__ARM_FEATURE_SVE)
	if(Count >= static_cast<size_t>(svcntw()) * 2)
		return DotKernelSveF32(A, B, Count);
#endif
	float Acc = 0.f;
#ifdef ASTRALDB_HAS_SIMD_F32
	Acc = DotKernelF32(A, B, Count);
#else
	DotScalar(A, B, Count, Acc);
#endif
	return Acc;
}

float SumF32(const float *A, size_t Count) {
#if defined(__ARM_FEATURE_SVE)
	if(Count >= static_cast<size_t>(svcntw()) * 2)
		return SumKernelSveF32(A, Count);
#endif
#ifdef ASTRALDB_HAS_SIMD_F32
	return SumKernelF32(A, Count);
#else
	float Acc = 0.f;
	for(size_t I = 0; I < Count; ++I)
		Acc += A[I];
	return Acc;
#endif
}

float MaxF32(const float *A, size_t Count) {
#ifdef ASTRALDB_HAS_SIMD_F32
	return MaxKernelF32(A, Count);
#else
	if(Count == 0)
		return 0.f;
	float Acc = A[0];
	for(size_t I = 1; I < Count; ++I)
		Acc = std::max(Acc, A[I]);
	return Acc;
#endif
}

float MinF32(const float *A, size_t Count) {
#ifdef ASTRALDB_HAS_SIMD_F32
	return MinKernelF32(A, Count);
#else
	if(Count == 0)
		return 0.f;
	float Acc = A[0];
	for(size_t I = 1; I < Count; ++I)
		Acc = std::min(Acc, A[I]);
	return Acc;
#endif
}

float L2SquaredF32(const float *A, const float *B, size_t Count) {
#ifdef ASTRALDB_HAS_SIMD_F32
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

void ReluF32(float *Dst, const float *A, size_t Count) {
#ifdef ASTRALDB_HAS_SIMD_F32
	ReluKernelF32(Dst, A, Count);
#else
	for(size_t I = 0; I < Count; ++I)
		Dst[I] = A[I] > 0.f ? A[I] : 0.f;
#endif
}

void SubF32(float *Dst, const float *A, const float *B, size_t Count) {
#ifdef ASTRALDB_HAS_SIMD_F32
	SubKernelF32(Dst, A, B, Count);
#else
	SubScalar(Dst, A, B, Count);
#endif
}

void MulF32(float *Dst, const float *A, const float *B, size_t Count) {
#ifdef ASTRALDB_HAS_SIMD_F32
	MulKernelF32(Dst, A, B, Count);
#else
	MulScalar(Dst, A, B, Count);
#endif
}

void ScaleF32(float *Dst, const float *A, float Scalar, size_t Count) {
#ifdef ASTRALDB_HAS_SIMD_F32
	ScaleKernelF32(Dst, A, Scalar, Count);
#else
	ScaleScalar(Dst, A, Scalar, Count);
#endif
}

void AddF32(float *Dst, const float *A, const float *B, size_t Count) {
#ifdef ASTRALDB_HAS_SIMD_F32
	AddKernelF32(Dst, A, B, Count);
#else
	AddScalar(Dst, A, B, Count);
#endif
}

void MulAccumulateF32(float *Dst, const float *A, float Scalar, size_t Count) {
#ifdef ASTRALDB_HAS_SIMD_F32
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
#if defined(__AVX512F__)
	size_t I = 0;
	for(; I + 15 < Count; I += 16) {
		const __m512 Ar = _mm512_loadu_ps(ARe + I);
		const __m512 Ai = _mm512_loadu_ps(AIm + I);
		const __m512 Br = _mm512_loadu_ps(BRe + I);
		const __m512 Bi = _mm512_loadu_ps(BIm + I);
		__m512 Or = _mm512_loadu_ps(OutRe + I);
		__m512 Oi = _mm512_loadu_ps(OutIm + I);
		Or = _mm512_add_ps(Or, _mm512_sub_ps(_mm512_mul_ps(Ar, Br), _mm512_mul_ps(Ai, Bi)));
		Oi = _mm512_add_ps(Oi, _mm512_add_ps(_mm512_mul_ps(Ar, Bi), _mm512_mul_ps(Ai, Br)));
		_mm512_storeu_ps(OutRe + I, Or);
		_mm512_storeu_ps(OutIm + I, Oi);
	}
	for(; I < Count; ++I) {
		const float Re = ARe[I] * BRe[I] - AIm[I] * BIm[I];
		const float Im = ARe[I] * BIm[I] + AIm[I] * BRe[I];
		OutRe[I] += Re;
		OutIm[I] += Im;
	}
#elif defined(__AVX2__)
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
	const TiledCachePlan Plan = SimdTiling::ActivePlan(WorkloadClass::MathSciNumeric, sizeof(float));
	const size_t MC = Plan.Mc;
	SimdTileSession TileSession;
	SimdTiling::BeginTileScan(Cols, WorkloadClass::MathSciNumeric, sizeof(float), TileSession);
	for(size_t R0 = 0; R0 < Rows; R0 += MC) {
		const size_t CurMc = std::min(MC, Rows - R0);
		for(size_t C0 = 0; C0 < Cols; C0 += KC) {
			const size_t CurK = std::min(KC, Cols - C0);
			if(TileSession.Armed)
				SimdTiling::PrefetchStreamAhead(TileSession, Vector, C0 + CurK, sizeof(float), CurK);
			const float *VecBlock = Vector + C0;
			for(size_t R1 = 0; R1 < CurMc; R1 += MR) {
				const size_t CurMr = std::min(MR, CurMc - R1);
				const float *MatBlock = MatrixRowMajor + (R0 + R1) * Cols + C0;
#ifdef ASTRALDB_HAS_SIMD_F32
				GemvMicroKernelF32(MatBlock, VecBlock, Out + R0 + R1, Cols, CurK, CurMr);
#else
				for(size_t R = 0; R < CurMr; ++R) {
					float Acc = Out[R0 + R1 + R];
					const float *Row = MatBlock + R * Cols;
					for(size_t K = 0; K < CurK; ++K)
						Acc += Row[K] * VecBlock[K];
					Out[R0 + R1 + R] = Acc;
				}
#endif
			}
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
#if defined(__AVX512F__)
	size_t I = 0;
	for(; I + 15 < N; I += 16) {
		const __m512 Va = _mm512_loadu_ps(A + I);
		const __m512 Vb = _mm512_loadu_ps(B + I);
		_mm512_storeu_ps(Dst + I, _mm512_add_ps(Va, Vb));
	}
	for(; I < N; ++I)
		Dst[I] = A[I] + B[I];
#elif defined(__AVX2__)
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
	const size_t MR = Cfg.GemvMr;
	const TiledCachePlan Plan = SimdTiling::ActivePlan(WorkloadClass::MathSciNumeric, sizeof(float) * 2);
	const size_t MC = Plan.Mc;
	SimdTileSession TileSession;
	SimdTiling::BeginTileScan(Cols, WorkloadClass::MathSciNumeric, sizeof(float) * 2, TileSession);
	for(size_t R0 = 0; R0 < Rows; R0 += MC) {
		const size_t CurMc = std::min(MC, Rows - R0);
		for(size_t C0 = 0; C0 < Cols; C0 += KC) {
			const size_t CurK = std::min(KC, Cols - C0);
			if(TileSession.Armed)
				SimdTiling::PrefetchStreamAhead(TileSession, Vec, (C0 + CurK) * 2, sizeof(float) * 2, CurK * 2);
			for(size_t R1 = 0; R1 < CurMc; R1 += MR) {
				const size_t CurMr = std::min(MR, CurMc - R1);
				for(size_t R = 0; R < CurMr; ++R) {
					const size_t Row = R0 + R1 + R;
					float AccRe = Out[2 * Row];
					float AccIm = Out[2 * Row + 1];
					const float *RowBlock = Mat + (Row * Cols + C0) * 2;
					const float *VecBlock = Vec + C0 * 2;
					for(size_t K = 0; K < CurK; ++K) {
						const float Ar = RowBlock[2 * K];
						const float Ai = RowBlock[2 * K + 1];
						const float Br = VecBlock[2 * K];
						const float Bi = VecBlock[2 * K + 1];
						AccRe += Ar * Br - Ai * Bi;
						AccIm += Ar * Bi + Ai * Br;
					}
					Out[2 * Row] = AccRe;
					Out[2 * Row + 1] = AccIm;
				}
			}
		}
	}
}

namespace {

constexpr std::size_t MemChunkBytes() noexcept {
#if defined(__AVX512F__)
	return 64;
#elif defined(__AVX2__)
	return 32;
#elif defined(__ARM_NEON) || defined(__aarch64__) || defined(__SSE4_2__) || defined(__SSE4_1__) || defined(__SSE2__)
	return 16;
#else
	return 8;
#endif
}

[[nodiscard]] bool PtrAlignedTo(const void *Ptr, const std::size_t Align) noexcept {
	const auto A = reinterpret_cast<std::uintptr_t>(Ptr);
	return Align > 0 && (A & (Align - 1)) == 0;
}

void CopyBytes(char *Dst, const char *Src, std::size_t Size, [[maybe_unused]] const bool Aligned) {
#if defined(__AVX512F__)
	constexpr std::size_t Chunk = 64;
	if(Aligned) {
		while(Size >= Chunk) {
			const __m512i V = _mm512_load_si512(reinterpret_cast<const void *>(Src));
			_mm512_store_si512(reinterpret_cast<void *>(Dst), V);
			Src += Chunk;
			Dst += Chunk;
			Size -= Chunk;
		}
	} else {
		while(Size >= Chunk) {
			const __m512i V = _mm512_loadu_si512(reinterpret_cast<const void *>(Src));
			_mm512_storeu_si512(reinterpret_cast<void *>(Dst), V);
			Src += Chunk;
			Dst += Chunk;
			Size -= Chunk;
		}
	}
#elif defined(__AVX2__)
	constexpr std::size_t Chunk = 32;
	if(Aligned) {
		while(Size >= Chunk) {
			const __m256i V = _mm256_load_si256(reinterpret_cast<const __m256i *>(Src));
			_mm256_store_si256(reinterpret_cast<__m256i *>(Dst), V);
			Src += Chunk;
			Dst += Chunk;
			Size -= Chunk;
		}
	} else {
		while(Size >= Chunk) {
			const __m256i V = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(Src));
			_mm256_storeu_si256(reinterpret_cast<__m256i *>(Dst), V);
			Src += Chunk;
			Dst += Chunk;
			Size -= Chunk;
		}
	}
#elif defined(__ARM_FEATURE_SVE)
	(void)Aligned;
	while(Size >= static_cast<std::size_t>(svcntb())) {
		const std::size_t Chunk = static_cast<std::size_t>(svcntb());
		svbool_t Pg = svptrue_b8();
		svuint8_t V = svld1_u8(Pg, reinterpret_cast<const uint8_t *>(Src));
		svst1_u8(Pg, reinterpret_cast<uint8_t *>(Dst), V);
		Src += Chunk;
		Dst += Chunk;
		Size -= Chunk;
	}
#elif defined(__ARM_NEON) || defined(__aarch64__)
	(void)Aligned;
	constexpr std::size_t Chunk = 16;
	while(Size >= 32) {
		const uint8x16_t V0 = vld1q_u8(reinterpret_cast<const uint8_t *>(Src));
		const uint8x16_t V1 = vld1q_u8(reinterpret_cast<const uint8_t *>(Src + 16));
		vst1q_u8(reinterpret_cast<uint8_t *>(Dst), V0);
		vst1q_u8(reinterpret_cast<uint8_t *>(Dst + 16), V1);
		Src += 32;
		Dst += 32;
		Size -= 32;
	}
	while(Size >= Chunk) {
		const uint8x16_t V = vld1q_u8(reinterpret_cast<const uint8_t *>(Src));
		vst1q_u8(reinterpret_cast<uint8_t *>(Dst), V);
		Src += Chunk;
		Dst += Chunk;
		Size -= Chunk;
	}
#elif defined(__SSE4_2__) || defined(__SSE4_1__) || defined(__SSE2__)
	constexpr std::size_t Chunk = 16;
	if(Aligned) {
		while(Size >= Chunk) {
			const __m128i V = _mm_load_si128(reinterpret_cast<const __m128i *>(Src));
			_mm_store_si128(reinterpret_cast<__m128i *>(Dst), V);
			Src += Chunk;
			Dst += Chunk;
			Size -= Chunk;
		}
	} else {
		while(Size >= Chunk) {
			const __m128i V = _mm_loadu_si128(reinterpret_cast<const __m128i *>(Src));
			_mm_storeu_si128(reinterpret_cast<__m128i *>(Dst), V);
			Src += Chunk;
			Dst += Chunk;
			Size -= Chunk;
		}
	}
#endif
	if(Size > 0)
		std::memcpy(Dst, Src, Size);
}

void FillBytes(char *Dst, const std::uint8_t Value, std::size_t Size, [[maybe_unused]] const bool Aligned) {
#if defined(__AVX512F__)
	constexpr std::size_t Chunk = 64;
	const __m512i Pattern = _mm512_set1_epi8(static_cast<char>(Value));
	if(Aligned) {
		while(Size >= Chunk) {
			_mm512_store_si512(reinterpret_cast<void *>(Dst), Pattern);
			Dst += Chunk;
			Size -= Chunk;
		}
	} else {
		while(Size >= Chunk) {
			_mm512_storeu_si512(reinterpret_cast<void *>(Dst), Pattern);
			Dst += Chunk;
			Size -= Chunk;
		}
	}
#elif defined(__AVX2__)
	constexpr std::size_t Chunk = 32;
	const __m256i Pattern = _mm256_set1_epi8(static_cast<char>(Value));
	if(Aligned) {
		while(Size >= Chunk) {
			_mm256_store_si256(reinterpret_cast<__m256i *>(Dst), Pattern);
			Dst += Chunk;
			Size -= Chunk;
		}
	} else {
		while(Size >= Chunk) {
			_mm256_storeu_si256(reinterpret_cast<__m256i *>(Dst), Pattern);
			Dst += Chunk;
			Size -= Chunk;
		}
	}
#elif defined(__ARM_FEATURE_SVE)
	while(Size >= static_cast<std::size_t>(svcntb())) {
		const std::size_t Chunk = static_cast<std::size_t>(svcntb());
		svbool_t Pg = svptrue_b8();
		svuint8_t Pattern = svdup_u8(Value);
		svst1_u8(Pg, reinterpret_cast<uint8_t *>(Dst), Pattern);
		Dst += Chunk;
		Size -= Chunk;
	}
#elif defined(__ARM_NEON) || defined(__aarch64__)
	constexpr std::size_t Chunk = 16;
	const uint8x16_t Pattern = vdupq_n_u8(Value);
	while(Size >= 32) {
		vst1q_u8(reinterpret_cast<uint8_t *>(Dst), Pattern);
		vst1q_u8(reinterpret_cast<uint8_t *>(Dst + 16), Pattern);
		Dst += 32;
		Size -= 32;
	}
	while(Size >= Chunk) {
		vst1q_u8(reinterpret_cast<uint8_t *>(Dst), Pattern);
		Dst += Chunk;
		Size -= Chunk;
	}
#elif defined(__SSE4_2__) || defined(__SSE4_1__) || defined(__SSE2__)
	constexpr std::size_t Chunk = 16;
	const __m128i Pattern = _mm_set1_epi8(static_cast<char>(Value));
	if(Aligned) {
		while(Size >= Chunk) {
			_mm_store_si128(reinterpret_cast<__m128i *>(Dst), Pattern);
			Dst += Chunk;
			Size -= Chunk;
		}
	} else {
		while(Size >= Chunk) {
			_mm_storeu_si128(reinterpret_cast<__m128i *>(Dst), Pattern);
			Dst += Chunk;
			Size -= Chunk;
		}
	}
#endif
	if(Size > 0)
		std::memset(Dst, static_cast<int>(Value), Size);
}

} // namespace

void Memcpy(void *Dst, const void *Src, const std::size_t Size) {
	if(Size == 0)
		return;
	auto *D = static_cast<char *>(Dst);
	auto *S = static_cast<const char *>(Src);
	const std::size_t Chunk = MemChunkBytes();
	CopyBytes(D, S, Size, PtrAlignedTo(D, Chunk) && PtrAlignedTo(S, Chunk));
}

void MemcpyAligned(void *Dst, const void *Src, const std::size_t Size) {
	if(Size == 0)
		return;
	CopyBytes(static_cast<char *>(Dst), static_cast<const char *>(Src), Size, true);
}

bool MemcpySafeAt(void *Dst, const std::size_t DstOffset, const std::size_t DstCap, const void *Src,
                  const std::size_t SrcOffset, const std::size_t SrcCap, const std::size_t Size) noexcept {
	if(Size == 0)
		return true;
	if(!Dst || !Src)
		return false;
	if(!MemoryGuard::BufferRangeFits(DstOffset, Size, DstCap))
		return false;
	if(!MemoryGuard::BufferRangeFits(SrcOffset, Size, SrcCap))
		return false;
	Memcpy(static_cast<char *>(Dst) + DstOffset, static_cast<const char *>(Src) + SrcOffset, Size);
	return true;
}

bool MemcpySafe(void *Dst, const std::size_t DstCap, const void *Src, const std::size_t SrcCap,
                const std::size_t Size) noexcept {
	return MemcpySafeAt(Dst, 0, DstCap, Src, 0, SrcCap, Size);
}

void Memset(void *Dst, const std::uint8_t Value, const std::size_t Size) {
	if(Size == 0)
		return;
	const std::size_t Chunk = MemChunkBytes();
	FillBytes(static_cast<char *>(Dst), Value, Size, PtrAlignedTo(Dst, Chunk));
}

void MemsetAligned(void *Dst, const std::uint8_t Value, const std::size_t Size) {
	if(Size == 0)
		return;
	FillBytes(static_cast<char *>(Dst), Value, Size, true);
}

bool MemsetSafeAt(void *Dst, const std::size_t DstOffset, const std::size_t DstCap, const std::uint8_t Value,
                  const std::size_t Size) noexcept {
	if(Size == 0)
		return true;
	if(!Dst)
		return false;
	if(!MemoryGuard::BufferRangeFits(DstOffset, Size, DstCap))
		return false;
	Memset(static_cast<char *>(Dst) + DstOffset, Value, Size);
	return true;
}

bool MemsetSafe(void *Dst, const std::size_t DstCap, const std::uint8_t Value, const std::size_t Size) noexcept {
	return MemsetSafeAt(Dst, 0, DstCap, Value, Size);
}

const char *ActiveArchLabel() noexcept {
	switch(DetectArch()) {
	case Arch::Avx512:
		return "avx512";
	case Arch::Avx2:
		return "avx2";
	case Arch::Sse42:
		return "sse42";
	case Arch::Sse41:
		return "sse41";
	case Arch::Sse2:
		return "sse2";
	case Arch::Sve:
		return "sve";
	case Arch::Neon:
		return "neon";
	case Arch::Riscv:
		return "riscv";
	default:
		return "scalar";
	}
}

double SumF64(const double *A, const size_t Count) {
	if(!A || Count == 0)
		return 0.0;
#if defined(__ARM_FEATURE_SVE)
	if(Count >= static_cast<size_t>(svcntd()) * 2)
		return SumKernelSveF64(A, Count);
#endif
#ifdef ASTRALDB_HAS_SIMD_F64
	return SumKernelF64(A, Count);
#else
	double Acc = 0.0;
	for(size_t I = 0; I < Count; ++I)
		Acc += A[I];
	return Acc;
#endif
}

double DotProductF64(const double *A, const double *B, const size_t Count) {
	if(!A || !B || Count == 0)
		return 0.0;
#ifdef ASTRALDB_HAS_SIMD_F64
	return DotKernelF64(A, B, Count);
#else
	double Acc = 0.0;
	for(size_t I = 0; I < Count; ++I)
		Acc += A[I] * B[I];
	return Acc;
#endif
}

double MinF64(const double *A, const size_t Count) {
	if(!A || Count == 0)
		return 0.0;
#ifdef ASTRALDB_HAS_SIMD_F64
	return MinKernelF64(A, Count);
#else
	double Acc = A[0];
	for(size_t I = 1; I < Count; ++I)
		Acc = std::min(Acc, A[I]);
	return Acc;
#endif
}

double MaxF64(const double *A, const size_t Count) {
	if(!A || Count == 0)
		return 0.0;
#ifdef ASTRALDB_HAS_SIMD_F64
	return MaxKernelF64(A, Count);
#else
	double Acc = A[0];
	for(size_t I = 1; I < Count; ++I)
		Acc = std::max(Acc, A[I]);
	return Acc;
#endif
}

void AddF64(double *Dst, const double *A, const double *B, const size_t Count) {
	if(!Dst || !A || !B || Count == 0)
		return;
#ifdef ASTRALDB_HAS_SIMD_F64
	AddKernelF64(Dst, A, B, Count);
#else
	for(size_t I = 0; I < Count; ++I)
		Dst[I] = A[I] + B[I];
#endif
}

void ScaleF64(double *Dst, const double *A, const double Scalar, const size_t Count) {
	if(!Dst || !A || Count == 0)
		return;
#ifdef ASTRALDB_HAS_SIMD_F64
	ScaleKernelF64(Dst, A, Scalar, Count);
#else
	for(size_t I = 0; I < Count; ++I)
		Dst[I] = A[I] * Scalar;
#endif
}

int64_t SumI64(const int64_t *A, const size_t Count) {
	if(!A || Count == 0)
		return 0;
#if defined(__ARM_FEATURE_SVE)
	if(Count >= static_cast<size_t>(svcntd()) * 2)
		return SumKernelSveI64(A, Count);
#endif
#if defined(__AVX512F__) || defined(__AVX2__) || defined(__ARM_NEON) || defined(__aarch64__)
	return SumKernelI64(A, Count);
#else
	int64_t Acc = 0;
	for(size_t I = 0; I < Count; ++I)
		Acc += A[I];
	return Acc;
#endif
}

int64_t MinI64(const int64_t *A, const size_t Count) {
	if(!A || Count == 0)
		return 0;
#if defined(__AVX512F__) || defined(__ARM_NEON) || defined(__aarch64__)
	return MinKernelI64(A, Count);
#else
	int64_t Acc = A[0];
	for(size_t I = 1; I < Count; ++I)
		Acc = std::min(Acc, A[I]);
	return Acc;
#endif
}

int64_t MaxI64(const int64_t *A, const size_t Count) {
	if(!A || Count == 0)
		return 0;
#if defined(__AVX512F__) || defined(__ARM_NEON) || defined(__aarch64__)
	return MaxKernelI64(A, Count);
#else
	int64_t Acc = A[0];
	for(size_t I = 1; I < Count; ++I)
		Acc = std::max(Acc, A[I]);
	return Acc;
#endif
}

} // namespace Simd
} // namespace AstralDB
