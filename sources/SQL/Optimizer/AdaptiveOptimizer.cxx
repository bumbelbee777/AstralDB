#include <SQL/Optimizer/AdaptiveOptimizer.hxx>
#include <SQL/Optimizer/RuntimeChecks.hxx>

namespace AstralDB {
namespace SQL {

Confidence ConfidenceFromOptFloor(const double Floor) noexcept {
	if(Floor >= 0.99)
		return Confidence::Certain;
	if(Floor >= 0.90)
		return Confidence::High;
	if(Floor >= 0.75)
		return Confidence::Medium;
	if(Floor >= 0.50)
		return Confidence::Low;
	return Confidence::Reject;
}

AdaptiveOptimizer::AdaptiveOptimizer(AdaptiveOptimizerOptions Options) : Options_(Options) {}

bool AdaptiveOptimizer::RunAdaptiveOptimizerPipeline(Bytecode &Code, Logger *Logger) {
	if(Code.empty())
		return false;

	const Confidence Score = Scorer_.ScoreBytecode(Code);
	if(Score < Options_.MinConfidence) {
		if(Logger)
			Logger->Info("AdaptiveOptimizer: confidence too low; skipping aggressive passes");
		return false;
	}

	if(Logger)
		Logger->Info("AdaptiveOptimizer: running optimizer pipeline");

	RunOptimizerPipeline(Code, Options_.BaseLevel, Logger);

	if(Options_.InsertRuntimeChecks && Score < Confidence::Certain) {
		RuntimeCheckGenerator Gen;
		Gen.InsertChecks(Code, Score);
	}

	if(!ValidateBytecodeControlFlow(Code)) {
		if(Logger)
			Logger->Info("AdaptiveOptimizer: control-flow invalid after passes");
		return false;
	}
	return true;
}

void RunAdaptiveOptimizerPipeline(Bytecode &Code, Logger *Logger) {
	AdaptiveOptimizer Opt;
	Opt.RunAdaptiveOptimizerPipeline(Code, Logger);
}

void RunAdaptiveOptimizerPipeline(Bytecode &Code, Logger *Logger, const SqlSessionConfig *SessionCfg) {
	AdaptiveOptimizerOptions Opts;
	if(SessionCfg)
		Opts.MinConfidence = ConfidenceFromOptFloor(SessionCfg->OptConfidenceFloor);
	AdaptiveOptimizer Opt(Opts);
	Opt.RunAdaptiveOptimizerPipeline(Code, Logger);
}

} // namespace SQL
} // namespace AstralDB
