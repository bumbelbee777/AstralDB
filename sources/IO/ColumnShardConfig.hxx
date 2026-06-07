#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace AstralDB {

enum class ColumnShardCompression : uint8_t { Off = 0, Lz4 = 1, Lzx = 2 };

/** Tunable bulk shard spill parameters (env + defaults). */
struct ColumnShardConfig {
	std::uint64_t TtlSeconds = 3600;
	std::uint64_t SpillMinRows = 1'000'000;
	ColumnShardCompression Compression = ColumnShardCompression::Lz4;
	unsigned IocpDepth = 4;
	unsigned IocpReadAheadChunks = 2;

	[[nodiscard]] static ColumnShardConfig FromEnvironment() noexcept;
	[[nodiscard]] static ColumnShardCompression ParseCompression(std::string_view Token) noexcept;
	[[nodiscard]] const char *CompressionName() const noexcept;
};

} // namespace AstralDB
