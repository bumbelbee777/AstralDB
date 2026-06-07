#pragma once

#include <SQL/Profiler/SqlSessionConfig.hxx>
#include <SQL/SQL.hxx>

namespace AstralDB {
namespace SQL {

class BytecodeInterpreter;

void ApplyPragmaAST(const PragmaAST &Pragma, SqlSessionConfig &Cfg, BytecodeInterpreter *Interpreter = nullptr);
void ApplyPragmaStatementsFromAst(SqlSessionConfig &Cfg, BytecodeInterpreter *Interpreter = nullptr);

} // namespace SQL
} // namespace AstralDB
