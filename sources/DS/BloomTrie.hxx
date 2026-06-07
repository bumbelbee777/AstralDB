#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {

/** Prefix Bloom filter for async shard / column pruning. */
class BloomTrie {
public:
	explicit BloomTrie(std::size_t BitCount = 1u << 16, unsigned HashCount = 4);

	void Insert(std::string_view Key);
	[[nodiscard]] bool MayContain(std::string_view Key) const noexcept;

private:
	[[nodiscard]] std::size_t HashSlot(std::string_view Key, unsigned Seed) const noexcept;

	std::vector<uint64_t> Words_;
	std::size_t BitMask_;
	unsigned HashCount_;
};

} // namespace AstralDB
