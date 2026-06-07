#include <SQL/Optimizer/GraphOptimizer.hxx>
#include <IO/Limits.hxx>
#include <IO/Logger.hxx>
#include <algorithm>

namespace AstralDB {
namespace SQL {

namespace {

bool BytecodeHasGraphOp(const Bytecode &Code) {
	for(const Instruction &Inst : Code) {
		switch(Inst.Opcode_) {
		case Opcode::GRAPH_REGISTER:
		case Opcode::GRAPH_DROP:
		case Opcode::GRAPH_TRAVERSE:
		case Opcode::GRAPH_MATCH:
		case Opcode::GRAPH_SHORTEST_PATH:
		case Opcode::GRAPH_PAGERANK:
		case Opcode::GRAPH_REGISTER_PROJECTION:
			return true;
		default:
			break;
		}
	}
	return false;
}

std::optional<std::string> GraphNameOperand(const Instruction &Inst) {
	if(Inst.Operands.empty())
		return std::nullopt;
	if(const auto *S = std::get_if<std::string>(&Inst.Operands[0]))
		return *S;
	return std::nullopt;
}

void NormalizeGraphMatchHops(Bytecode &Code) {
	for(Instruction &Inst : Code) {
		if(Inst.Opcode_ != Opcode::GRAPH_MATCH || Inst.Operands.size() < 5)
			continue;
		auto *MinH = std::get_if<int64_t>(&Inst.Operands[3]);
		auto *MaxH = std::get_if<int64_t>(&Inst.Operands[4]);
		if(!MinH || !MaxH)
			continue;
		if(*MinH < 1)
			*MinH = 1;
		if(*MaxH < *MinH)
			std::swap(*MinH, *MaxH);
		if(*MaxH > static_cast<int64_t>(Limits::MaxGraphTraverseDepth))
			*MaxH = static_cast<int64_t>(Limits::MaxGraphTraverseDepth);
	}
}

void ClampGraphTraverseDepth(Bytecode &Code) {
	const int64_t Cap = static_cast<int64_t>(Limits::MaxGraphTraverseDepth);
	for(Instruction &Inst : Code) {
		if(Inst.Opcode_ != Opcode::GRAPH_TRAVERSE || Inst.Operands.size() < 4)
			continue;
		if(auto *Depth = std::get_if<int64_t>(&Inst.Operands[2])) {
			if(*Depth < 1)
				*Depth = 1;
			if(*Depth > Cap)
				*Depth = Cap;
		}
	}
}

void ElideRedundantGraphDropRegister(Bytecode &Code) {
	for(size_t I = 0; I + 1 < Code.size();) {
		const Instruction &Drop = Code[I];
		const Instruction &Reg = Code[I + 1];
		if(Drop.Opcode_ == Opcode::GRAPH_DROP && Reg.Opcode_ == Opcode::GRAPH_REGISTER) {
			const auto DropName = GraphNameOperand(Drop);
			const auto RegName = GraphNameOperand(Reg);
			if(DropName && RegName && *DropName == *RegName) {
				Code.erase(Code.begin() + static_cast<std::ptrdiff_t>(I));
				continue;
			}
		}
		++I;
	}
}

} // namespace

void RunGraphOptimizerPipeline(Bytecode &Code, const OptimizationLevel OptLevel, Logger *Logger) {
	if(OptLevel == OptimizationLevel::None || Code.empty() || !BytecodeHasGraphOp(Code))
		return;
	if(Logger)
		Logger->Info("Applying graph bytecode optimizations");
	NormalizeGraphMatchHops(Code);
	ClampGraphTraverseDepth(Code);
	if(OptLevel >= OptimizationLevel::Basic)
		ElideRedundantGraphDropRegister(Code);
}

} // namespace SQL
} // namespace AstralDB
