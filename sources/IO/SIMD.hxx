#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace AstralDB {
namespace Simd {

enum class Arch : int8_t { Scalar = 0, Sse2 = 1, Avx2 = 2, Neon = 3, Riscv = 4 };

constexpr Arch DetectArch() {
#if defined(__AVX2__)
	return Arch::Avx2;
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

inline void Memcpy(void *Dst, const void *Src, size_t Size) {
#if defined(__AVX2__)
	char *DstPtr = static_cast<char *>(Dst);
	const char *SrcPtr = static_cast<const char *>(Src);
	while(Size >= 32) {
		__m256i Data = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(SrcPtr));
		_mm256_storeu_si256(reinterpret_cast<__m256i *>(DstPtr), Data);
		SrcPtr += 32;
		DstPtr += 32;
		Size -= 32;
	}
	while(Size--)
		*DstPtr++ = *SrcPtr++;
#elif defined(__SSE2__)
	char *DstPtr = static_cast<char *>(Dst);
	const char *SrcPtr = static_cast<const char *>(Src);
	while(Size >= 16) {
		__m128i Data = _mm_loadu_si128(reinterpret_cast<const __m128i *>(SrcPtr));
		_mm_storeu_si128(reinterpret_cast<__m128i *>(DstPtr), Data);
		SrcPtr += 16;
		DstPtr += 16;
		Size -= 16;
	}
	while(Size--)
		*DstPtr++ = *SrcPtr++;
#else
	std::memcpy(Dst, Src, Size);
#endif
}

inline void Memset(void *Dst, uint8_t Value, size_t Size) {
#if defined(__AVX2__)
	char *DstPtr = static_cast<char *>(Dst);
	__m256i Pattern = _mm256_set1_epi8(static_cast<char>(Value));
	while(Size >= 32) {
		_mm256_storeu_si256(reinterpret_cast<__m256i *>(DstPtr), Pattern);
		DstPtr += 32;
		Size -= 32;
	}
	while(Size--)
		*DstPtr++ = static_cast<char>(Value);
#elif defined(__SSE2__)
	char *DstPtr = static_cast<char *>(Dst);
	__m128i Pattern = _mm_set1_epi8(static_cast<char>(Value));
	while(Size >= 16) {
		_mm_storeu_si128(reinterpret_cast<__m128i *>(DstPtr), Pattern);
		DstPtr += 16;
		Size -= 16;
	}
	while(Size--)
		*DstPtr++ = static_cast<char>(Value);
#else
	std::memset(Dst, Value, Size);
#endif
}

float DotProductF32(const float *A, const float *B, size_t Count);
void AddF32(float *Dst, const float *A, const float *B, size_t Count);
void SubF32(float *Dst, const float *A, const float *B, size_t Count);
void MulF32(float *Dst, const float *A, const float *B, size_t Count);
void ScaleF32(float *Dst, const float *A, float Scalar, size_t Count);
void MulAccumulateF32(float *Dst, const float *A, float Scalar, size_t Count);
void ComplexMulF32(const float *ARe, const float *AIm, const float *BRe, const float *BIm, float *OutRe,
                   float *OutIm);
/** \p OutRe/OutIm += A ⊙ B in the complex sense, length \p Count. */
void ComplexMulAccumulateF32(const float *ARe, const float *AIm, const float *BRe, const float *BIm, float *OutRe,
                             float *OutIm, size_t Count);
void MatrixVectorMulF32(const float *MatrixRowMajor, const float *Vector, float *Out, size_t Rows, size_t Cols);

} // namespace Simd

inline void SimdMemcpy(void *Dst, const void *Src, size_t Size) { Simd::Memcpy(Dst, Src, Size); }
inline void SimdMemset(void *Dst, uint8_t Value, size_t Size) { Simd::Memset(Dst, Value, Size); }

} // namespace AstralDB
