#pragma once

#include <SQL/Bytecode/BytecodeInterpreter.hxx>

namespace AstralDB {
namespace SQL {

/** Ten-table HTAP warehouse Q10/Q11 megafusion fast path (lazy scan, checkpointable phases). */
[[nodiscard]] bool TryExecuteDominantWarehouseMegafusionBytecode(BytecodeInterpreter &Vm, const Bytecode &Code);

} // namespace SQL
} // namespace AstralDB
