#pragma once

#include <cstddef>
#include <cstdint>

namespace AstralDB {

/** Environment-overridable session limits (bulk caps, memory). */
struct RuntimeLimits {
	[[nodiscard]] static std::uint64_t MaxBulkInsertRowsBench() noexcept;
	/** Ephemeral session paths or \c ASTRALDB_MAX_BULK_ROWS use the bench/env cap; else production default. */
	[[nodiscard]] static std::uint64_t EffectiveMaxBulkInsertRows(bool EphemeralDbPath) noexcept;
	/** Lazy bulk megafusion + spill paths (same as \c -m session benchmarks when env cap is set). */
	[[nodiscard]] static bool BulkBenchScaleEnabled(bool EphemeralDbPath) noexcept;
	[[nodiscard]] static std::size_t HardSessionCapBytes() noexcept;
};

} // namespace AstralDB
