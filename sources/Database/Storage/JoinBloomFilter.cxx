#include <Database/Storage/JoinBloomFilter.hxx>

#include <DS/SimdHash.hxx>

#include <algorithm>

namespace AstralDB {

JoinBloomFilter::JoinBloomFilter(std::size_t ExpectedKeys, unsigned HashCount)
    : HashCount_(HashCount == 0 ? 4u : HashCount) {
	std::size_t Bits = ExpectedKeys * 10;
	if(Bits < 4096)
		Bits = 4096;
	Bits = 1;
	while(Bits < ExpectedKeys * 8)
		Bits <<= 1;
	BitMask_ = Bits - 1;
	Words_.assign((Bits + 63) / 64, 0);
}

void JoinBloomFilter::Clear() { std::fill(Words_.begin(), Words_.end(), 0); }

std::size_t JoinBloomFilter::Slot(std::string_view Key, unsigned Seed) const noexcept {
	if(Seed == 0)
		return static_cast<std::size_t>(SimdHash::Hash64(Key) & BitMask_);
	std::string Mix;
	Mix.reserve(Key.size() + 2);
	Mix.append(Key);
	Mix.push_back(static_cast<char>('0' + static_cast<char>(Seed % 10)));
	return static_cast<std::size_t>(SimdHash::Hash64(Mix) & BitMask_);
}

void JoinBloomFilter::Insert(std::string_view Key) {
	for(unsigned I = 0; I < HashCount_; ++I) {
		const std::size_t Bit = Slot(Key, I);
		Words_[Bit / 64] |= (1ULL << (Bit % 64));
	}
}

bool JoinBloomFilter::MayContain(std::string_view Key) const noexcept {
	for(unsigned I = 0; I < HashCount_; ++I) {
		const std::size_t Bit = Slot(Key, I);
		if((Words_[Bit / 64] & (1ULL << (Bit % 64))) == 0)
			return false;
	}
	return true;
}

} // namespace AstralDB
