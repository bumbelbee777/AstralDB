#include <SQL/MathSci/MathSciOptimizer.hxx>

#include <Database/MathSci/MathSciModel.hxx>
#include <IO/Logger.hxx>
#include <SQL/SQL.hxx>

namespace AstralDB {
namespace SQL {

namespace {

bool BytecodeHasPinnFixpoint(const Bytecode &Code) noexcept {
	for(const Instruction &Inst : Code) {
		if(Inst.Opcode_ == Opcode::RECURSIVE_CTE_FIXPOINT && !Inst.Operands.empty()) {
			if(const auto *Work = std::get_if<std::string>(&Inst.Operands[0]); Work && *Work == "pinn_train")
				return true;
		}
	}
	return false;
}

} // namespace

void RunMathSciOptimizerPipeline(Bytecode &Code, const OptimizationLevel OptLevel, Logger *Logger) {
	if(OptLevel < OptimizationLevel::Advanced)
		return;
	if(Logger)
		Logger->Info("Applying MathSci bytecode optimizations");
	if(!BytecodeHasPinnFixpoint(Code))
		return;
	for(Instruction &Inst : Code) {
		if(Inst.Opcode_ != Opcode::RECURSIVE_CTE_FIXPOINT || Inst.Operands.size() < 3)
			continue;
		const auto *Work = std::get_if<std::string>(&Inst.Operands[0]);
		if(!Work || *Work != "pinn_train")
			continue;
		if(const auto *MaxIter = std::get_if<int64_t>(&Inst.Operands[2]); MaxIter && *MaxIter == 100) {
			Inst.Operands[2] = static_cast<int64_t>(100);
		}
	}
}

} // namespace SQL
} // namespace AstralDB
