#include <Database/Storage/SemistructuredResultPolicy.hxx>

#include <algorithm>

namespace AstralDB {
namespace SemistructuredResultPolicy {

namespace {

std::size_t ParseSizeEnv(const char *Name, std::size_t Default) noexcept {
	const char *V = std::getenv(Name);
	if(V == nullptr || V[0] == '\0')
		return Default;
	char *End = nullptr;
	const unsigned long long N = std::strtoull(V, &End, 10);
	if(End == V)
		return Default;
	return static_cast<std::size_t>(N);
}

std::uint32_t ParseU32Env(const char *Name, std::uint32_t Default) noexcept {
	const std::size_t N = ParseSizeEnv(Name, Default);
	return static_cast<std::uint32_t>(std::min<std::size_t>(N, 0xFFFFFFFFu));
}

} // namespace

std::size_t InsertCellStripMinTableRows() noexcept {
	return ParseSizeEnv("ASTRALDB_INSERT_CELL_STRIP_MIN_ROWS", 10'000'000);
}

std::uint32_t InsertCellStripMinK() noexcept { return ParseU32Env("ASTRALDB_INSERT_CELL_STRIP_MIN_K", 10'000); }

std::size_t QueryInsertCellStripMinTableRows() noexcept {
	return ParseSizeEnv("ASTRALDB_QUERY_INSERT_CELL_STRIP_MIN_ROWS", 10'000'000);
}

std::size_t QueryStripPackCacheMinTableRows() noexcept {
	return ParseSizeEnv("ASTRALDB_QUERY_STRIP_PACK_CACHE_MIN_ROWS", 10'000'000);
}

std::size_t ParallelMaterializeMinWinners(const std::size_t TableRowCount) noexcept {
	if(TableRowCount >= 50'000'000)
		return ParseSizeEnv("ASTRALDB_PARALLEL_MAT_MIN_WINNERS_LARGE", 4'096);
	if(TableRowCount >= 10'000'000)
		return ParseSizeEnv("ASTRALDB_PARALLEL_MAT_MIN_WINNERS_MED", 8'192);
	if(TableRowCount < 10'000'000)
		return ParseSizeEnv("ASTRALDB_PARALLEL_MAT_MIN_WINNERS_TINY", 2'048);
	return ParseSizeEnv("ASTRALDB_PARALLEL_MAT_MIN_WINNERS_SMALL", 32'768);
}

bool ShouldBuildInsertCellStrips(const std::size_t TableRowCount, const std::uint32_t K) noexcept {
	if(TableRowCount < InsertCellStripMinTableRows())
		return false;
	return K >= InsertCellStripMinK();
}

bool ShouldUseInsertCellStripsAtQuery(const std::size_t TableRowCount, const std::size_t WinnerCount) noexcept {
	(void)WinnerCount;
	return TableRowCount >= QueryInsertCellStripMinTableRows();
}

bool ShouldCacheStripPackAtQuery(const std::size_t TableRowCount) noexcept {
	return TableRowCount >= QueryStripPackCacheMinTableRows();
}

} // namespace SemistructuredResultPolicy
} // namespace AstralDB
