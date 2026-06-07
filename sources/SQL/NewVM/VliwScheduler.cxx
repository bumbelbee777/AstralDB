#include <SQL/NewVM/VliwBundle.hxx>

#include <algorithm>

namespace AstralDB {
namespace SQL {

namespace {

bool IsIndependent(const Instruction &Inst) {
	return Inst.IsPure() || Inst.Opcode_ == Opcode::PUSH || Inst.Opcode_ == Opcode::LOAD ||
	       Inst.Opcode_ == Opcode::STORE || Inst.Opcode_ == Opcode::NOP;
}

bool HasDataDependency(const VliwSlot &A, const VliwSlot &B) {
	if(!A.WritesReg || !B.WritesReg)
		return false;
	return A.DestReg == B.DestReg;
}

} // namespace

std::vector<VliwBundle> ScheduleBytecodeToBundles(const Bytecode &Code) {
	std::vector<VliwBundle> Bundles;
	if(Code.empty())
		return Bundles;

	VliwBundle Current{};
	Current.StartIp = 0;
	std::uint16_t NextReg = 1;

	for(std::size_t I = 0; I < Code.size(); ++I) {
		const Instruction &Inst = Code[I];
		if(!IsIndependent(Inst)) {
			if(Current.ActiveCount > 0) {
				Bundles.push_back(Current);
				Current = VliwBundle{};
				Current.StartIp = I;
			}
			VliwSlot Slot;
			Slot.Op = Inst.Opcode_;
			Slot.Operands = Inst.Operands;
			Slot.DestReg = NextReg++;
			Slot.WritesReg = Inst.Opcode_ != Opcode::NOP;
			Current.Slots[0] = Slot;
			Current.ActiveCount = 1;
			Bundles.push_back(Current);
			Current = VliwBundle{};
			Current.StartIp = I + 1;
			continue;
		}

		bool Placed = false;
		for(std::uint8_t S = 0; S < kVliwWidth; ++S) {
			if(S >= Current.ActiveCount) {
				VliwSlot Slot;
				Slot.Op = Inst.Opcode_;
				Slot.Operands = Inst.Operands;
				Slot.DestReg = NextReg++;
				Slot.WritesReg = Inst.Opcode_ != Opcode::NOP && Inst.Opcode_ != Opcode::POP;
				Current.Slots[S] = Slot;
				Current.ActiveCount = static_cast<std::uint8_t>(S + 1);
				Placed = true;
				break;
			}
			bool Conflict = false;
			for(std::uint8_t T = 0; T < Current.ActiveCount; ++T) {
				if(HasDataDependency(Current.Slots[T], Current.Slots[S])) {
					Conflict = true;
					break;
				}
			}
			if(!Conflict) {
				VliwSlot Slot;
				Slot.Op = Inst.Opcode_;
				Slot.Operands = Inst.Operands;
				Slot.DestReg = NextReg++;
				Slot.WritesReg = Inst.Opcode_ != Opcode::NOP && Inst.Opcode_ != Opcode::POP;
				Current.Slots[S] = Slot;
				if(S + 1 > Current.ActiveCount)
					Current.ActiveCount = static_cast<std::uint8_t>(S + 1);
				Placed = true;
				break;
			}
		}

		if(!Placed) {
			Bundles.push_back(Current);
			Current = VliwBundle{};
			Current.StartIp = I;
			VliwSlot Slot;
			Slot.Op = Inst.Opcode_;
			Slot.Operands = Inst.Operands;
			Slot.DestReg = NextReg++;
			Slot.WritesReg = Inst.Opcode_ != Opcode::NOP && Inst.Opcode_ != Opcode::POP;
			Current.Slots[0] = Slot;
			Current.ActiveCount = 1;
		}
	}

	if(Current.ActiveCount > 0)
		Bundles.push_back(Current);
	return Bundles;
}

} // namespace SQL
} // namespace AstralDB
