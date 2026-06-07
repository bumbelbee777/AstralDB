#include <DS/CuckooMap.hxx>

#include <algorithm>

namespace AstralDB {

CuckooMap::CuckooMap(std::size_t CapacityPow2) {
	const std::size_t Cap = std::size_t{1} << CapacityPow2;
	Table0_.resize(Cap);
	Table1_.resize(Cap);
	Mask_ = Cap - 1;
}

std::size_t CuckooMap::ProbeIndex(uint64_t Hash, std::size_t Table) const noexcept {
	const uint64_t Mixed = Hash ^ (Table * 0xD6E8FEB86659FD93ull);
	return static_cast<std::size_t>(Mixed) & Mask_;
}

uint32_t CuckooMap::FindOrInsert(std::string_view Key) {
	const uint64_t H = SimdHash::Hash64(Key);
	const std::size_t I0 = ProbeIndex(H, 0);
	if(Table0_[I0].Occupied && Table0_[I0].Key == Key)
		return Table0_[I0].Slot;
	const std::size_t I1 = ProbeIndex(H, 1);
	if(Table1_[I1].Occupied && Table1_[I1].Key == Key)
		return Table1_[I1].Slot;
	const uint32_t Slot = NextSlot_++;
	if(InsertAt(I0, std::string(Key), Slot))
		return Slot;
	if(InsertAt(I1, std::string(Key), Slot))
		return Slot;
	// Fallback: linear probe on table0
	for(std::size_t I = 0; I <= Mask_; ++I) {
		if(!Table0_[I].Occupied) {
			Table0_[I] = {std::string(Key), Slot, true};
			++Size_;
			return Slot;
		}
	}
	return Slot;
}

bool CuckooMap::InsertAt(std::size_t Idx, std::string Key, uint32_t Slot) {
	if(!Table0_[Idx].Occupied) {
		Table0_[Idx] = {std::move(Key), Slot, true};
		++Size_;
		return true;
	}
	return false;
}

CuckooMap::Entry *CuckooMap::Find(std::string_view Key) noexcept {
	const uint64_t H = SimdHash::Hash64(Key);
	const std::size_t I0 = ProbeIndex(H, 0);
	if(Table0_[I0].Occupied && Table0_[I0].Key == Key)
		return &Table0_[I0];
	const std::size_t I1 = ProbeIndex(H, 1);
	if(Table1_[I1].Occupied && Table1_[I1].Key == Key)
		return &Table1_[I1];
	return nullptr;
}

const CuckooMap::Entry *CuckooMap::Find(std::string_view Key) const noexcept {
	return const_cast<CuckooMap *>(this)->Find(Key);
}

} // namespace AstralDB
