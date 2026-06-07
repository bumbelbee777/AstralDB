#include <SQL/Bytecode.hxx>
#include <IO/Logger.hxx>
#include <algorithm>
#include <limits>
#include <optional>
#include <unordered_set>
#include <vector>

namespace AstralDB {
namespace SQL {

namespace {

constexpr size_t kIpRemoved = std::numeric_limits<size_t>::max();

int PeepholeRoundsFor(const OptimizationLevel Level) {
	if(Level >= OptimizationLevel::Maximum)
		return 4;
	if(Level >= OptimizationLevel::Advanced)
		return 3;
	return 2;
}

int AdvancedRoundsFor(const OptimizationLevel Level) {
	if(Level >= OptimizationLevel::Maximum)
		return 3;
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
	default:
		return std::nullopt;
	}
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
	return false;
}

void RunBasicPasses(Bytecode &Code, Logger *Logger, const int Rounds) {
	const auto RunPass = [&](OptimizationPass &Pass) -> bool { return Pass.Run(Code, Logger); };
	PeepholePass Peephole;
	ConstantFoldingPass Fold;
	DeadCodeEliminationPass Dce;
	for(int Round = 0; Round < Rounds; ++Round) {
		const bool Changed = RunPass(Peephole) || RunPass(Fold);
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

	const Bytecode Backup = Code;
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
		DeadCodeEliminationPass().Run(Code, Logger);
		RunBasicPasses(Code, Logger, BasicRounds);
		DeadCodeEliminationPass().Run(Code, Logger);
	}

	if(!ValidateBytecodeControlFlow(Code)) {
		if(Logger)
			Logger->Info("Optimizer: control-flow invalid after passes; reverting bytecode");
		Code = Backup;
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

bool DeadCodeEliminationPass::Run(Bytecode &Code, Logger *Logger) {
	if(Logger)
		Logger->Info("Running dead code elimination");
	if(Code.empty())
		return false;

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

	bool Modified = false;
	for(bool K : Keep) {
		if(!K) {
			Modified = true;
			break;
		}
	}
	if(!Modified)
		return false;

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
