#include <DS/SimdHash.hxx>

#include <cstring>

#if defined(__SSE4_2__)
#include <nmmintrin.h>
#endif
#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

namespace AstralDB {

uint64_t SimdHash::Hash64(std::string_view Key) noexcept {
	uint64_t H = 14695981039346656037ull;
	for(unsigned char C : Key) {
		H ^= static_cast<uint64_t>(C);
		H *= 1099511628211ull;
	}
#if defined(__SSE4_2__)
	const char *P = Key.data();
	const std::size_t Len = Key.size();
	uint32_t Fold = static_cast<uint32_t>(H);
	for(std::size_t I = 0; I + 8 <= Len; I += 8) {
		uint64_t W = 0;
		std::memcpy(&W, P + I, sizeof(W));
		Fold = static_cast<uint32_t>(_mm_crc32_u64(Fold, W));
	}
	for(std::size_t I = (Len / 8) * 8; I < Len; ++I)
		Fold = _mm_crc32_u8(Fold, static_cast<unsigned char>(P[I]));
	H ^= static_cast<uint64_t>(Fold) * 0x9E3779B97F4A7C15ull;
#elif defined(__ARM_FEATURE_CRC32)
	const char *P = Key.data();
	const std::size_t Len = Key.size();
	uint32_t Fold = static_cast<uint32_t>(H);
	for(std::size_t I = 0; I + 8 <= Len; I += 8) {
		uint64_t W = 0;
		std::memcpy(&W, P + I, sizeof(W));
		Fold = __crc32cd(Fold, W);
	}
	for(std::size_t I = (Len / 8) * 8; I < Len; ++I)
		Fold = __crc32cb(Fold, static_cast<unsigned char>(P[I]));
	H ^= static_cast<uint64_t>(Fold) * 0x9E3779B97F4A7C15ull;
#endif
	return H;
}

void SimdHash::Hash64Batch(const char *const *Keys, std::size_t *Lens, std::size_t Count, uint64_t *Out) noexcept {
	for(std::size_t I = 0; I < Count; ++I)
		Out[I] = Hash64(std::string_view(Keys[I], Lens[I]));
}

} // namespace AstralDB
