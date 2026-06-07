#pragma once

#include <SQL/Bytecode/Bytecode.hxx>
#include <SQL/Bytecode/BytecodeInterpreter.hxx>

namespace AstralDB {
namespace SQL {

struct Instruction;

/** VM dispatch for *_BULK query opcodes. Returns false when operands are invalid. */
bool HandleBulkOpcode(BytecodeInterpreter &Vm, const Bytecode &Code, const Instruction &Inst);

/** Shared lazy-bulk semistructured scan + top-K (also used by \c FUSED_SCAN_PROJECT_LIMIT). */
bool RunSemistructuredTopk(Database &Db, BytecodeInterpreter &Vm, const Instruction &Inst);

/** When bytecode is only SELECT/PUSH/CLONE + one \c SEMISTRUCTURED_TOPK_BULK, run bulk then tail ops. */
bool TryExecuteDominantSemistructuredBytecode(BytecodeInterpreter &Vm, const Bytecode &Code);

/** Q1-shaped: \c STAR_JOIN_CUBE_BULK plus window/order tail on the CTE rowset. */
bool TryExecuteDominantStarJoinCubeBytecode(BytecodeInterpreter &Vm, const Bytecode &Code);

/** Star-join SELECT + top-K on lazy bulk synthetic fact scan. */
bool TryExecuteDominantStarJoinSelectBytecode(BytecodeInterpreter &Vm, const Bytecode &Code);

/** Star-join multi-agg GROUP BY on lazy bulk synthetic fact scan. */
bool TryExecuteDominantStarJoinGroupBytecode(BytecodeInterpreter &Vm, const Bytecode &Code);

} // namespace SQL
} // namespace AstralDB
