#pragma once

#include <cstddef>
#include <cstdint>

namespace AstralDB {

/** Central guards against pathological hosts (OOM-ish growth, VM runaway, parser blow-up, lock contention). */
struct Limits {
	static constexpr std::size_t MaxSqlSourceBytes = 16ULL * 1024ULL * 1024ULL;
	static constexpr std::size_t MaxSqlTokens = 2'000'000ULL;
	static constexpr std::size_t MaxSqlStatementsPerScript = 200'000ULL;
	static constexpr std::size_t MaxBuildAstIterations = 64'000'000ULL;
	static constexpr std::uint64_t MaxInterpreterSteps = 500'000'000ULL;
	static constexpr std::size_t MaxInterpreterStackDepth = 65'536ULL;
	/** Busy-spins before yielding the OS thread while waiting on Spinlock_. */
	static constexpr unsigned SpinYieldSpinsThreshold = 2'048U;
	static constexpr unsigned SpinSleepMicrosecondsCap = 2'048U;
	/** Rough ceiling on std::launch::async futures from Database::RunAsync (best-effort, see Database.cxx). */
	static constexpr unsigned MaxOutstandingAsyncJobs = 1'024U;
	/** INSERT ... BULK N codegen expands to N logical rows at compile time; keep bounded. */
	static constexpr std::uint64_t MaxBulkInsertRows = 500'000ULL;
	/** Scratch for SQL bytecode staging (\c std::pmr::monotonic_buffer_resource); spills to heap after exhaustion. */
	static constexpr std::size_t SqlBytecodeArenaBytes = 512ULL * 1024ULL;
	/** LIKE DP grid \c (pat+1)*(str+1) cell cap — rejects absurd patterns before \c std::bad_array_new_length. */
	static constexpr std::size_t MaxSqlLikeDpCells = 4ULL * 1024ULL * 1024ULL;
	static constexpr unsigned MaxSqlViewExpansionDepth = 16U;
	/** `ROW_NUMBER() OVER (PARTITION BY …)` emits this many identifiers at codegen; rejects larger lists early. */
	static constexpr std::size_t MaxWindowPartitionColumns = 16ULL;
	/** Window functions (\c ROW_NUMBER , \c SUM() OVER , \c LAG , …) per \c SELECT list. */
	static constexpr std::size_t MaxWindowFunctionsPerSelect = 16ULL;
	/** Searched `CASE WHEN … THEN …` arms in one expression; rejects larger lists early. */
	static constexpr std::size_t MaxCaseWhenArms = 32ULL;
	/** Scalar SQL builtin arguments (`CONCAT`, etc.) per function call in a SELECT projection. */
	static constexpr std::size_t MaxScalarSqlFuncArgs = 16ULL;
	/** \c WITH RECURSIVE fixpoint iterations (row-growth rounds) per CTE. */
	static constexpr std::uint64_t MaxCteRecursionDepth = 1'000'000ULL;
	/** \c GRAPH TRAVERSE maximum hop depth per query. */
	static constexpr std::uint64_t MaxGraphTraverseDepth = 64ULL;
	/** Variable-length \c GRAPH MATCH without \c FROM : max distinct source vertices. */
	static constexpr std::size_t MaxGraphVarLenSourceVertices = 4096ULL;
	/** Cap rows materialized by one variable-length \c GRAPH MATCH . */
	static constexpr std::uint64_t MaxGraphVarLenResultRows = 2'000'000ULL;
	/** \c GRAPH PAGERANK power-iteration cap per statement. */
	static constexpr std::uint64_t MaxGraphPageRankIterations = 10'000ULL;
};

} // namespace AstralDB
