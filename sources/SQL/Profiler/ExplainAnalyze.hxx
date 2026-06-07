#pragma once

#include <Database/Database.hxx>
#include <SQL/Bytecode/Bytecode.hxx>
#include <SQL/Bytecode/BytecodeInterpreter.hxx>

#include <chrono>
#include <string>

namespace AstralDB {
namespace SQL {

struct ExplainPlanNode {
	std::string Kind;
	std::string Detail;
	std::chrono::nanoseconds ActualTime{};
	std::uint64_t Rows = 0;
	std::vector<ExplainPlanNode> Children;
};

struct ExplainAnalyzeResult {
	std::string PlanText;
	ExplainPlanNode Root;
	std::chrono::nanoseconds TotalTime{};
	std::uint64_t ScannedRows = 0;
	std::uint64_t ResultRows = 0;
};

/** EXPLAIN / EXPLAIN ANALYZE plan generation and timed execution. */
class ExplainAnalyze {
public:
	static std::string GeneratePlan(const Bytecode &Code, bool Analyze);
	static ExplainAnalyzeResult ExecuteWithAnalyze(BytecodeInterpreter &Vm, const Bytecode &Code);
	static ExplainAnalyzeResult ExecuteWithAnalyze(BytecodeInterpreter &Vm, const CompiledBytecode &Compiled);
};

} // namespace SQL
} // namespace AstralDB
