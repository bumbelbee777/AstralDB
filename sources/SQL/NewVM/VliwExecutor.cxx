#include <SQL/NewVM/VliwExecutor.hxx>

#include <IO/Job.hxx>

#include <atomic>
#include <future>
#include <vector>

namespace AstralDB {
namespace SQL {

namespace {

int64_t PopInt64(const std::vector<Value> &Ops, std::size_t Idx) {
	if(Idx >= Ops.size())
		return 0;
	if(const auto *V = std::get_if<int64_t>(&Ops[Idx]))
		return *V;
	return 0;
}

int64_t EvalPure(const VliwSlot &Slot, const std::vector<int64_t> &Regs) {
	const int64_t A = Slot.SrcRegs[0] < Regs.size() ? Regs[Slot.SrcRegs[0]] : PopInt64(Slot.Operands, 0);
	const int64_t B = Slot.SrcRegs[1] < Regs.size() ? Regs[Slot.SrcRegs[1]] : PopInt64(Slot.Operands, 1);
	switch(Slot.Op) {
	case Opcode::ADD:
		return A + B;
	case Opcode::SUB:
		return A - B;
	case Opcode::MUL:
		return A * B;
	case Opcode::DIV:
		return B != 0 ? A / B : 0;
	case Opcode::AND:
		return static_cast<int64_t>(A && B);
	case Opcode::OR:
		return static_cast<int64_t>(A || B);
	case Opcode::NOT:
		return static_cast<int64_t>(!A);
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
	case Opcode::PUSH:
		return PopInt64(Slot.Operands, 0);
	default:
		return 0;
	}
}

} // namespace

VliwExecutor::VliwExecutor(ExecContext *Ctx) : Ctx_(Ctx) {
	if(Ctx_ && Ctx_->RegFile.empty())
		Ctx_->RegFile.resize(64, 0);
}

void VliwExecutor::ExecuteSlot(const VliwSlot &Slot) {
	if(!Ctx_)
		return;
	if(Slot.Op == Opcode::NOP)
		return;
	const int64_t Result = EvalPure(Slot, Ctx_->RegFile);
	if(Slot.WritesReg && Slot.DestReg < Ctx_->RegFile.size())
		Ctx_->RegFile[Slot.DestReg] = Result;
}

void VliwExecutor::ExecuteBundle(const VliwBundle &Bundle) {
	if(Bundle.ActiveCount <= 1 || !Ctx_) {
		for(std::uint8_t S = 0; S < Bundle.ActiveCount; ++S)
			ExecuteSlot(Bundle.Slots[S]);
		return;
	}

	struct SlotResult {
		std::uint16_t Dest = 0;
		int64_t Value = 0;
		bool Write = false;
	};

	std::vector<SlotResult> Results(Bundle.ActiveCount);
	std::vector<std::future<void>> Futs;
	Futs.reserve(Bundle.ActiveCount);
	auto &Jobs = JobSystem::Instance();

	for(std::uint8_t S = 0; S < Bundle.ActiveCount; ++S) {
		const VliwSlot Slot = Bundle.Slots[S];
		Futs.push_back(Jobs.SubmitAsync([this, Slot, &Results, S]() {
			if(Slot.Op == Opcode::NOP)
				return;
			Results[S].Dest = Slot.DestReg;
			Results[S].Value = EvalPure(Slot, Ctx_->RegFile);
			Results[S].Write = Slot.WritesReg;
		}));
	}
	for(auto &F : Futs)
		F.wait();

	for(const SlotResult &R : Results) {
		if(R.Write && R.Dest < Ctx_->RegFile.size())
			Ctx_->RegFile[R.Dest] = R.Value;
	}
}

void VliwExecutor::ExecuteAll(const std::vector<VliwBundle> &Bundles, bool Parallel) {
	(void)Parallel;
	for(const VliwBundle &B : Bundles)
		ExecuteBundle(B);
}

} // namespace SQL
} // namespace AstralDB
