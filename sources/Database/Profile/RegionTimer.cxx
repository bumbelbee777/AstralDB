#include <Database/Profile/RegionTimer.hxx>

namespace AstralDB {
namespace {

RegionTimingHook GHook;

} // namespace

void SetRegionTimingHook(RegionTimingHook Hook) { GHook = std::move(Hook); }

void RecordRegionTiming(const std::string &Name, const std::chrono::nanoseconds Elapsed) {
	if(GHook)
		GHook(Name, Elapsed);
}

const RegionTimingHook &SemistructuredProfileScope::RegionTimingHookRef() { return GHook; }

} // namespace AstralDB
