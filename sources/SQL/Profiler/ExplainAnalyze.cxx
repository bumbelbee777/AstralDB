#include <SQL/Profiler/ExplainAnalyze.hxx>

#include <SQL/Bytecode/BytecodeDisasm.hxx>
#include <SQL/Bytecode/BytecodeInspect.hxx>
#include <IO/EnvUtil.hxx>
#include <sstream>

namespace AstralDB {
namespace SQL {

namespace {

void AppendPlanNode(std::ostringstream &O, const ExplainPlanNode &Node, int Indent) {
	O << std::string(static_cast<std::size_t>(Indent) * 2, ' ') << Node.Kind;
	if(!Node.Detail.empty())
		O << " (" << Node.Detail << ")";
	if(Node.Rows > 0)
		O << " rows=" << Node.Rows;
	if(Node.ActualTime.count() > 0)
		O << " time=" << (Node.ActualTime.count() / 1000) << "us";
	O << '\n';
	for(const ExplainPlanNode &Child : Node.Children)
		AppendPlanNode(O, Child, Indent + 1);
}

ExplainPlanNode BuildPlanFromBytecode(const Bytecode &Code) {
	ExplainPlanNode Root;
	Root.Kind = "Program";
	Root.Detail = std::to_string(Code.size()) + " instructions";
	const BytecodeAnalysis Analysis = AnalyzeBytecode(Code);
	for(const auto &[Op, Count] : Analysis.OpcodeHistogram) {
		ExplainPlanNode Child;
		Child.Kind = Op;
		Child.Detail = "count=" + std::to_string(Count);
		Root.Children.push_back(std::move(Child));
	}
	if(Analysis.HasJoins) {
		ExplainPlanNode J;
		J.Kind = "Joins";
		Root.Children.push_back(std::move(J));
	}
	if(Analysis.HasAggregates) {
		ExplainPlanNode A;
		A.Kind = "Aggregates";
		Root.Children.push_back(std::move(A));
	}
	return Root;
}

} // namespace

std::string ExplainAnalyze::GeneratePlan(const Bytecode &Code, bool Analyze) {
	std::ostringstream O;
	O << (Analyze ? "EXPLAIN ANALYZE\n" : "EXPLAIN\n");
	const ExplainPlanNode Root = BuildPlanFromBytecode(Code);
	AppendPlanNode(O, Root, 0);
#if !defined(NDEBUG)
	if(!Analyze)
		O << "\n-- bytecode --\n" << DisassemblePretty(Code);
#else
	if(!Analyze && EnvTruthy("ASTRALDB_VERBOSE_EXPLAIN"))
		O << "\n-- bytecode --\n" << DisassemblePretty(Code);
#endif
	return O.str();
}

ExplainAnalyzeResult ExplainAnalyze::ExecuteWithAnalyze(BytecodeInterpreter &Vm, const Bytecode &Code) {
	ExplainAnalyzeResult Out;
	const auto T0 = std::chrono::steady_clock::now();
	Out.Root = BuildPlanFromBytecode(Code);
	Out.PlanText = GeneratePlan(Code, true);
	Vm.Execute(Code);
	const auto T1 = std::chrono::steady_clock::now();
	Out.TotalTime = std::chrono::duration_cast<std::chrono::nanoseconds>(T1 - T0);
	Out.ScannedRows = Vm.LastTimeSqlStats().RowsScanned;
	Out.ResultRows = Vm.LastTimeSqlStats().ResultRows;
	Out.Root.ActualTime = Out.TotalTime;
	return Out;
}

ExplainAnalyzeResult ExplainAnalyze::ExecuteWithAnalyze(BytecodeInterpreter &Vm, const CompiledBytecode &Compiled) {
	return ExecuteWithAnalyze(Vm, Compiled.Instructions);
}

} // namespace SQL
} // namespace AstralDB
