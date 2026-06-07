#pragma once

#include <DS/BitSliceIndex.hxx>
#include <Database/Execution/BytecodeTypes.hxx>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace AstralDB {

using RowItem = std::unordered_map<std::string, std::string>;
using RowTable = std::vector<RowItem>;

/** Single-scan sparse CUBE / ROLLUP with partial aggregate cache. */
struct SparseCube {
	static void RunCube(RowTable &Tbl, const SQL::Instruction &Inst, const std::vector<std::string> &AllKeys);
	static void RunRollup(RowTable &Tbl, const SQL::Instruction &Inst, const std::vector<std::string> &AllKeys);
};

} // namespace AstralDB
