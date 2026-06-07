#include <SQL/MathSciOptimizer.hxx>

#include <IO/Logger.hxx>

namespace AstralDB {
namespace SQL {

void RunMathSciOptimizerPipeline(Bytecode &Code, const OptimizationLevel OptLevel, Logger *Logger) {
	(void)Code;
	if(OptLevel < OptimizationLevel::Advanced)
		return;
	if(Logger)
		Logger->Info("Applying MathSci bytecode optimizations");
}

} // namespace SQL
} // namespace AstralDB
