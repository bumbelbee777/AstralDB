#include <SQL/Profiler/SqlPlanCache.hxx>
#include <SQL/Profiler/SqlPipeline.hxx>

#include <SQL/Bytecode/FastPathGuard.hxx>
#include <SQL/Profiler/ExplainAnalyze.hxx>
#include <SQL/Profiler/PragmaHandler.hxx>

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace AstralDB {
namespace SQL {

namespace {
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::duration<double, std::milli>;
} // namespace

const char *VmChoiceLabel(const SqlSessionConfig::VmChoice Choice) {
	switch(Choice) {
	case SqlSessionConfig::VmChoice::Dbvm:
		return "DBVM";
	case SqlSessionConfig::VmChoice::Silly:
		return "SILLY";
	case SqlSessionConfig::VmChoice::Jit:
		return "JIT";
	default:
		return "AUTO";
	}
}

static OptimizationLevel EffectiveOptLevel(const SqlSessionConfig &Cfg, OptimizationLevel Cli) {
	if(Cfg.OptLevel != OptimizationLevel::Advanced || Cli != OptimizationLevel::Advanced)
		return Cfg.OptLevel;
	return Cli;
}

SqlRunTimings RunSqlScript(BytecodeInterpreter &Interpreter, const std::string_view Sql, Logger *Log,
                           const OptimizationLevel CliOptLevel) {
	SqlRunTimings Timings;
	const auto T0 = Clock::now();
	Parser P(Sql);
	const auto T1 = Clock::now();
	Timings.ParseMs = Ms(T1 - T0).count();

	SqlSessionConfig &Cfg = Interpreter.SessionConfig();
	ApplyPragmaStatementsFromAst(Cfg, &Interpreter);
	Cfg.OptLevel = EffectiveOptLevel(Cfg, CliOptLevel);

	const auto T2 = Clock::now();
	Bytecode Code;
	const uint64_t CatalogFp = SqlPlanCache::CatalogFingerprint(Interpreter.PrimaryDatabase());
	if(auto Hit = SqlPlanCache::Lookup(Sql, Cfg.OptLevel, CatalogFp))
		Code = Hit->Instructions;
	else {
		Code = BuildBytecode(Log, Cfg.OptLevel, Interpreter.PrimaryDatabase(), &Cfg);
		CompiledBytecode Compiled;
		Compiled.Instructions = Code;
		DedupBytecodeStringImmediates(Compiled.Instructions, Compiled.StringPool);
		SqlPlanCache::Store(Sql, Cfg.OptLevel, CatalogFp, std::move(Compiled));
	}
	const auto T3 = Clock::now();
	Timings.CompileMs = Ms(T3 - T2).count();

	if(Cfg.Explain != SqlSessionConfig::ExplainMode::Off) {
		const bool Analyze = Cfg.Explain == SqlSessionConfig::ExplainMode::Analyze;
		if(Analyze) {
			const ExplainAnalyzeResult R = ExplainAnalyze::ExecuteWithAnalyze(Interpreter, Code);
			std::cout << ExplainAnalyze::GeneratePlan(Code, true) << "\n";
			const double Ms =
			    std::chrono::duration<double, std::milli>(R.TotalTime).count();
			std::cout << "Total time: " << Ms << "ms scanned: " << R.ScannedRows << " result: " << R.ResultRows
			          << "\n";
			Timings.ExecuteMs = Ms;
		} else {
			std::cout << ExplainAnalyze::GeneratePlan(Code, false) << "\n";
		}
		return Timings;
	}

	for(auto &Node : AST) {
		if(Node && Node->Value && dynamic_cast<ExplainSelectAST *>(Node->Value.get())) {
			if(ExplainSelectAST *Ex = dynamic_cast<ExplainSelectAST *>(Node->Value.get())) {
				if(Ex->Analyze) {
					const ExplainAnalyzeResult R = ExplainAnalyze::ExecuteWithAnalyze(Interpreter, Code);
					std::cout << ExplainAnalyze::GeneratePlan(Code, true) << "\n";
					const double Ms =
					    std::chrono::duration<double, std::milli>(R.TotalTime).count();
					std::cout << "Total time: " << Ms << "ms scanned: " << R.ScannedRows << "\n";
					Timings.ExecuteMs = Ms;
					return Timings;
				}
				std::cout << ExplainAnalyze::GeneratePlan(Code, false) << "\n";
				return Timings;
			}
		}
	}

	Interpreter.ResetTimeSqlStats();
	const auto T4 = Clock::now();
	Interpreter.Execute(Code);
	const auto T5 = Clock::now();
	Timings.ExecuteMs = Ms(T5 - T4).count();

	ValidateTimeSqlIntegrity(Interpreter.MutableTimeSqlStats(), Timings.ExecuteMs);

	QueryProfile Profile;
	Profile.Name = Cfg.ProfileName.value_or("");
	Profile.Region = Cfg.RegionName.value_or("");
	Profile.ParseTime = std::chrono::duration_cast<std::chrono::nanoseconds>(T1 - T0);
	Profile.CompileTime = std::chrono::duration_cast<std::chrono::nanoseconds>(T3 - T2);
	Profile.ExecuteTime = std::chrono::duration_cast<std::chrono::nanoseconds>(T5 - T4);
	Profile.ScannedRows = Interpreter.LastTimeSqlStats().RowsScanned;
	Profile.ResultRows = Interpreter.LastTimeSqlStats().ResultRows;
	Profile.VmUsed = VmChoiceLabel(Cfg.Vm);
	Profile.Verified = !Interpreter.LastTimeSqlStats().IntegrityFailed;
	QueryProfiler::Instance().EndQuery(Profile);

	if(Cfg.ProfileDumpPath)
		QueryProfiler::Instance().DumpToJSON(*Cfg.ProfileDumpPath);

	if(Interpreter.LastTimeSqlStats().IntegrityFailed)
		throw std::runtime_error(Interpreter.LastTimeSqlStats().IntegrityMessage);

	return Timings;
}

} // namespace SQL
} // namespace AstralDB
