#pragma once

#include <SQL/Bytecode/BytecodeInterpreter.hxx>

namespace AstralDB {
namespace SQL {

/** Shape-inferred metadata fast path for lazy-bulk SELECT (filters, window, limit, count). */
[[nodiscard]] bool TryExecuteDominantBulkQueryMetadata(BytecodeInterpreter &Vm, const Bytecode &Code);

/** Multi-window orders suite (shape-detected; no query-specific stubs). */
[[nodiscard]] bool TryExecuteDominantAmbBytecode(BytecodeInterpreter &Vm, const Bytecode &Code);

} // namespace SQL
} // namespace AstralDB
