#include <Database/MathSci/MathSciListCache.hxx>

#include <Database/MathSci/MathSciComplex.hxx>

#include <cstdint>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>

namespace AstralDB::MathSciListCache {
namespace {

constexpr std::size_t MaxCacheEntries = 96;

std::unordered_map<std::uint64_t, std::vector<double>> g_Cache;
std::list<std::uint64_t> g_Order;
std::unordered_map<std::uint64_t, std::list<std::uint64_t>::iterator> g_Iter;

std::uint64_t HashKey(std::string_view S) {
	std::uint64_t H = 1469598103934665603ull;
	for(unsigned char C : S) {
		H ^= C;
		H *= 1099511628211ull;
	}
	return H;
}

std::optional<double> ToNum(std::string_view S) {
	if(S.empty())
		return std::nullopt;
	try {
		size_t Pos = 0;
		const double V = std::stod(std::string(S.data(), S.size()), &Pos);
		if(Pos > 0)
			return V;
	} catch(...) {
	}
	return std::nullopt;
}

std::optional<std::vector<double>> FastParseLCell(std::string_view Cell) {
	if(Cell.size() < 4 || Cell[0] != 'L' || Cell[1] != '[')
		return std::nullopt;
	const size_t Close = Cell.find(']');
	if(Close == std::string::npos || Close + 2 >= Cell.size() || Cell[Close + 1] != ':')
		return std::nullopt;
	std::vector<double> Out;
	Out.reserve(32);
	std::string_view Body = Cell.substr(Close + 2);
	size_t Pos = 0;
	while(Pos < Body.size()) {
		size_t End = Pos;
		while(End < Body.size() && Body[End] != ',')
			++End;
		const auto N = ToNum(Body.substr(Pos, End - Pos));
		if(!N)
			return std::nullopt;
		Out.push_back(*N);
		Pos = End + (End < Body.size() ? 1 : 0);
	}
	return Out.empty() ? std::nullopt : std::optional<std::vector<double>>(std::move(Out));
}

void EvictIfNeeded() {
	while(g_Order.size() > MaxCacheEntries) {
		const std::uint64_t Old = g_Order.back();
		g_Order.pop_back();
		g_Cache.erase(Old);
		g_Iter.erase(Old);
	}
}

void Touch(std::uint64_t Key) {
	const auto It = g_Iter.find(Key);
	if(It != g_Iter.end())
		g_Order.splice(g_Order.begin(), g_Order, It->second);
}

} // namespace

std::optional<std::vector<double>> ParseDoublesUncached(std::string_view Cell) {
	if(const auto Fast = FastParseLCell(Cell))
		return Fast;
	const auto V = MathSciComplex::ParseNumericVec(Cell);
	if(!V || V->Kind != MathSciComplex::NumericKind::Real)
		return std::nullopt;
	std::vector<double> Out(V->Values.size());
	for(std::size_t I = 0; I < V->Values.size(); ++I)
		Out[I] = static_cast<double>(V->Values[I]);
	return Out;
}

const std::vector<double> *LookupDoubles(std::string_view Cell) {
	const std::uint64_t Key = HashKey(Cell);
	const auto Hit = g_Cache.find(Key);
	if(Hit != g_Cache.end()) {
		Touch(Key);
		return &Hit->second;
	}
	const auto Parsed = ParseDoublesUncached(Cell);
	if(!Parsed)
		return nullptr;
	const std::string StoredKey(Cell);
	const std::uint64_t StoredHash = HashKey(StoredKey);
	auto [Ins, _] = g_Cache.emplace(StoredHash, *Parsed);
	g_Order.push_front(StoredHash);
	g_Iter[StoredHash] = g_Order.begin();
	EvictIfNeeded();
	return &Ins->second;
}

void ClearCacheForTests() {
	g_Cache.clear();
	g_Order.clear();
	g_Iter.clear();
}

} // namespace AstralDB::MathSciListCache
