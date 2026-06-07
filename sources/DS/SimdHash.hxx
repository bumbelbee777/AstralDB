#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace AstralDB {

/** CRC32C / group-key hashing helpers (SSE4.2 when available). */
struct SimdHash {
	static uint64_t Hash64(std::string_view Key) noexcept;
	static void Hash64Batch(const char *const *Keys, std::size_t *Lens, std::size_t Count, uint64_t *Out) noexcept;
};

} // namespace AstralDB
