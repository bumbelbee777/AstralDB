#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace AstralDB {
namespace SQL {

/** Per-statement cache keyed by serialized outer-row correlation values. */
class LazyCorrelationCache {
public:
	void Clear();
	[[nodiscard]] bool Lookup(const std::string &Key, bool &ResultOut) const;
	void Store(const std::string &Key, bool Result);
	[[nodiscard]] std::size_t Size() const noexcept { return Entries_.size(); }

	static std::string BuildKey(const std::vector<std::string> &CorrelationValues);

private:
	std::unordered_map<std::string, bool> Entries_;
};

LazyCorrelationCache &StatementCorrelationCache();

} // namespace SQL
} // namespace AstralDB
