#include <DS/RadixPartition.hxx>

namespace AstralDB {

RadixPartition::RadixPartition(unsigned Bits) : Bits_(Bits), Mask_((1u << Bits) - 1u) {}

void RadixPartition::Assign(const uint64_t *Hashes, std::size_t Count, std::vector<std::size_t> &OutPartition) const {
	OutPartition.resize(Count);
	for(std::size_t I = 0; I < Count; ++I)
		OutPartition[I] = static_cast<std::size_t>(Hashes[I] & Mask_);
}

} // namespace AstralDB
