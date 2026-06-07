#include <SQL/Bytecode/Bytecode.hxx>
#include <IO/Logger.hxx>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace AstralDB {
namespace SQL {

namespace {

constexpr size_t kIpRemoved = std::numeric_limits<size_t>::max();

int PeepholeRoundsFor(const OptimizationLevel Level) {
	if(Level >= OptimizationLevel::Maximum)
		return 6;
	if(Level >= OptimizationLevel::Advanced)
		return 3;
	return 2;
}

int AdvancedRoundsFor(const OptimizationLevel Level) {
	if(Level >= OptimizationLevel::Maximum)
		return 5;
	return 2;
}

bool IsPushInt64(const Instruction &Inst, int64_t &Out) {
	if(Inst.Opcode_ != Opcode::PUSH || Inst.Operands.size() != 1)
		return false;
	if(const auto *V = std::get_if<int64_t>(&Inst.Operands[0])) {
		Out = *V;
		return true;
	}
	return false;
}

std::optional<int64_t> FoldBinary(Opcode Op, int64_t A, int64_t B) {
	switch(Op) {
	case Opcode::ADD:
		return A + B;
	case Opcode::SUB:
		return A - B;
	case Opcode::MUL:
		return A * B;
	case Opcode::DIV:
		if(B == 0)
			return std::nullopt;
		return A / B;
	case Opcode::MOD:
		if(B == 0)
			return std::nullopt;
		return A % B;
	case Opcode::AND:
		return static_cast<int64_t>(A && B);
	case Opcode::OR:
		return static_cast<int64_t>(A || B);
	case Opcode::EQ:
		return static_cast<int64_t>(A == B);
	case Opcode::NE:
		return static_cast<int64_t>(A != B);
	case Opcode::LT:
		return static_cast<int64_t>(A < B);
	case Opcode::LE:
		return static_cast<int64_t>(A <= B);
	case Opcode::GT:
		return static_cast<int64_t>(A > B);
	case Opcode::GE:
		return static_cast<int64_t>(A >= B);
	case Opcode::INT_DIV:
		if(B == 0)
			return std::nullopt;
		return A / B;
	default:
		return std::nullopt;
	}
}

using RowTriple = std::tuple<std::string, std::string, std::string>;

bool IsPowerOfTwo(int64_t V) {
	return V > 0 && (static_cast<uint64_t>(V) & (static_cast<uint64_t>(V) - 1)) == 0;
}

bool ReadDnfOperands(const std::vector<Value> &Ops, size_t Start, size_t &OutEnd,
                     std::vector<std::vector<RowTriple>> &OutBranches) {
	size_t I = Start;
	if(I >= Ops.size())
		return false;
	const auto *Nb = std::get_if<int64_t>(&Ops[I++]);
	if(!Nb || *Nb < 0 || *Nb > 64)
		return false;
	for(int64_t B = 0; B < *Nb; ++B) {
		if(I >= Ops.size())
			return false;
		const auto *Nk = std::get_if<int64_t>(&Ops[I++]);
		if(!Nk || *Nk < 0 || *Nk > 128)
			return false;
		std::vector<RowTriple> Branch;
		for(int64_t K = 0; K < *Nk; ++K) {
			if(I + 3 > Ops.size())
				return false;
			const auto *Cs = std::get_if<std::string>(&Ops[I++]);
			const auto *Os = std::get_if<std::string>(&Ops[I++]);
			const auto *Vs = std::get_if<std::string>(&Ops[I++]);
			if(!Cs || !Os || !Vs)
				return false;
			Branch.emplace_back(*Cs, *Os, *Vs);
		}
		OutBranches.push_back(std::move(Branch));
	}
	OutEnd = I;
	return true;
}

void AppendDnfOperands(std::vector<Value> &Ops, const std::vector<std::vector<RowTriple>> &Dnf) {
	Ops.push_back(static_cast<int64_t>(Dnf.size()));
	for(const auto &Branch : Dnf) {
		Ops.push_back(static_cast<int64_t>(Branch.size()));
		for(const auto &[Col, O, V] : Branch) {
			Ops.push_back(Col);
			Ops.push_back(O);
			Ops.push_back(V);
		}
	}
}

void MergeConsecutiveDnfs(const std::vector<std::vector<RowTriple>> &A, const std::vector<std::vector<RowTriple>> &B,
                          std::vector<std::vector<RowTriple>> &Out) {
	Out.clear();
	Out.reserve(A.size() * B.size());
	for(const auto &Ba : A) {
		for(const auto &Bb : B) {
			std::vector<RowTriple> Branch = Ba;
			Branch.insert(Branch.end(), Bb.begin(), Bb.end());
			Out.push_back(std::move(Branch));
		}
	}
}

struct StackSlot {
	bool Known = false;
	int64_t Value = 0;
};

void ClearStackOnBoundary(const Instruction &Inst, std::vector<StackSlot> &Stack) {
	if(Inst.HasSideEffects() || Inst.IsTerminator() || Inst.Opcode_ == Opcode::POP || Inst.Opcode_ == Opcode::CALL ||
	   Inst.Opcode_ == Opcode::RET)
		Stack.clear();
}

bool EliminateDeadPureBeforePop(Bytecode &Code) {
	bool Modified = false;
	for(size_t I = 0; I + 3 < Code.size(); ++I) {
		if(Code[I + 3].Opcode_ != Opcode::POP)
			continue;
		int64_t A = 0;
		int64_t B = 0;
		if(!IsPushInt64(Code[I], A) || !IsPushInt64(Code[I + 1], B))
			continue;
		if(!Code[I + 2].IsPure() || !Code[I + 2].Operands.empty())
			continue;
		Code[I] = MakeInstruction(Opcode::NOP);
		Code[I + 1] = MakeInstruction(Opcode::NOP);
		Code[I + 2] = MakeInstruction(Opcode::NOP);
		Modified = true;
	}
	return Modified;
}

std::optional<int64_t> FoldUnary(Opcode Op, int64_t A) {
	if(Op == Opcode::NOT)
		return static_cast<int64_t>(!A);
	return std::nullopt;
}

void MarkRecursiveCteRegions(const Bytecode &Code, std::vector<bool> &Keep) {
	for(size_t I = 0; I < Code.size(); ++I) {
		if(Code[I].Opcode_ != Opcode::RECURSIVE_CTE_FIXPOINT || Code[I].Operands.size() < 4)
			continue;
		const auto *LoopStart = std::get_if<int64_t>(&Code[I].Operands[3]);
		if(!LoopStart || *LoopStart < 0)
			continue;
		const size_t Start = static_cast<size_t>(*LoopStart);
		if(Start > I)
			continue;
		for(size_t J = Start; J <= I; ++J)
			Keep[J] = true;
	}
}

std::vector<size_t> CompactBytecode(Bytecode &Code, const std::vector<bool> &Keep) {
	std::vector<size_t> Remap(Code.size(), kIpRemoved);
	Bytecode Out;
	Out.reserve(Code.size());
	for(size_t I = 0; I < Code.size(); ++I) {
		if(!Keep[I])
			continue;
		Remap[I] = Out.size();
		Out.push_back(std::move(Code[I]));
	}
	Code = std::move(Out);
	return Remap;
}

int64_t MapIpOperand(int64_t OldIp, const std::vector<size_t> &Remap) {
	if(OldIp < 0)
		return OldIp;
	const size_t O = static_cast<size_t>(OldIp);
	if(O >= Remap.size())
		return OldIp;
	if(Remap[O] != kIpRemoved)
		return static_cast<int64_t>(Remap[O]);
	for(size_t J = O; J < Remap.size(); ++J) {
		if(Remap[J] != kIpRemoved)
			return static_cast<int64_t>(Remap[J]);
	}
	for(size_t J = O; J > 0; --J) {
		if(Remap[J - 1] != kIpRemoved)
			return static_cast<int64_t>(Remap[J - 1]);
	}
	return 0;
}

void ApplyIpRemap(Bytecode &Code, const std::vector<size_t> &Remap) {
	for(Instruction &Inst : Code) {
		switch(Inst.Opcode_) {
		case Opcode::RECURSIVE_CTE_FIXPOINT:
			if(Inst.Operands.size() >= 4) {
				if(auto *LoopStart = std::get_if<int64_t>(&Inst.Operands[3]))
					*LoopStart = MapIpOperand(*LoopStart, Remap);
			}
			break;
		case Opcode::JMP:
		case Opcode::CALL:
			if(!Inst.Operands.empty()) {
				if(auto *Target = std::get_if<int64_t>(&Inst.Operands[0]))
					*Target = MapIpOperand(*Target, Remap);
			}
			break;
		default:
			break;
		}
	}
}

void CollectJumpTargets(const Bytecode &Code, std::unordered_set<size_t> &Targets) {
	for(size_t I = 0; I < Code.size(); ++I) {
		const Instruction &Inst = Code[I];
		if(Inst.Opcode_ == Opcode::RECURSIVE_CTE_FIXPOINT && Inst.Operands.size() >= 4) {
			if(const auto *LoopStart = std::get_if<int64_t>(&Inst.Operands[3])) {
				if(*LoopStart >= 0)
					Targets.insert(static_cast<size_t>(*LoopStart));
			}
		} else if((Inst.Opcode_ == Opcode::JMP || Inst.Opcode_ == Opcode::CALL) && !Inst.Operands.empty()) {
			if(const auto *Target = std::get_if<int64_t>(&Inst.Operands[0])) {
				if(*Target >= 0)
					Targets.insert(static_cast<size_t>(*Target));
			}
		}
	}
}

bool TryFoldPushPushOp(Bytecode &Code, size_t I, Bytecode &Out, std::vector<size_t> &Remap, bool &Modified) {
	int64_t A = 0;
	int64_t B = 0;
	if(I + 2 >= Code.size() || !IsPushInt64(Code[I], A) || !IsPushInt64(Code[I + 1], B))
		return false;
	const Instruction &Op = Code[I + 2];
	if(!Op.IsPure() || !Op.Operands.empty())
		return false;

	const size_t OutIdx = Out.size();
	if(A == 0 || B == 0) {
		if(Op.Opcode_ == Opcode::MUL) {
			Remap[I] = OutIdx;
			Remap[I + 1] = OutIdx;
			Remap[I + 2] = OutIdx;
			Out.push_back(MakeInstruction(Opcode::PUSH, static_cast<int64_t>(0)));
			Modified = true;
			return true;
		}
		if(Op.Opcode_ == Opcode::DIV && A == 0) {
			Remap[I] = OutIdx;
			Remap[I + 1] = OutIdx;
			Remap[I + 2] = OutIdx;
			Out.push_back(MakeInstruction(Opcode::PUSH, static_cast<int64_t>(0)));
			Modified = true;
			return true;
		}
	}
	if(A == B && (Op.Opcode_ == Opcode::EQ || Op.Opcode_ == Opcode::NE)) {
		Remap[I] = OutIdx;
		Remap[I + 1] = OutIdx;
		Remap[I + 2] = OutIdx;
		const int64_t Truth = Op.Opcode_ == Opcode::EQ ? 1 : 0;
		Out.push_back(MakeInstruction(Opcode::PUSH, Truth));
		Modified = true;
		return true;
	}
	if(auto Folded = FoldBinary(Op.Opcode_, A, B)) {
		Remap[I] = OutIdx;
		Remap[I + 1] = OutIdx;
		Remap[I + 2] = OutIdx;
		Out.push_back(MakeInstruction(Opcode::PUSH, *Folded));
		Modified = true;
		return true;
	}
	if(Op.Opcode_ == Opcode::ADD && A == 0) {
		Remap[I] = OutIdx;
		Remap[I + 1] = OutIdx;
		Out.push_back(Code[I + 1]);
		Modified = true;
		return true;
	}
	if(Op.Opcode_ == Opcode::ADD && B == 0) {
		Remap[I] = OutIdx;
		Remap[I + 1] = OutIdx;
		Out.push_back(Code[I]);
		Modified = true;
		return true;
	}
	if(Op.Opcode_ == Opcode::MUL && A == 1) {
		Remap[I] = OutIdx;
		Remap[I + 1] = OutIdx;
		Out.push_back(Code[I + 1]);
		Modified = true;
		return true;
	}
	if(Op.Opcode_ == Opcode::MUL && B == 1) {
		Remap[I] = OutIdx;
		Remap[I + 1] = OutIdx;
		Out.push_back(Code[I]);
		Modified = true;
		return true;
	}
	if(Op.Opcode_ == Opcode::SUB && B == 0) {
		Remap[I] = OutIdx;
		Remap[I + 1] = OutIdx;
		Out.push_back(Code[I]);
		Modified = true;
		return true;
	}
	if(A == B) {
		if(Op.Opcode_ == Opcode::SUB) {
			Remap[I] = OutIdx;
			Remap[I + 1] = OutIdx;
			Remap[I + 2] = OutIdx;
			Out.push_back(MakeInstruction(Opcode::PUSH, static_cast<int64_t>(0)));
			Modified = true;
			return true;
		}
		if(Op.Opcode_ == Opcode::DIV && A != 0) {
			Remap[I] = OutIdx;
			Remap[I + 1] = OutIdx;
			Remap[I + 2] = OutIdx;
			Out.push_back(MakeInstruction(Opcode::PUSH, static_cast<int64_t>(1)));
			Modified = true;
			return true;
		}
		if(Op.Opcode_ == Opcode::MOD && A != 0) {
			Remap[I] = OutIdx;
			Remap[I + 1] = OutIdx;
			Remap[I + 2] = OutIdx;
			Out.push_back(MakeInstruction(Opcode::PUSH, static_cast<int64_t>(0)));
			Modified = true;
			return true;
		}
	}
	if(Op.Opcode_ == Opcode::AND && (A == 0 || B == 0)) {
		Remap[I] = OutIdx;
		Remap[I + 1] = OutIdx;
		Remap[I + 2] = OutIdx;
		Out.push_back(MakeInstruction(Opcode::PUSH, static_cast<int64_t>(0)));
		Modified = true;
		return true;
	}
	if(Op.Opcode_ == Opcode::OR && (A == 1 || B == 1)) {
		Remap[I] = OutIdx;
		Remap[I + 1] = OutIdx;
		Remap[I + 2] = OutIdx;
		Out.push_back(MakeInstruction(Opcode::PUSH, static_cast<int64_t>(1)));
		Modified = true;
		return true;
	}
	if(Op.Opcode_ == Opcode::OR && A == 0) {
		Remap[I] = OutIdx;
		Remap[I + 1] = OutIdx;
		Out.push_back(Code[I + 1]);
		Modified = true;
		return true;
	}
	if(Op.Opcode_ == Opcode::OR && B == 0) {
		Remap[I] = OutIdx;
		Remap[I + 1] = OutIdx;
		Out.push_back(Code[I]);
		Modified = true;
		return true;
	}
	if(Op.Opcode_ == Opcode::DIV && B == 1) {
		Remap[I] = OutIdx;
		Remap[I + 1] = OutIdx;
		Out.push_back(Code[I]);
		Modified = true;
		return true;
	}
	if(Op.Opcode_ == Opcode::MOD && B == 1) {
		Remap[I] = OutIdx;
		Remap[I + 1] = OutIdx;
		Remap[I + 2] = OutIdx;
		Out.push_back(MakeInstruction(Opcode::PUSH, static_cast<int64_t>(0)));
		Modified = true;
		return true;
	}
	return false;
}

bool TryStrengthReduceMul(Bytecode &Code, size_t I, Bytecode &Out, std::vector<size_t> &Remap, bool &Modified) {
	int64_t A = 0;
	int64_t B = 0;
	if(I + 2 >= Code.size() || !IsPushInt64(Code[I], A) || !IsPushInt64(Code[I + 1], B))
		return false;
	if(Code[I + 2].Opcode_ != Opcode::MUL || !Code[I + 2].Operands.empty())
		return false;
	if(!IsPowerOfTwo(B) || B == 1)
		return false;
	const size_t OutIdx = Out.size();
	Remap[I] = OutIdx;
	Out.push_back(Code[I]);
	if(B != 2)
		return false;
	Remap[I + 1] = OutIdx + 1;
	Out.push_back(Code[I]);
	Remap[I + 2] = OutIdx + 2;
	Out.push_back(MakeInstruction(Opcode::ADD));
	Modified = true;
	return true;
}

void RunBasicPasses(Bytecode &Code, Logger *Logger, const int Rounds) {
	const auto RunPass = [&](OptimizationPass &Pass) -> bool { return Pass.Run(Code, Logger); };
	ConstantPropagationPass CProp;
	PeepholePass Peephole;
	ConstantFoldingPass Fold;
	PredicatePushdownPass Pushdown;
	DeadCodeEliminationPass Dce;
	for(int Round = 0; Round < Rounds; ++Round) {
		const bool Changed = RunPass(CProp) || RunPass(Peephole) || RunPass(Fold) || RunPass(Pushdown);
		if(!Changed)
			break;
	}
	RunPass(Dce);
}

void RunAdvancedPasses(Bytecode &Code, Logger *Logger, const int Rounds) {
	const auto RunPass = [&](OptimizationPass &Pass) -> bool { return Pass.Run(Code, Logger); };
	PeepholePass Peephole;
	InstructionCombiningPass Combine;
	JumpThreadingPass Thread;
	for(int Round = 0; Round < Rounds; ++Round) {
		const bool Changed = RunPass(Peephole) || RunPass(Combine) || RunPass(Thread);
		if(!Changed)
			break;
	}
}

} // namespace

bool ValidateBytecodeControlFlow(const Bytecode &Code) noexcept {
	for(size_t I = 0; I < Code.size(); ++I) {
		const Instruction &Inst = Code[I];
		if(Inst.Opcode_ == Opcode::RECURSIVE_CTE_FIXPOINT) {
			if(Inst.Operands.size() < 4)
				return false;
			const auto *LoopStart = std::get_if<int64_t>(&Inst.Operands[3]);
			if(!LoopStart || *LoopStart < 0)
				return false;
			if(static_cast<size_t>(*LoopStart) >= I)
				return false;
		} else if((Inst.Opcode_ == Opcode::JMP || Inst.Opcode_ == Opcode::CALL) && !Inst.Operands.empty()) {
			const auto *Target = std::get_if<int64_t>(&Inst.Operands[0]);
			if(!Target || *Target < 0 || static_cast<size_t>(*Target) >= Code.size())
				return false;
		}
	}
	return true;
}

void RemapBytecodeIpOperands(Bytecode &Code) {
	if(!ValidateBytecodeControlFlow(Code))
		return;
}

void RunOptimizerPipeline(Bytecode &Code, const OptimizationLevel OptLevel, Logger *Logger) {
	if(OptLevel == OptimizationLevel::None || Code.empty())
		return;
	if(Logger)
		Logger->Info("Applying bytecode optimizations");

	std::optional<Bytecode> Backup;
	if(OptLevel >= OptimizationLevel::Basic)
		Backup = Code;
	const int BasicRounds = PeepholeRoundsFor(OptLevel);
	const int AdvRounds = AdvancedRoundsFor(OptLevel);

	if(OptLevel >= OptimizationLevel::Basic)
		RunBasicPasses(Code, Logger, BasicRounds);
	if(OptLevel >= OptimizationLevel::Advanced)
		RunAdvancedPasses(Code, Logger, AdvRounds);
	if(OptLevel >= OptimizationLevel::Aggressive)
		UnreachableBlockPass().Run(Code, Logger);
	if(OptLevel >= OptimizationLevel::Maximum) {
		RunAdvancedPasses(Code, Logger, AdvRounds);
		AlgebraicSimplificationPass().Run(Code, Logger);
		LogicalSimplificationPass().Run(Code, Logger);
		IdentityEliminationPass().Run(Code, Logger);
		StrengthReductionPass().Run(Code, Logger);
		DeadCodeEliminationPass().Run(Code, Logger);
		RunBasicPasses(Code, Logger, BasicRounds);
		RunAdvancedPasses(Code, Logger, AdvRounds);
		DeadCodeEliminationPass().Run(Code, Logger);
		RunBasicPasses(Code, Logger, BasicRounds);
		DeadCodeEliminationPass().Run(Code, Logger);
	}

	if(!ValidateBytecodeControlFlow(Code)) {
		if(Logger)
			Logger->Info("Optimizer: control-flow invalid after passes; reverting bytecode");
		if(Backup)
			Code = std::move(*Backup);
	}
}

bool ConstantFoldingPass::Run(Bytecode &Code, Logger *Logger) {
	if(Logger)
		Logger->Info("Running constant folding optimization");
	bool Modified = false;

	for(size_t I = 0; I < Code.size(); ++I) {
		Instruction &Inst = Code[I];
		if(!Inst.IsPure() || Inst.Operands.empty())
			continue;

		std::vector<int64_t> Constants;
		Constants.reserve(Inst.Operands.size());
		for(const auto &Op : Inst.Operands) {
			if(const auto *Val = std::get_if<int64_t>(&Op))
				Constants.push_back(*Val);
			else {
				Constants.clear();
				break;
			}
		}
		if(Constants.empty())
			continue;

		std::optional<int64_t> Folded;
		if(Constants.size() == 1)
			Folded = FoldUnary(Inst.Opcode_, Constants[0]);
		else if(Constants.size() >= 2)
			Folded = FoldBinary(Inst.Opcode_, Constants[0], Constants[1]);
		if(!Folded)
			continue;

		Inst = MakeInstruction(Opcode::PUSH, *Folded);
		Modified = true;
	}

	return Modified;
}

bool PeepholePass::Run(Bytecode &Code, Logger *Logger) {
	if(Logger)
		Logger->Info("Running peephole optimization");
	if(Code.empty())
		return false;

	Bytecode Out;
	Out.reserve(Code.size());
	std::vector<size_t> Remap(Code.size(), kIpRemoved);
	bool Modified = false;

	for(size_t I = 0; I < Code.size();) {
		if(Code[I].Opcode_ == Opcode::NOP) {
			Modified = true;
			++I;
			continue;
		}

		if(TryFoldPushPushOp(Code, I, Out, Remap, Modified)) {
			I += 3;
			continue;
		}
		if(TryStrengthReduceMul(Code, I, Out, Remap, Modified)) {
			I += 3;
			continue;
		}

		if(I + 2 < Code.size()) {
			int64_t A = 0;
			if(IsPushInt64(Code[I], A) && Code[I + 1].Opcode_ == Opcode::NOT && Code[I + 1].Operands.empty() &&
			   Code[I + 2].Opcode_ == Opcode::NOT && Code[I + 2].Operands.empty()) {
				Remap[I] = Out.size();
				Remap[I + 1] = Out.size();
				Remap[I + 2] = Out.size();
				Out.push_back(Code[I]);
				Modified = true;
				I += 3;
				continue;
			}
		}

		if(I + 1 < Code.size()) {
			int64_t A = 0;
			if(IsPushInt64(Code[I], A) && Code[I + 1].Opcode_ == Opcode::NOT && Code[I + 1].Operands.empty()) {
				Remap[I] = Out.size();
				Out.push_back(MakeInstruction(Opcode::PUSH, static_cast<int64_t>(!A)));
				Modified = true;
				I += 2;
				continue;
			}
		}

		Remap[I] = Out.size();
		Out.push_back(Code[I]);
		++I;
	}

	if(!Modified)
		return false;

	Code = std::move(Out);
	ApplyIpRemap(Code, Remap);
	return true;
}

bool ConstantPropagationPass::Run(Bytecode &Code, Logger *Logger) {
	if(Logger)
		Logger->Info("Running constant propagation");
	if(Code.empty())
		return false;
	bool Modified = false;
	std::vector<StackSlot> Stack;
	Stack.reserve(32);
	for(size_t I = 0; I < Code.size(); ++I) {
		Instruction &Inst = Code[I];
		if(Inst.Opcode_ == Opcode::PUSH) {
			if(const auto *V = std::get_if<int64_t>(&Inst.Operands[0])) {
				Stack.push_back(StackSlot{true, *V});
				continue;
			}
			Stack.clear();
			continue;
		}
		if(Inst.Opcode_ == Opcode::POP) {
			if(!Stack.empty())
				Stack.pop_back();
			continue;
		}
		if(Inst.IsPure() && Inst.Operands.empty() && Stack.size() >= 2) {
			const StackSlot B = Stack.back();
			Stack.pop_back();
			const StackSlot A = Stack.back();
			Stack.pop_back();
			if(A.Known && B.Known) {
				if(const auto Folded = FoldBinary(Inst.Opcode_, A.Value, B.Value)) {
					if(I >= 2) {
						Code[I - 2] = MakeInstruction(Opcode::NOP);
						Code[I - 1] = MakeInstruction(Opcode::NOP);
					}
					Inst = MakeInstruction(Opcode::PUSH, *Folded);
					Stack.push_back(StackSlot{true, *Folded});
					Modified = true;
					continue;
				}
			}
			Stack.push_back(StackSlot{});
			continue;
		}
		ClearStackOnBoundary(Inst, Stack);
	}
	return Modified;
}

bool AlgebraicSimplificationPass::Run(Bytecode &Code, Logger *Logger) {
	if(Logger)
		Logger->Info("Running algebraic simplification (via peephole)");
	return PeepholePass().Run(Code, Logger);
}

bool LogicalSimplificationPass::Run(Bytecode &Code, Logger *Logger) {
	if(Logger)
		Logger->Info("Running logical simplification (via peephole)");
	return PeepholePass().Run(Code, Logger);
}

bool IdentityEliminationPass::Run(Bytecode &Code, Logger *Logger) {
	if(Logger)
		Logger->Info("Running identity elimination (via peephole)");
	return PeepholePass().Run(Code, Logger);
}

bool StrengthReductionPass::Run(Bytecode &Code, Logger *Logger) {
	if(Logger)
		Logger->Info("Running strength reduction");
	if(Code.empty())
		return false;
	Bytecode Out;
	Out.reserve(Code.size());
	std::vector<size_t> Remap(Code.size(), kIpRemoved);
	bool Modified = false;
	for(size_t I = 0; I < Code.size();) {
		if(Code[I].Opcode_ == Opcode::NOP) {
			++I;
			continue;
		}
		if(TryStrengthReduceMul(Code, I, Out, Remap, Modified)) {
			I += 3;
			continue;
		}
		Remap[I] = Out.size();
		Out.push_back(Code[I]);
		++I;
	}
	if(!Modified)
		return false;
	Code = std::move(Out);
	ApplyIpRemap(Code, Remap);
	return true;
}

bool PredicatePushdownPass::Run(Bytecode &Code, Logger *Logger) {
	if(Logger)
		Logger->Info("Running predicate pushdown");
	if(Code.size() < 2)
		return false;
	bool Modified = false;
	for(size_t I = 0; I + 1 < Code.size(); ++I) {
		if(Code[I].Opcode_ != Opcode::FILTER_DNF || Code[I + 1].Opcode_ != Opcode::FILTER_DNF)
			continue;
		std::vector<std::vector<RowTriple>> Left;
		std::vector<std::vector<RowTriple>> Right;
		size_t End = 0;
		if(!ReadDnfOperands(Code[I].Operands, 0, End, Left) || End != Code[I].Operands.size())
			continue;
		if(!ReadDnfOperands(Code[I + 1].Operands, 0, End, Right) || End != Code[I + 1].Operands.size())
			continue;
		std::vector<std::vector<RowTriple>> Merged;
		MergeConsecutiveDnfs(Left, Right, Merged);
		Code[I].Operands.clear();
		AppendDnfOperands(Code[I].Operands, Merged);
		Code[I + 1] = MakeInstruction(Opcode::NOP);
		Modified = true;
	}
	if(Modified)
		DeadCodeEliminationPass().Run(Code, Logger);
	return Modified;
}

bool DeadCodeEliminationPass::Run(Bytecode &Code, Logger *Logger) {
	if(Logger)
		Logger->Info("Running dead code elimination");
	if(Code.empty())
		return false;

	bool Modified = EliminateDeadPureBeforePop(Code);

	std::vector<bool> Keep(Code.size(), true);
	MarkRecursiveCteRegions(Code, Keep);

	std::unordered_set<size_t> JumpTargets;
	CollectJumpTargets(Code, JumpTargets);
	for(const size_t T : JumpTargets)
		if(T < Keep.size())
			Keep[T] = true;

	for(size_t I = 0; I < Code.size(); ++I) {
		if(Code[I].Opcode_ == Opcode::NOP)
			Keep[I] = false;
		if(Code[I].HasSideEffects() || Code[I].IsTerminator())
			Keep[I] = true;
	}

	std::optional<size_t> LastHalt;
	for(size_t I = 0; I < Code.size(); ++I) {
		if(Code[I].Opcode_ == Opcode::HALT)
			LastHalt = I;
	}
	if(LastHalt.has_value()) {
		for(size_t I = *LastHalt + 1; I < Code.size(); ++I)
			Keep[I] = false;
	}

	for(bool K : Keep) {
		if(!K) {
			Modified = true;
			break;
		}
	}
	if(!Modified)
		return Modified;

	const std::vector<size_t> Remap = CompactBytecode(Code, Keep);
	ApplyIpRemap(Code, Remap);
	return true;
}

bool InstructionCombiningPass::Run(Bytecode &Code, Logger *Logger) {
	if(Logger)
		Logger->Info("Running instruction combining optimization");
	bool Modified = false;
	for(size_t I = 0; I + 1 < Code.size(); ++I) {
		int64_t Lit = 0;
		if(IsPushInt64(Code[I], Lit) && Lit == 0 && Code[I + 1].Opcode_ == Opcode::ADD && Code[I + 1].Operands.empty()) {
			Code[I] = MakeInstruction(Opcode::NOP);
			Modified = true;
		}
	}
	if(Modified)
		DeadCodeEliminationPass().Run(Code, Logger);
	return Modified;
}

bool JumpThreadingPass::Run(Bytecode &Code, Logger *Logger) {
	if(Logger)
		Logger->Info("Running jump threading optimization");
	bool Modified = false;
	for(size_t I = 0; I < Code.size(); ++I) {
		if(Code[I].Opcode_ != Opcode::JMP || Code[I].Operands.empty())
			continue;
		auto *Cur = std::get_if<int64_t>(&Code[I].Operands[0]);
		if(!Cur || *Cur < 0)
			continue;
		for(int Guard = 0; Guard < 32; ++Guard) {
			const size_t Target = static_cast<size_t>(*Cur);
			if(Target >= Code.size() || Code[Target].Opcode_ != Opcode::JMP ||
			   Code[Target].Operands.empty())
				break;
			const auto *Next = std::get_if<int64_t>(&Code[Target].Operands[0]);
			if(!Next || *Next < 0)
				break;
			*Cur = *Next;
			Modified = true;
		}
	}
	return Modified;
}

bool UnreachableBlockPass::Run(Bytecode &Code, Logger *Logger) {
	if(Logger)
		Logger->Info("Running unreachable block elimination");
	if(Code.empty())
		return false;
	std::vector<bool> Reach(Code.size(), false);
	std::vector<size_t> Work;
	Reach[0] = true;
	Work.push_back(0);
	auto Enqueue = [&](size_t Ip) {
		if(Ip < Code.size() && !Reach[Ip]) {
			Reach[Ip] = true;
			Work.push_back(Ip);
		}
	};
	while(!Work.empty()) {
		const size_t I = Work.back();
		Work.pop_back();
		if(I + 1 < Code.size() && Code[I].Opcode_ != Opcode::JMP && Code[I].Opcode_ != Opcode::HALT)
			Enqueue(I + 1);
		if(Code[I].Opcode_ == Opcode::JMP || Code[I].Opcode_ == Opcode::CALL) {
			if(const auto *T = std::get_if<int64_t>(&Code[I].Operands[0]))
				Enqueue(static_cast<size_t>(*T));
		}
		if(Code[I].Opcode_ == Opcode::RECURSIVE_CTE_FIXPOINT && Code[I].Operands.size() >= 4) {
			if(const auto *Ls = std::get_if<int64_t>(&Code[I].Operands[3]))
				Enqueue(static_cast<size_t>(*Ls));
		}
	}
	MarkRecursiveCteRegions(Code, Reach);
	bool Modified = false;
	for(size_t I = 0; I < Code.size(); ++I) {
		if(!Reach[I] && !Code[I].HasSideEffects() && Code[I].Opcode_ != Opcode::HALT) {
			Code[I] = MakeInstruction(Opcode::NOP);
			Modified = true;
		}
	}
	if(Modified)
		DeadCodeEliminationPass().Run(Code, Logger);
	return Modified;
}

} // namespace SQL
} // namespace AstralDB
