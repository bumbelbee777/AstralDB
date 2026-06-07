#pragma once

#include <Database/Storage/StarJoinCubeBulk.hxx>
#include <Database/Execution/PlanTypes.hxx>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace AstralDB {

struct StarJoinCubeWindowSpec {
	int Kind = 0;
	std::string PartCol;
	std::string OrderCol;
	std::string SrcCol;
	std::string OutCol;
	bool OrderAscending = true;
	int64_t FrameOffset = 1;
};

struct StarJoinCubeGroupAggSpec {
	SQL::GroupCombAggKind Kind = SQL::GroupCombAggKind::Sum;
	std::string SrcCol;
	std::string OutCol;
};

struct StarJoinCubeTailPlan {
	std::vector<StarJoinCubeWindowSpec> Windows;
	std::string FilterNotNullCol;
	std::vector<std::string> GroupKeys;
	std::vector<StarJoinCubeGroupAggSpec> GroupAggs;
	std::vector<SQL::OrderBySpec> OrderBy;
	std::size_t Limit = 0;
	std::size_t FinalSelectIp = 0;
};

/** Parse Q1-shaped bytecode tail after STAR_JOIN_CUBE_BULK (windows + filter + group + order + limit). */
[[nodiscard]] bool TryParseStarJoinCubeTail(const SQL::Bytecode &Code, std::size_t BulkIx,
                                            StarJoinCubeTailPlan &Out);

/** Fused numeric pipeline: precomputed CUBE -> windows -> filter -> group -> order -> limit (no columnar emit).
 *  Caller must hold Db exclusive bytecode lock. */
[[nodiscard]] bool ExecuteFusedStarJoinCubeTail(Database &Db, const StarJoinCubeBulkParams &Params,
                                              const StarJoinCubeTailPlan &Tail, RowTable &OutRows,
                                              std::uint64_t *RowsScannedOut);

/** True when bytecode is eligible for full fused Q1 execution (bulk + tail shape). */
[[nodiscard]] bool IsStarJoinCubeDominantBytecode(const SQL::Bytecode &Code, std::size_t &BulkIxOut);

/** Decode STAR_JOIN_CUBE_BULK operands. */
[[nodiscard]] bool ParseStarJoinCubeBulkParams(const SQL::Instruction &Inst, StarJoinCubeBulkParams &Params);

} // namespace AstralDB
