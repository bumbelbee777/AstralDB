#include <SQL/NewVM/VliwBundle.hxx>

namespace AstralDB {
namespace SQL {

void RenameBundleRegisters(std::vector<VliwBundle> &Bundles, std::uint16_t PhysicalRegCount) {
	std::uint16_t NextPhys = 0;
	for(VliwBundle &Bundle : Bundles) {
		for(std::uint8_t S = 0; S < Bundle.ActiveCount; ++S) {
			VliwSlot &Slot = Bundle.Slots[S];
			if(Slot.WritesReg) {
				Slot.DestReg = NextPhys % PhysicalRegCount;
				++NextPhys;
			}
			for(std::uint16_t &Src : Slot.SrcRegs) {
				if(Src != 0)
					Src = Src % PhysicalRegCount;
			}
		}
	}
}

} // namespace SQL
} // namespace AstralDB
