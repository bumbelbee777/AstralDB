#pragma once

#include <SQL/Bytecode.hxx>

namespace AstralDB {
namespace SQL {

void RunMathSciOptimizerPipeline(Bytecode &Code, OptimizationLevel OptLevel, Logger *Logger = nullptr);

} // namespace SQL
} // namespace AstralDB
