#include <IO/ColumnShardRegistry.hxx>

#include <chrono>
#include <mutex>

namespace AstralDB {

namespace {

std::uint64_t NowUnixSec() {
	using Clock = std::chrono::system_clock;
	return static_cast<std::uint64_t>(
	    std::chrono::duration_cast<std::chrono::seconds>(Clock::now().time_since_epoch()).count());
}

void RemoveTree(const std::filesystem::path &Root) {
	std::error_code Ec;
	std::filesystem::remove_all(Root, Ec);
}

} // namespace

std::mutex &ColumnShardRegistry::Mutex() noexcept {
	static std::mutex M;
	return M;
}

std::vector<ColumnShardRegistry::Entry> &ColumnShardRegistry::Entries() noexcept {
	static std::vector<Entry> E;
	return E;
}

void ColumnShardRegistry::RegisterDirectory(const std::filesystem::path &Root, std::uint64_t TtlSeconds) {
	if(Root.empty())
		return;
	std::lock_guard<std::mutex> Lock(Mutex());
	const std::uint64_t Exp = NowUnixSec() + (TtlSeconds > 0 ? TtlSeconds : 3600);
	Entries().push_back(Entry{Root, Exp});
}

void ColumnShardRegistry::PurgeExpired() noexcept {
	const std::uint64_t Now = NowUnixSec();
	std::lock_guard<std::mutex> Lock(Mutex());
	auto &E = Entries();
	std::vector<Entry> Keep;
	Keep.reserve(E.size());
	for(const Entry &Ent : E) {
		if(Ent.ExpiresUnixSec <= Now) {
			RemoveTree(Ent.Path);
		} else {
			Keep.push_back(Ent);
		}
	}
	E = std::move(Keep);
}

void ColumnShardRegistry::PurgeAllRegistered() noexcept {
	std::lock_guard<std::mutex> Lock(Mutex());
	for(const Entry &Ent : Entries())
		RemoveTree(Ent.Path);
	Entries().clear();
}

std::size_t ColumnShardRegistry::RegisteredCount() noexcept {
	std::lock_guard<std::mutex> Lock(Mutex());
	return Entries().size();
}

} // namespace AstralDB
