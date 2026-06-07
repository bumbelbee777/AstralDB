#include <IO/RuntimeLimits.hxx>
#include <IO/Limits.hxx>
#include <IO/MemoryGuard.hxx>

#include <cstdlib>

namespace AstralDB {

namespace {

const char *EnvGet(const char *Name) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
	return std::getenv(Name);
#pragma warning(pop)
#else
	return std::getenv(Name);
#endif
}

std::uint64_t EnvU64(const char *Name, std::uint64_t Default) {
	if(const char *V = EnvGet(Name)) {
		char *End = nullptr;
		const unsigned long long N = std::strtoull(V, &End, 10);
		if(End != V && N > 0)
			return static_cast<std::uint64_t>(N);
	}
	return Default;
}

} // namespace

std::uint64_t RuntimeLimits::MaxBulkInsertRowsBench() noexcept {
	return EnvU64("ASTRALDB_MAX_BULK_ROWS", Limits::MaxBulkInsertRowsBench);
}

std::uint64_t RuntimeLimits::EffectiveMaxBulkInsertRows(const bool EphemeralDbPath) noexcept {
	if(EphemeralDbPath || EnvGet("ASTRALDB_MAX_BULK_ROWS") != nullptr)
		return MaxBulkInsertRowsBench();
	return Limits::MaxBulkInsertRows;
}

bool RuntimeLimits::BulkBenchScaleEnabled(const bool EphemeralDbPath) noexcept {
	return EphemeralDbPath || EnvGet("ASTRALDB_MAX_BULK_ROWS") != nullptr;
}

std::size_t RuntimeLimits::HardSessionCapBytes() noexcept {
	if(const char *Gb = EnvGet("ASTRALDB_MEMORY_CAP_GB")) {
		char *End = nullptr;
		const double G = std::strtod(Gb, &End);
		if(End != Gb && G > 0.)
			return static_cast<std::size_t>(G * 1024. * 1024. * 1024.);
	}
	if(EnvGet("ASTRALDB_MEMORY_CAP_BYTES"))
		return static_cast<std::size_t>(EnvU64("ASTRALDB_MEMORY_CAP_BYTES", MemoryGuard::HardSessionCapBytes));
	return MemoryGuard::HardSessionCapBytes;
}

} // namespace AstralDB
