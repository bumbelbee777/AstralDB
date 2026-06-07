#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <vector>

namespace AstralDB {

/** Tracks ephemeral shard directories and purges expired entries. */
struct ColumnShardRegistry {
	static void RegisterDirectory(const std::filesystem::path &Root, std::uint64_t TtlSeconds);
	static void PurgeExpired() noexcept;
	static void PurgeAllRegistered() noexcept;
	static std::size_t RegisteredCount() noexcept;

private:
	struct Entry {
		std::filesystem::path Path;
		std::uint64_t ExpiresUnixSec = 0;
	};
	static std::mutex &Mutex() noexcept;
	static std::vector<Entry> &Entries() noexcept;
};

} // namespace AstralDB
