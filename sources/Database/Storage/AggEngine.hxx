#pragma once

#include <DS/AggArena.hxx>
#include <DS/AggState.hxx>
#include <DS/CuckooMap.hxx>
#include <DS/RadixPartition.hxx>
#include <DS/SimdHash.hxx>
#include <IO/Job.hxx>
#include <Database/Execution/BytecodeTypes.hxx>
#include <Database/Execution/PlanTypes.hxx>

#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace AstralDB {

using RowItem = std::unordered_map<std::string, std::string>;
using RowTable = std::vector<RowItem>;

enum class AggEngineMode { Legacy, Simd, Async };

AggEngineMode ParseAggEngineMode();

/** SIMD/async GROUP BY engine feeding RunGroupByCore fast paths. */
struct AggEngine {
	static bool TryRun(RowTable &Tbl, const SQL::Instruction &Inst, const std::vector<std::string> &ActiveKeys);
};

} // namespace AstralDB
