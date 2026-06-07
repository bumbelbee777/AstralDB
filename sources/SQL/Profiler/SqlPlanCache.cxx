#include <SQL/Profiler/SqlPlanCache.hxx>

#include <Database/Database.hxx>
#include <Database/Storage/BulkQueryMetadata.hxx>
#include <Database/Storage/BulkShapePlan.hxx>
#include <SQL/Shape/ShapeComposition.hxx>
#include <DS/SimdHash.hxx>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <unordered_map>

namespace AstralDB {
namespace SQL {
namespace SqlPlanCache {
namespace {

struct Entry {
	uint64_t SqlHash = 0;
	uint64_t CatalogFp = 0;
	OptimizationLevel Opt = OptimizationLevel::Basic;
	CompiledBytecode Compiled;
};

std::unordered_map<uint64_t, Entry> g_Cache;
std::deque<uint64_t> g_Lru;
std::atomic<uint64_t> g_Hits{0};
std::atomic<uint64_t> g_Misses{0};

constexpr std::size_t kDefaultMaxEntries = 128;

bool EnvEnabled(const char *Name, const bool Default) {
#if defined(_MSC_VER)
	char *Buf = nullptr;
	size_t Len = 0;
	if(_dupenv_s(&Buf, &Len, Name) != 0 || !Buf)
		return Default;
	const bool Out = !(Buf[0] == '0' && Buf[1] == '\0');
	free(Buf);
	return Out;
#else
	if(const char *V = std::getenv(Name))
		return !(V[0] == '0' && V[1] == '\0');
	return Default;
#endif
}

std::size_t MaxEntries() {
#if defined(_MSC_VER)
	char *Buf = nullptr;
	size_t Len = 0;
	if(_dupenv_s(&Buf, &Len, "ASTRALDB_PLAN_CACHE_SIZE") != 0 || !Buf)
		return kDefaultMaxEntries;
	char *End = nullptr;
	const unsigned long long N = std::strtoull(Buf, &End, 10);
	free(Buf);
	if(End == Buf || N == 0)
		return kDefaultMaxEntries;
	return static_cast<std::size_t>(N);
#else
	if(const char *V = std::getenv("ASTRALDB_PLAN_CACHE_SIZE")) {
		char *End = nullptr;
		const unsigned long long N = std::strtoull(V, &End, 10);
		if(End != V && N > 0)
			return static_cast<std::size_t>(N);
	}
	return kDefaultMaxEntries;
#endif
}

uint64_t MixKey(const uint64_t SqlHash, const OptimizationLevel Opt, const uint64_t CatalogFp) noexcept {
	return SqlHash ^ (static_cast<uint64_t>(Opt) * 0x9E3779B97F4A7C15ULL) ^ (CatalogFp * 0xBF58476D1CE4E5B9ULL);
}

void TouchLru(const uint64_t Key) {
	g_Lru.erase(std::remove(g_Lru.begin(), g_Lru.end(), Key), g_Lru.end());
	g_Lru.push_back(Key);
	while(g_Lru.size() > MaxEntries()) {
		const uint64_t Old = g_Lru.front();
		g_Lru.pop_front();
		g_Cache.erase(Old);
	}
}

} // namespace

bool Enabled() noexcept { return EnvEnabled("ASTRALDB_PLAN_CACHE", true); }

uint64_t CatalogFingerprint(const Database *Db) noexcept {
	if(!Db)
		return 0;
	uint64_t Fp = 0;
	const auto Views = Db->ViewDefinitionsSnapshot();
	for(const auto &[Name, Body] : Views) {
		Fp ^= SimdHash::Hash64(Name);
		Fp ^= SimdHash::Hash64(Body);
	}
	Fp ^= DatabaseWorkloadFingerprint(Db);
	return Fp;
}

std::optional<CompiledBytecode> Lookup(const std::string_view Sql, const OptimizationLevel OptLevel,
                                       const uint64_t CatalogFp) noexcept {
	if(!Enabled())
		return std::nullopt;
	const uint64_t SqlHash = SimdHash::Hash64(Sql);
	const uint64_t Key = MixKey(SqlHash, OptLevel, CatalogFp);
	const auto It = g_Cache.find(Key);
	if(It == g_Cache.end()) {
		g_Misses.fetch_add(1, std::memory_order_relaxed);
		return std::nullopt;
	}
	if(It->second.SqlHash != SqlHash || It->second.CatalogFp != CatalogFp || It->second.Opt != OptLevel)
		return std::nullopt;
	g_Hits.fetch_add(1, std::memory_order_relaxed);
	TouchLru(Key);
	return It->second.Compiled;
}

void Store(const std::string_view Sql, const OptimizationLevel OptLevel, const uint64_t CatalogFp,
           CompiledBytecode Compiled) noexcept {
	if(!Enabled())
		return;
	const uint64_t SqlHash = SimdHash::Hash64(Sql);
	BulkQueryShape Shape;
	QueryShapeComposition Comp;
	if(InferShapeCompositionFromBytecode(Compiled.Instructions, Comp) && Comp.Count > 0)
		Compiled.ShapeFingerprint = QueryShapeFingerprint128FromComposition(Comp);
	else if(InferBulkQueryShapeFromBytecode(Compiled.Instructions, Shape))
		Compiled.ShapeFingerprint = QueryShapeFingerprint128FromBytecode(Compiled.Instructions, Shape);
	else
		Compiled.ShapeFingerprint = {};
	const uint64_t Key = MixKey(SqlHash, OptLevel, CatalogFp);
	Entry E;
	E.SqlHash = SqlHash;
	E.CatalogFp = CatalogFp;
	E.Opt = OptLevel;
	E.Compiled = std::move(Compiled);
	g_Cache.insert_or_assign(Key, std::move(E));
	TouchLru(Key);
}

CompiledBytecode LookupOrCompile(const std::string_view Sql, Logger *Log, const OptimizationLevel OptLevel,
                                 const Database *Db) noexcept {
	const uint64_t CatalogFp = CatalogFingerprint(Db);
	if(auto Hit = Lookup(Sql, OptLevel, CatalogFp))
		return *Hit;
	CompiledBytecode Out = BuildCompiledBytecode(Log, OptLevel, Db);
	Store(Sql, OptLevel, CatalogFp, std::move(Out));
	const auto Hit = Lookup(Sql, OptLevel, CatalogFp);
	return Hit ? *Hit : BuildCompiledBytecode(Log, OptLevel, Db);
}

void Clear() noexcept {
	g_Cache.clear();
	g_Lru.clear();
}

uint64_t HitsForTests() noexcept { return g_Hits.load(std::memory_order_relaxed); }

uint64_t MissesForTests() noexcept { return g_Misses.load(std::memory_order_relaxed); }

} // namespace SqlPlanCache
} // namespace SQL
} // namespace AstralDB
