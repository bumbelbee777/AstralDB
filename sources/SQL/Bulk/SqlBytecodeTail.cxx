#include <SQL/Bulk/SqlBytecodeTail.hxx>

#include <cstdint>

namespace AstralDB::SQL {

bool RemainingBytecodeOnlyConsumesSelectFinalize(const Bytecode &Code, const std::size_t StartIp) noexcept {
	for(std::size_t J = StartIp; J < Code.size(); ++J) {
		switch(Code[J].Opcode_) {
		case Opcode::NOP:
			break;
		case Opcode::HALT:
			return true;
		case Opcode::SELECT:
			if(Code[J].Operands.empty() || !std::get_if<int64_t>(&Code[J].Operands[0]))
				return false;
			break;
		default:
			return false;
		}
	}
	return true;
}

bool RemainingBytecodeOnlySemistructuredFinalize(const Bytecode &Code, const std::size_t StartIp) noexcept {
	for(std::size_t J = StartIp; J < Code.size(); ++J) {
		switch(Code[J].Opcode_) {
		case Opcode::NOP:
			break;
		case Opcode::HALT:
			return true;
		case Opcode::SELECT:
			if(Code[J].Operands.empty() || !std::get_if<int64_t>(&Code[J].Operands[0]))
				return false;
			break;
		case Opcode::PUSH:
			if(Code[J].Operands.empty() || !std::get_if<std::string>(&Code[J].Operands[0]))
				return false;
			break;
		default:
			return false;
		}
	}
	return true;
}

} // namespace AstralDB::SQL
