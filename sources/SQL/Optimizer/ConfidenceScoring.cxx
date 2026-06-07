#include <SQL/Optimizer/ConfidenceScoring.hxx>

namespace AstralDB {
namespace SQL {

namespace {

bool IsControlFlow(const Instruction &Inst) {
	switch(Inst.Opcode_) {
	case Opcode::JMP:
	case Opcode::CALL:
	case Opcode::RET:
	case Opcode::RECURSIVE_CTE_FIXPOINT:
	case Opcode::PROC_TRY:
	case Opcode::PROC_END_TRY:
	case Opcode::PROC_JUMP_IF_TABLE_EMPTY:
		return true;
	default:
		return false;
	}
}

} // namespace

bool ConfidenceScorer::IsSideEffectFreeScan(const Instruction &Inst) {
	switch(Inst.Opcode_) {
	case Opcode::SELECT:
	case Opcode::FILTER_DNF:
	case Opcode::GROUP_BY:
	case Opcode::LIMIT:
	case Opcode::SLICE_RANGE:
	case Opcode::INNER_JOIN:
	case Opcode::STORAGE_HINT:
	case Opcode::NOP:
		return true;
	default:
		return Inst.IsPure();
	}
}

Confidence ConfidenceScorer::ScoreBytecode(const Bytecode &Code) const {
	if(Code.empty())
		return Confidence::Certain;

	std::size_t Pure = 0;
	std::size_t SideFx = 0;
	std::size_t Control = 0;
	for(const Instruction &Inst : Code) {
		if(IsControlFlow(Inst))
			++Control;
		else if(Inst.HasSideEffects())
			++SideFx;
		else if(IsSideEffectFreeScan(Inst))
			++Pure;
	}

	if(Control > 0 && SideFx > 0)
		return Confidence::Low;
	if(SideFx == 0 && Pure > 0)
		return Confidence::High;
	if(SideFx <= 1 && Pure >= SideFx)
		return Confidence::Medium;
	if(SideFx == 0)
		return Confidence::Certain;
	return Confidence::Reject;
}

Confidence ConfidenceScorer::ScoreFusionPlan(bool HasColumnarHint, std::size_t FilterCount, bool HasJoin) const {
	if(!HasColumnarHint)
		return Confidence::Low;
	if(HasJoin && FilterCount > 4)
		return Confidence::Medium;
	if(FilterCount == 0)
		return Confidence::Low;
	if(FilterCount <= 3)
		return Confidence::High;
	return Confidence::Medium;
}

} // namespace SQL
} // namespace AstralDB
