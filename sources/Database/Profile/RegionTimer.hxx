#pragma once

#include <IO/EnvUtil.hxx>

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

namespace AstralDB {

using RegionTimingHook = std::function<void(const std::string &, std::chrono::nanoseconds)>;

/** Optional hook for SQL profiler integration; unset by default. */
void SetRegionTimingHook(RegionTimingHook Hook);

void RecordRegionTiming(const std::string &Name, std::chrono::nanoseconds Elapsed);

/** Scoped region timer; active only when \c ASTRALDB_PROFILE_OUTPUT is set. */
class SemistructuredProfileScope {
public:
	explicit SemistructuredProfileScope(const std::string_view Region) noexcept
	    : Region_(Region), Start_(std::chrono::steady_clock::now()), Active_(Enabled()) {}

	~SemistructuredProfileScope() {
		if(!Active_)
			return;
		if(const RegionTimingHook &Hook = RegionTimingHookRef())
			Hook(std::string(Region_),
			     std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Start_));
	}

	SemistructuredProfileScope(const SemistructuredProfileScope &) = delete;
	SemistructuredProfileScope &operator=(const SemistructuredProfileScope &) = delete;

private:
	static bool Enabled() noexcept {
		const char *P = EnvGet("ASTRALDB_PROFILE_OUTPUT");
		return P != nullptr && P[0] != '\0';
	}

	static const RegionTimingHook &RegionTimingHookRef();

	std::string_view Region_;
	std::chrono::steady_clock::time_point Start_;
	bool Active_;
};

} // namespace AstralDB
