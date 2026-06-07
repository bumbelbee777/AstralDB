#pragma once

#include <Database/Execution/FastPathGuard.hxx>
#include <SQL/Bytecode/BytecodeInterpreter.hxx>

#include <cstdint>
#include <functional>
#include <string>

namespace AstralDB {
namespace SQL {

/** Bit flags for fast paths that bypass full row materialization. */
enum FastPathFlag : std::uint32_t {
	FastPathNone = 0,
	FastPathLazyBulkCount = 1u << 0,
	FastPathJoinMatchCount = 1u << 1,
	FastPathJoinStarGroupBy = 1u << 2,
	FastPathLazyBulkGroupBy3 = 1u << 3,
	FastPathLazyBulkLimitPk = 1u << 4,
	FastPathOrderByPkSkip = 1u << 5,
	FastPathRadixHashJoin = 1u << 6,
	FastPathRadixGroupBy = 1u << 7,
	FastPathSlidingWindowBulk = 1u << 8,
	/** Batched row-store materialization after sliding window on lazy bulk synthetic tables. */
	FastPathSlidingWindowBulkMaterialize = 1u << 9,
	/** Precomputed pass-bit filter + row-id scalar eval (lazy bulk entity-scan workloads). */
	FastPathLazyBulkSemistructured = 1u << 10,
	FastPathSemistructuredTopkBulk = 1u << 11,
	FastPathStarJoinCubeBulk = 1u << 12,
	FastPathStarJoinSelectBulk = 1u << 13,
	FastPathStarJoinGroupBulk = 1u << 14,
};

inline FastPathFlag operator|(FastPathFlag A, FastPathFlag B) {
	return static_cast<FastPathFlag>(static_cast<std::uint32_t>(A) | static_cast<std::uint32_t>(B));
}

inline FastPathFlag &operator|=(FastPathFlag &A, FastPathFlag B) {
	A = A | B;
	return A;
}

void RecordFastPathHit(BytecodeInterpreter::TimeSqlStats &Stats, FastPathFlag Flag, std::uint64_t MinScanned = 0,
                       std::uint64_t MinResult = 0);

void ValidateTimeSqlIntegrity(BytecodeInterpreter::TimeSqlStats &Stats, double ExecuteMs);

using AstralDB::VerifyFastPathEnabled;

/** Compare fast vs reference; on mismatch sets IntegrityFailed. Returns fast result when verify off or match. */
template<typename T>
T VerifyFastPathOrFail(const char *Label, const T &Fast, const T &Reference, BytecodeInterpreter::TimeSqlStats &Stats) {
	if(!VerifyFastPathEnabled())
		return Fast;
	if(Fast == Reference)
		return Fast;
	Stats.IntegrityFailed = true;
	Stats.IntegrityMessage = std::string("ASTRALDB_VERIFY_FAST_PATH mismatch: ") + Label;
	return Fast;
}

template<typename FastFn, typename RefFn>
auto RunWithOptionalVerify(const char *Label, FastFn &&Fast, RefFn &&Ref, BytecodeInterpreter::TimeSqlStats &Stats) {
	const auto FastVal = Fast();
	if(!VerifyFastPathEnabled())
		return FastVal;
	const auto RefVal = Ref();
	if(FastVal != RefVal) {
		Stats.IntegrityFailed = true;
		Stats.IntegrityMessage = std::string("ASTRALDB_VERIFY_FAST_PATH mismatch: ") + Label;
	}
	return FastVal;
}

} // namespace SQL
} // namespace AstralDB
