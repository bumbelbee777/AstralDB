#include <SQL/Bytecode/BytecodeInterpreter.hxx>
#include <SQL/Bytecode/Bytecode.hxx>
#include <SQL/Bulk/BulkOps.hxx>
#include <Database/Database.hxx>
#include <IO/Error.hxx>
#include <stdexcept>

namespace AstralDB {
namespace SQL {

namespace {
[[noreturn]] inline void FailVm(std::string Message) {
	throw std::runtime_error(AstralDB::Err::Prefixed("SQL VM", std::move(Message)));
}
} // namespace

bool BytecodeInterpreter::StepBulk(const Bytecode &Code, const Instruction &Inst) {
	(void)Code;
	switch(Inst.Opcode_) {
	case Opcode::INSERT_BULK: {
		if(Inst.Operands.size() != 4)
			FailVm("INSERT_BULK expects table, count, start, step operands");
		auto *Tbl = std::get_if<std::string>(&Inst.Operands[0]);
		auto *Cnt = std::get_if<int64_t>(&Inst.Operands[1]);
		auto *Start = std::get_if<int64_t>(&Inst.Operands[2]);
		auto *StepOp = std::get_if<int64_t>(&Inst.Operands[3]);
		if(!Tbl || !Cnt || !Start || !StepOp)
			FailVm("INSERT_BULK operand types");
		if(Databases_.empty())
			Databases_.push_back(std::make_unique<Database>(DatabasePath_));
		Databases_[0]->InsertBulkSyntheticRows(*Tbl, *Cnt, *Start, *StepOp);
		++Ic;
		return true;
	}
	default:
		if(HandleBulkOpcode(*this, Code, Inst)) {
			++Ic;
			return true;
		}
		return false;
	}
}

} // namespace SQL
} // namespace AstralDB
