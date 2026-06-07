#pragma once

#include <SQL/Bytecode/BytecodeInterpreter.hxx>
#include <SQL/Profiler/QueryProfiler.hxx>

#include <chrono>
#include <string>
#include <string_view>

namespace AstralDB {
class Logger;

namespace SQL {

struct SqlRunTimings {
	double ParseMs = 0;
	double CompileMs = 0;
	double OptimizeMs = 0;
	double ExecuteMs = 0;
};

/** Parse, apply PRAGMAs, optionally EXPLAIN, compile, and execute a SQL script. */
SqlRunTimings RunSqlScript(BytecodeInterpreter &Interpreter, std::string_view Sql, Logger *Log,
                           OptimizationLevel CliOptLevel = OptimizationLevel::Advanced);

const char *VmChoiceLabel(SqlSessionConfig::VmChoice Choice);

} // namespace SQL
} // namespace AstralDB
