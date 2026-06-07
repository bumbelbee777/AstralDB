#include <Database/MathSci/MathSciPrimitiveMicrokernels.hxx>

#include <IO/SIMD.hxx>

#include <algorithm>
#include <cmath>

namespace AstralDB {
namespace MathSciPrimitiveMicrokernels {

namespace {

inline std::int8_t ClampI8(int V) {
	return static_cast<std::int8_t>(std::clamp(V, -127, 127));
}

inline int ClampI4(int V) { return std::clamp(V, -7, 7); }

inline int NibbleFromSigned(int V) {
	const int C = ClampI4(V);
	return C < 0 ? (C + 16) : C;
}

inline int SignedFromNibble(int N) {
	N &= 0xF;
	return N >= 8 ? N - 16 : N;
}

} // namespace

float QuantizeScaleSymF32(const float *Values, std::size_t Count) noexcept {
	if(!Values || Count == 0)
		return 1.f;
	float MaxAbs = 0.f;
	for(std::size_t I = 0; I < Count; ++I)
		MaxAbs = std::max(MaxAbs, std::abs(Values[I]));
	if(MaxAbs <= 0.f)
		return 1.f;
	return MaxAbs / 127.f;
}

float QuantizeScaleSymI4F32(const float *Values, std::size_t Count) noexcept {
	if(!Values || Count == 0)
		return 1.f;
	float MaxAbs = 0.f;
	for(std::size_t I = 0; I < Count; ++I)
		MaxAbs = std::max(MaxAbs, std::abs(Values[I]));
	if(MaxAbs <= 0.f)
		return 1.f;
	return MaxAbs / 7.f;
}

void QuantizeSymF32ToI8(const float *In, std::int8_t *Out, std::size_t Count, float Scale) noexcept {
	if(!In || !Out || Count == 0)
		return;
	if(Scale <= 0.f) {
		for(std::size_t I = 0; I < Count; ++I)
			Out[I] = 0;
		return;
	}
	const float Inv = 1.f / Scale;
	for(std::size_t I = 0; I < Count; ++I)
		Out[I] = ClampI8(static_cast<int>(std::lround(In[I] * Inv)));
}

void DequantizeI8ToF32(const std::int8_t *In, float *Out, std::size_t Count, float Scale) noexcept {
	if(!In || !Out || Count == 0)
		return;
	for(std::size_t I = 0; I < Count; ++I)
		Out[I] = static_cast<float>(In[I]) * Scale;
}

void QuantizeSymF32ToI4Packed(const float *In, std::uint8_t *Out, std::size_t Count, float Scale) noexcept {
	if(!In || !Out || Count == 0)
		return;
	const float Inv = Scale > 0.f ? 1.f / Scale : 0.f;
	std::size_t O = 0;
	std::size_t I = 0;
	for(; I + 1 < Count; I += 2) {
		const int Lo = NibbleFromSigned(static_cast<int>(std::lround(In[I] * Inv)));
		const int Hi = NibbleFromSigned(static_cast<int>(std::lround(In[I + 1] * Inv)));
		Out[O++] = static_cast<std::uint8_t>((Hi << 4) | (Lo & 0xF));
	}
	if(I < Count) {
		const int Lo = NibbleFromSigned(static_cast<int>(std::lround(In[I] * Inv)));
		Out[O++] = static_cast<std::uint8_t>(Lo & 0xF);
	}
}

void DequantizeI4PackedToF32(const std::uint8_t *In, float *Out, std::size_t Count, float Scale) noexcept {
	if(!In || !Out || Count == 0)
		return;
	std::size_t B = 0;
	std::size_t O = 0;
	while(O < Count) {
		const std::uint8_t Byte = In[B++];
		Out[O++] = static_cast<float>(SignedFromNibble(Byte & 0xF)) * Scale;
		if(O < Count)
			Out[O++] = static_cast<float>(SignedFromNibble(Byte >> 4)) * Scale;
	}
}

float DotProductF32(const float *A, const float *B, std::size_t Count) noexcept {
	return Simd::DotProductF32(A, B, Count);
}

float SumF32(const float *Values, std::size_t Count) noexcept {
	return Simd::SumF32(Values, Count);
}

} // namespace MathSciPrimitiveMicrokernels
} // namespace AstralDB
