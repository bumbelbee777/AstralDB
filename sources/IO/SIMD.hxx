#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(__cpp_lib_experimental_simd)
#include <experimental/simd>
using namespace std::experimental;
#endif

namespace AstralDB {
//Generic SIMD routines and helpers

// Fast memory copy using SIMD (fallbacks to std::memcpy if not available)
inline void SimdMemcpy(void *Dst, const void *Src, size_t Size) {
#if defined(__AVX2__)
	char *DstPtr = static_cast<char*>(Dst);
	const char *SrcPtr = static_cast<const char*>(Src);
	while(Size >= 32) {
		__m256i Data = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(SrcPtr));
		_mm256_storeu_si256(reinterpret_cast<__m256i*>(DstPtr), Data);
		SrcPtr += 32;
		DstPtr += 32;
		Size -= 32;
	}
	while(Size--) *DstPtr++ = *SrcPtr++;
#elif defined(__SSE2__)
	char *DstPtr = static_cast<char*>(Dst);
	const char *SrcPtr = static_cast<const char*>(Src);
	while(Size >= 16) {
		__m128i Data = _mm_loadu_si128(reinterpret_cast<const __m128i*>(SrcPtr));
		_mm_storeu_si128(reinterpret_cast<__m128i*>(DstPtr), Data);
		SrcPtr += 16;
		DstPtr += 16;
		Size -= 16;
	}
	while(Size--) *DstPtr++ = *SrcPtr++;
#else
	std::memcpy(Dst, Src, Size);
#endif
}

// Fast memory set using SIMD (fallbacks to std::memset if not available)
inline void SimdMemset(void *Dst, uint8_t Value, size_t Size) {
#if defined(__AVX2__)
	char *DstPtr = static_cast<char*>(Dst);
	__m256i Pattern = _mm256_set1_epi8(Value);
	while(Size >= 32) {
		_mm256_storeu_si256(reinterpret_cast<__m256i*>(DstPtr), Pattern);
		DstPtr += 32;
		Size -= 32;
	}
	while(Size--) *DstPtr++ = Value;
#elif defined(__SSE2__)
	char *DstPtr = static_cast<char*>(Dst);
	__m128i Pattern = _mm_set1_epi8(Value);
	while(Size >= 16) {
		_mm_storeu_si128(reinterpret_cast<__m128i*>(DstPtr), Pattern);
		DstPtr += 16;
		Size -= 16;
	}
	while(Size--) *DstPtr++ = Value;
#else
	std::memset(Dst, Value, Size);
#endif
}

// Vectorized addition for float arrays (in-place) using software SIMD (loop unrolling)
inline void SimdAdd(float *Dst, const float *Src, size_t Count) {
    size_t i = 0;
    // Process 4 floats at a time (mimics 128-bit SIMD)
    for (; i + 3 < Count; i += 4) {
        Dst[i]     += Src[i];
        Dst[i + 1] += Src[i + 1];
        Dst[i + 2] += Src[i + 2];
        Dst[i + 3] += Src[i + 3];
    }
    // Handle remaining elements
    for (; i < Count; ++i) {
        Dst[i] += Src[i];
    }
}

// Vectorized multiplication for float arrays (in-place) using software SIMD (loop unrolling)
inline void SimdMul(float *Dst, const float *Src, size_t Count) {
    size_t i = 0;
    // Process 4 floats at a time (mimics 128-bit SIMD)
    for (; i + 3 < Count; i += 4) {
        Dst[i]     *= Src[i];
        Dst[i + 1] *= Src[i + 1];
        Dst[i + 2] *= Src[i + 2];
        Dst[i + 3] *= Src[i + 3];
    }
    // Handle remaining elements
    for (; i < Count; ++i) {
        Dst[i] *= Src[i];
    }
}

}