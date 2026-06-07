#include <SQL/Rewrite/LazyCorrelationCache.hxx>

#include <sstream>

namespace AstralDB {
namespace SQL {

namespace {
LazyCorrelationCache GStatementCache;
} // namespace

void LazyCorrelationCache::Clear() { Entries_.clear(); }

bool LazyCorrelationCache::Lookup(const std::string &Key, bool &ResultOut) const {
	const auto It = Entries_.find(Key);
	if(It == Entries_.end())
		return false;
	ResultOut = It->second;
	return true;
}

void LazyCorrelationCache::Store(const std::string &Key, bool Result) { Entries_[Key] = Result; }

std::string LazyCorrelationCache::BuildKey(const std::vector<std::string> &CorrelationValues) {
	std::ostringstream O;
	for(std::size_t I = 0; I < CorrelationValues.size(); ++I) {
		if(I)
			O << '\x1E';
		O << CorrelationValues[I];
	}
	return O.str();
}

LazyCorrelationCache &StatementCorrelationCache() { return GStatementCache; }

} // namespace SQL
} // namespace AstralDB
