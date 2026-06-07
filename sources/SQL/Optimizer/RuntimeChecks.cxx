#include <SQL/Optimizer/RuntimeChecks.hxx>

namespace AstralDB {
namespace SQL {

Instruction RuntimeCheckGenerator::MakeCheckInstruction(Confidence Level, int64_t Tag) {
	return MakeInstruction(Opcode::NOP, static_cast<int64_t>(Level), Tag);
}

void RuntimeCheckGenerator::InsertPreFilterCheck(Bytecode &Code, std::size_t Ip) {
	if(Ip >= Code.size())
		return;
	Code.insert(Code.begin() + static_cast<std::ptrdiff_t>(Ip),
	            MakeCheckInstruction(Confidence::Medium, static_cast<int64_t>(Opcode::FILTER_DNF)));
}

void RuntimeCheckGenerator::InsertPostAggCheck(Bytecode &Code, std::size_t Ip) {
	if(Ip >= Code.size())
		return;
	Code.insert(Code.begin() + static_cast<std::ptrdiff_t>(Ip),
	            MakeCheckInstruction(Confidence::Medium, static_cast<int64_t>(Opcode::GROUP_BY)));
}

void RuntimeCheckGenerator::InsertChecks(Bytecode &Code, Confidence Level) {
	if(Level >= Confidence::Certain || Code.empty())
		return;

	for(std::size_t I = 0; I < Code.size(); ++I) {
		switch(Code[I].Opcode_) {
		case Opcode::FILTER_DNF:
			InsertPreFilterCheck(Code, I);
			++I;
			break;
		case Opcode::GROUP_BY:
			InsertPostAggCheck(Code, I);
			++I;
			break;
		default:
			break;
		}
	}
}

} // namespace SQL
} // namespace AstralDB
