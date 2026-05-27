#pragma once

#include <cstddef>
#include <cstdint>

namespace AstralDB {

/** Spike-aware OOM guards: zero overhead until a large allocation or rapid growth is observed. */
struct MemoryGuard {
	static constexpr std::size_t SoftDisableBelowBytes = 48ULL * 1024ULL * 1024ULL;
	static constexpr std::size_t SpikeSingleAllocBytes = 8ULL * 1024ULL * 1024ULL;
	static constexpr std::size_t SpikeGrowthBytes = 32ULL * 1024ULL * 1024ULL;
	static constexpr std::size_t HardSessionCapBytes = 512ULL * 1024ULL * 1024ULL;

	/** Fast path: false → skip accounting on small steady-state work. */
	static bool GuardsActive() noexcept;

	/** Record growth; may enable guards when a spike is detected. */
	static void NoteAlloc(std::size_t Bytes) noexcept;

	/** Release accounted bytes (clamped at zero). */
	static void ReleaseBytes(std::size_t Bytes) noexcept;

	/** When guards are active, reject allocations that would exceed the hard cap. */
	static bool AllowAlloc(std::size_t Bytes);

	/** Bounds check for index/size pairs; returns false when \p Index >= \p Size or overflow would occur. */
	static bool SecureBoundsCheck(std::size_t Index, std::size_t Size, std::size_t ElementBytes = 1) noexcept;

	/** Reset session accounting (tests and post-VACUUM). */
	static void ResetSession() noexcept;

	/** Current estimated session bytes (for diagnostics/tests). */
	static std::size_t EstimatedSessionBytes() noexcept;

	/** RAII: treat nested bulk work as spike-tolerant (always account, never soft-disable). */
	struct SpikeScope {
		SpikeScope() noexcept;
		~SpikeScope();
		SpikeScope(const SpikeScope &) = delete;
		SpikeScope &operator=(const SpikeScope &) = delete;
	};
};

} // namespace AstralDB
