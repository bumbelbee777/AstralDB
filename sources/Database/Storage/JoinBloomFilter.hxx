#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {

/** Classic Bloom filter for hash-join build-side semi-join pruning. */
class JoinBloomFilter {
public:
	explicit JoinBloomFilter(std::size_t ExpectedKeys = 65536, unsigned HashCount = 4);

	void Clear();
	void Insert(std::string_view Key);
	[[nodiscard]] bool MayContain(std::string_view Key) const noexcept;

private:
	[[nodiscard]] std::size_t Slot(std::string_view Key, unsigned Seed) const noexcept;

	std::vector<uint64_t> Words_;
	std::size_t BitMask_;
	unsigned HashCount_;
};

} // namespace AstralDB
