#include <DS/BloomTrie.hxx>
#include <DS/SimdHash.hxx>

namespace AstralDB {

BloomTrie::BloomTrie(std::size_t BitCount, unsigned HashCount)
    : Words_((BitCount + 63) / 64, 0), BitMask_(BitCount - 1), HashCount_(HashCount) {}

std::size_t BloomTrie::HashSlot(std::string_view Key, unsigned Seed) const noexcept {
	const uint64_t H = SimdHash::Hash64(Key) ^ (static_cast<uint64_t>(Seed) * 0x9E3779B97F4A7C15ull);
	return static_cast<std::size_t>(H) & BitMask_;
}

void BloomTrie::Insert(std::string_view Key) {
	for(unsigned S = 0; S < HashCount_; ++S) {
		const std::size_t Bit = HashSlot(Key, S);
		Words_[Bit / 64] |= (std::uint64_t{1} << (Bit % 64));
	}
}

bool BloomTrie::MayContain(std::string_view Key) const noexcept {
	for(unsigned S = 0; S < HashCount_; ++S) {
		const std::size_t Bit = HashSlot(Key, S);
		if((Words_[Bit / 64] & (std::uint64_t{1} << (Bit % 64))) == 0)
			return false;
	}
	return true;
}

} // namespace AstralDB
