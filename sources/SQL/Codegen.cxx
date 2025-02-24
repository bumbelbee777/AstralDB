#include <SQL/SQL.hxx>
#include <SQL/Bytecode.hxx>
#include <string>
#include <vector>

namespace AstralDB {
namespace SQL {
Bytecode LiteralAST::EmitBytecode() const {
    Bytecode Code;
    Code.push_back(MakeInstruction(Opcode::PUSH, Value));
    return Code;
}

Bytecode TableAST::EmitBytecode() const {
    Bytecode Code;
    Code.push_back(MakeInstruction(Opcode::PUSH, TableName));
    return Code;
}

Bytecode ColumnAST::EmitBytecode() const {
    Bytecode Code;
    Code.push_back(MakeInstruction(Opcode::PUSH, ColumnName));
    return Code;
}
}
}