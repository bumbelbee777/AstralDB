#pragma once

#include <SQL/Bytecode/Bytecode.hxx>

#include <cstddef>

namespace AstralDB::SQL {

/** True when remaining bytecode only finalizes a SELECT projection (no row materialization). */
[[nodiscard]] bool RemainingBytecodeOnlyConsumesSelectFinalize(const Bytecode &Code, std::size_t StartIp) noexcept;

/**
 * Fused \c ScanFilterProjectLimit codegen may emit per-column \c SELECT ops before the bulk opcode and
 * a trailing \c PUSH of the work table; both are finalize-only for semistructured columnar commit.
 */
[[nodiscard]] bool RemainingBytecodeOnlySemistructuredFinalize(const Bytecode &Code, std::size_t StartIp) noexcept;

} // namespace AstralDB::SQL
