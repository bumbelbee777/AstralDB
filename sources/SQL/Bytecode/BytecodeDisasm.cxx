#include <SQL/Bytecode/BytecodeDisasm.hxx>
#include <SQL/Bytecode/OpcodeMeta.hxx>

#include <iomanip>
#include <sstream>

namespace AstralDB {
namespace SQL {

namespace {

std::string OperandPreview(const Value &V) {
	return std::visit(
	    [](const auto &Arg) -> std::string {
		    using T = std::decay_t<decltype(Arg)>;
		    if constexpr(std::is_same_v<T, int64_t>)
			    return std::to_string(Arg);
		    else if constexpr(std::is_same_v<T, double>)
			    return std::to_string(Arg);
		    else
			    return "\"" + std::string(Arg) + "\"";
	    },
	    V);
}

} // namespace

std::string OpcodeName(Opcode Op) {
	const std::string_view View = OpcodeNameView(Op);
	if(View == "?")
		return "OP_" + std::to_string(static_cast<int>(Op));
	return std::string(View);
}

std::string DisassemblePretty(const Bytecode &Code) {
	std::ostringstream Out;
	for(std::size_t I = 0; I < Code.size(); ++I) {
		const Instruction &Inst = Code[I];
		Out << std::setw(5) << I << "  " << OpcodeName(Inst.Opcode_);
		if(!Inst.Operands.empty()) {
			Out << "  ";
			for(std::size_t O = 0; O < Inst.Operands.size(); ++O) {
				if(O)
					Out << ", ";
				Out << OperandPreview(Inst.Operands[O]);
			}
		}
		Out << "\n";
	}
	return Out.str();
}

} // namespace SQL
} // namespace AstralDB
