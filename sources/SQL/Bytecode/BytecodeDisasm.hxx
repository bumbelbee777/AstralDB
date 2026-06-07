#pragma once

#include <SQL/Bytecode/Bytecode.hxx>
#include <string>

namespace AstralDB {
namespace SQL {

std::string OpcodeName(Opcode Op);
std::string DisassemblePretty(const Bytecode &Code);

} // namespace SQL
} // namespace AstralDB
