#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace AstralDB {

/** Radix partition assignment for parallel GROUP BY. */
class RadixPartition {
public:
	explicit RadixPartition(unsigned Bits = 8);

	void Assign(const uint64_t *Hashes, std::size_t Count, std::vector<std::size_t> &OutPartition) const;
	[[nodiscard]] unsigned PartitionCount() const noexcept { return 1u << Bits_; }

private:
	unsigned Bits_;
	unsigned Mask_;
};

} // namespace AstralDB
