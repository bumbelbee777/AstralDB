#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__AVX512F__)
#include <immintrin.h>
#elif defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSE4_2__)
#include <nmmintrin.h>
#elif defined(__SSE4_1__)
#include <smmintrin.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif
#if defined(__ARM_FEATURE_SVE)
#include <arm_sve.h>
#endif

namespace AstralDB {
namespace Simd {

enum class Arch : int8_t {
	Scalar = 0,
	Sse2 = 1,
	Sse41 = 2,
	Sse42 = 3,
	Avx2 = 4,
	Avx512 = 5,
	Neon = 6,
	Sve = 7,
	Riscv = 8
};

struct KernelConfig {
	size_t VectorWidthF32;
	size_t DotUnroll;
	size_t GemvMr;
	size_t GemvKc;
};

constexpr Arch DetectArch() {
#if defined(__AVX512F__)
	return Arch::Avx512;
#elif defined(__AVX2__)
	return Arch::Avx2;
#elif defined(__SSE4_2__)
	return Arch::Sse42;
#elif defined(__SSE4_1__)
	return Arch::Sse41;
#elif defined(__ARM_FEATURE_SVE)
	return Arch::Sve;
#elif defined(__ARM_NEON) || defined(__aarch64__)
	return Arch::Neon;
#elif defined(__riscv)
	return Arch::Riscv;
#elif defined(__SSE2__)
	return Arch::Sse2;
#else
	return Arch::Scalar;
#endif
}

/** Human-readable label for \c DetectArch() (e.g. \c "avx512", \c "sve"). */
const char *ActiveArchLabel() noexcept;

/** Natural SIMD vector width for \c Memcpy / \c Memset (64 on AVX-512, 32 AVX2, 16 SSE/NEON). */
inline std::size_t MemVectorBytes() noexcept {
#if defined(__ARM_FEATURE_SVE)
	return static_cast<std::size_t>(svcntb());
#elif defined(__AVX512F__)
	return 64;
#elif defined(__AVX2__)
	return 32;
#elif defined(__ARM_NEON) || defined(__aarch64__) || defined(__SSE4_2__) || defined(__SSE4_1__) || defined(__SSE2__)
	return 16;
#else
	return 8;
#endif
}

[[nodiscard]] inline bool IsMemOpsAligned(const void *Ptr, std::size_t Alignment = MemVectorBytes()) noexcept {
	const auto Addr = reinterpret_cast<std::uintptr_t>(Ptr);
	return Alignment > 0 && (Addr & (Alignment - 1)) == 0;
}

#if defined(__ARM_NEON) || defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
/** 16-bit movemask from \c vceqq_u8-style lanes (0xFF = match). */
inline std::uint32_t NeonMovemaskEq(uint8x16_t Cmp) noexcept {
	static const std::uint8_t LaneMask[16] = {1,  2,  4,  8, 16, 32, 64, 128,
	                                          1,  2,  4,  8, 16, 32, 64, 128};
	const uint8x16_t Mask = vld1q_u8(LaneMask);
	const uint8x16_t And = vandq_u8(Cmp, Mask);
	const uint8x8_t Sum8 = vpadd_u8(vget_low_u8(And), vget_high_u8(And));
	const uint8x8_t Sum4 = vpadd_u8(Sum8, Sum8);
	const uint8x8_t Sum2 = vpadd_u8(Sum4, Sum4);
	return static_cast<std::uint32_t>(vget_lane_u8(Sum2, 0)) |
	       (static_cast<std::uint32_t>(vget_lane_u8(Sum2, 1)) << 8);
}

inline bool NeonAllEq(uint8x16_t Cmp) noexcept { return vminvq_u8(Cmp) == 0xFF; }
#endif

KernelConfig ActiveKernelConfig();

/** Fast copy; uses aligned loads/stores when both pointers are vector-aligned. */
void Memcpy(void *Dst, const void *Src, std::size_t Size);

/** Copy when \p Dst and \p Src are aligned to \c MemVectorBytes(). */
void MemcpyAligned(void *Dst, const void *Src, std::size_t Size);

/** Bounds-checked copy into \p DstCap / from \p SrcCap; returns false without writing on violation. */
[[nodiscard]] bool MemcpySafe(void *Dst, std::size_t DstCap, const void *Src, std::size_t SrcCap,
                              std::size_t Size) noexcept;

/** Bounds-checked copy at byte offsets. */
[[nodiscard]] bool MemcpySafeAt(void *Dst, std::size_t DstOffset, std::size_t DstCap, const void *Src,
                                std::size_t SrcOffset, std::size_t SrcCap, std::size_t Size) noexcept;

/** Fast fill; uses aligned stores when \p Dst is vector-aligned. */
void Memset(void *Dst, std::uint8_t Value, std::size_t Size);

/** Fill when \p Dst is aligned to \c MemVectorBytes(). */
void MemsetAligned(void *Dst, std::uint8_t Value, std::size_t Size);

/** Bounds-checked fill; returns false without writing on violation. */
[[nodiscard]] bool MemsetSafe(void *Dst, std::size_t DstCap, std::uint8_t Value, std::size_t Size) noexcept;

[[nodiscard]] bool MemsetSafeAt(void *Dst, std::size_t DstOffset, std::size_t DstCap, std::uint8_t Value,
                                std::size_t Size) noexcept;

float DotProductF32(const float *A, const float *B, size_t Count);
float L2SquaredF32(const float *A, const float *B, size_t Count);
float SumF32(const float *A, size_t Count);
float MaxF32(const float *A, size_t Count);
float MinF32(const float *A, size_t Count);
void AddF32(float *Dst, const float *A, const float *B, size_t Count);
void SubF32(float *Dst, const float *A, const float *B, size_t Count);
void MulF32(float *Dst, const float *A, const float *B, size_t Count);
void ScaleF32(float *Dst, const float *A, float Scalar, size_t Count);
void MulAccumulateF32(float *Dst, const float *A, float Scalar, size_t Count);
void ReluF32(float *Dst, const float *A, size_t Count);
void ComplexMulF32(const float *ARe, const float *AIm, const float *BRe, const float *BIm, float *OutRe,
                   float *OutIm);
void ComplexMulAccumulateF32(const float *ARe, const float *AIm, const float *BRe, const float *BIm, float *OutRe,
                             float *OutIm, size_t Count);
void ComplexDotHermitianInterleavedF32(const float *A, const float *B, size_t Slots, float &OutRe, float &OutIm);
void ComplexAddInterleavedF32(float *Dst, const float *A, const float *B, size_t Slots);
void ComplexScaleInterleavedF32(float *Dst, const float *A, float ScaleRe, float ScaleIm, size_t Slots);
void ComplexNormSqInterleavedF32(const float *A, size_t Slots, float &OutRe, float &OutIm);
void MatrixVectorMulF32(const float *MatrixRowMajor, const float *Vector, float *Out, size_t Rows, size_t Cols);
void ComplexMatVecInterleavedF32(const float *Mat, const float *Vec, float *Out, size_t Rows, size_t Cols);

double SumF64(const double *A, size_t Count);
double DotProductF64(const double *A, const double *B, size_t Count);
double MinF64(const double *A, size_t Count);
double MaxF64(const double *A, size_t Count);
void AddF64(double *Dst, const double *A, const double *B, size_t Count);
void ScaleF64(double *Dst, const double *A, double Scalar, size_t Count);

int64_t SumI64(const int64_t *A, size_t Count);
int64_t MinI64(const int64_t *A, size_t Count);
int64_t MaxI64(const int64_t *A, size_t Count);

} // namespace Simd

inline void SimdMemcpy(void *Dst, const void *Src, size_t Size) { Simd::Memcpy(Dst, Src, Size); }
inline void SimdMemset(void *Dst, uint8_t Value, size_t Size) { Simd::Memset(Dst, Value, Size); }

} // namespace AstralDB
