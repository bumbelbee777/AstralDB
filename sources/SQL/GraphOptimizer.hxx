#pragma once

#include <SQL/Bytecode.hxx>

namespace AstralDB {
namespace SQL {

/** Modest graph-specific bytecode passes (hop folding, redundant catalog ops, depth clamps). */
void RunGraphOptimizerPipeline(Bytecode &Code, OptimizationLevel OptLevel, Logger *Logger = nullptr);

} // namespace SQL
} // namespace AstralDB
