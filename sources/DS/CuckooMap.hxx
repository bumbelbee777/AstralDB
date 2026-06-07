#pragma once

#include <DS/SimdHash.hxx>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AstralDB {

/** Open-addressing cuckoo hash map for GROUP BY keys. */
class CuckooMap {
public:
	struct Entry {
		std::string Key;
		uint32_t Slot = 0;
		bool Occupied = false;
	};

	explicit CuckooMap(std::size_t CapacityPow2 = 16);

	[[nodiscard]] std::size_t Size() const noexcept { return Size_; }

	uint32_t FindOrInsert(std::string_view Key);
	Entry *Find(std::string_view Key) noexcept;
	const Entry *Find(std::string_view Key) const noexcept;

private:
	[[nodiscard]] std::size_t ProbeIndex(uint64_t Hash, std::size_t Table) const noexcept;
	bool InsertAt(std::size_t Idx, std::string Key, uint32_t Slot);

	std::vector<Entry> Table0_;
	std::vector<Entry> Table1_;
	std::size_t Mask_;
	std::size_t Size_ = 0;
	uint32_t NextSlot_ = 0;
};

} // namespace AstralDB
