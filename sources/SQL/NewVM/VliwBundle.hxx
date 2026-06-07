#pragma once

#include <SQL/Bytecode/Bytecode.hxx>

#include <array>
#include <cstdint>
#include <vector>

namespace AstralDB {
namespace SQL {

static constexpr std::size_t kVliwWidth = 4;

struct VliwSlot {
	Opcode Op = Opcode::NOP;
	std::vector<Value> Operands;
	std::uint16_t DestReg = 0;
	std::array<std::uint16_t, 3> SrcRegs{};
	bool WritesReg = false;
};

struct VliwBundle {
	std::array<VliwSlot, kVliwWidth> Slots{};
	std::uint8_t ActiveCount = 0;
	std::size_t StartIp = 0;
};

/** Schedule independent bytecode ops into VLIW bundles. */
std::vector<VliwBundle> ScheduleBytecodeToBundles(const Bytecode &Code);

/** SSA-style register renaming for VLIW bundles. */
void RenameBundleRegisters(std::vector<VliwBundle> &Bundles, std::uint16_t PhysicalRegCount = 64);

} // namespace SQL
} // namespace AstralDB
