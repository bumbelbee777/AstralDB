#pragma once

#include <SQL/Bytecode/Bytecode.hxx>
#include <SQL/Optimizer/ConfidenceScoring.hxx>
#include <SQL/Profiler/SqlSessionConfig.hxx>

namespace AstralDB {
namespace SQL {

struct AdaptiveOptimizerOptions {
	OptimizationLevel BaseLevel = OptimizationLevel::Maximum;
	Confidence MinConfidence = Confidence::Medium;
	bool InsertRuntimeChecks = false;
};

/** Confidence-gated aggressive optimizer pipeline. */
class AdaptiveOptimizer {
public:
	explicit AdaptiveOptimizer(AdaptiveOptimizerOptions Options = {});

	bool RunAdaptiveOptimizerPipeline(Bytecode &Code, Logger *Logger = nullptr);

private:
	AdaptiveOptimizerOptions Options_;
	ConfidenceScorer Scorer_;
};

void RunAdaptiveOptimizerPipeline(Bytecode &Code, Logger *Logger = nullptr);

[[nodiscard]] Confidence ConfidenceFromOptFloor(double Floor) noexcept;

void RunAdaptiveOptimizerPipeline(Bytecode &Code, Logger *Logger, const SqlSessionConfig *SessionCfg);

} // namespace SQL
} // namespace AstralDB
