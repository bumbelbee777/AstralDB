#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace AstralDB {
namespace SQL {

struct QueryProfile {
	std::string Name;
	std::string Region;
	std::chrono::nanoseconds ParseTime{};
	std::chrono::nanoseconds CompileTime{};
	std::chrono::nanoseconds OptimizeTime{};
	std::chrono::nanoseconds ExecuteTime{};
	std::uint64_t ScannedRows = 0;
	std::uint64_t ResultRows = 0;
	std::string VmUsed;
	std::string Plan;
	bool Verified = false;
	std::vector<std::string> FallbackReasons;
};

class QueryProfiler {
public:
	static QueryProfiler &Instance();

	void BeginQuery(const std::string &Name);
	void EndQuery(const QueryProfile &Profile);
	void RecordRegionTiming(const std::string &Name, std::chrono::nanoseconds Elapsed);
	void ClearRegionTimings();
	void DumpToJSON(const std::string &Path) const;
	void Reset();

	const std::vector<QueryProfile> &History() const noexcept { return History_; }
	const std::unordered_map<std::string, std::chrono::nanoseconds> &RegionTimings() const noexcept {
		return RegionTimings_;
	}

private:
	std::vector<QueryProfile> History_;
	std::string ActiveName_;
	std::unordered_map<std::string, std::chrono::nanoseconds> RegionTimings_;
};

} // namespace SQL
} // namespace AstralDB
