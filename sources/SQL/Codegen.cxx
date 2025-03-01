#include <SQL/SQL.hxx>
#include <SQL/Bytecode.hxx>

namespace AstralDB {
namespace SQL {
Bytecode LiteralAST::EmitBytecode() const {
    Bytecode Code;
    AppendInstruction(Code, MakeInstruction(Opcode::PUSH, Value));
    return Code;
}

Bytecode TableAST::EmitBytecode() const {
    Bytecode Code;
    AppendInstruction(Code, MakeInstruction(Opcode::PUSH, TableName));
    return Code;
}

/*Bytecode ColumnAST::EmitBytecode() const {
    Bytecode Code;
    AppendInstruction(MakeInstruction(Opcode::PUSH, ColumnName));
    return Code;
}*/

Bytecode CreateAST::EmitBytecode() const {
    Bytecode Code;
    AppendInstruction(Code, MakeInstruction(Opcode::CREATE_TABLE, TableName));
    for(const auto &Column : Columns) {
        AppendInstruction(Code, MakeInstruction(Opcode::PUSH, Column.Name));
        AppendInstruction(Code, MakeInstruction(Opcode::PUSH, Column.Type));
        for(auto Constraint : Column.Constraints)
            AppendInstruction(Code, MakeInstruction(Opcode::PUSH, Constraint));
    }
    return Code;
}

Bytecode SelectAST::EmitBytecode() const {
    Bytecode Code;
    for(const auto &Column : Columns)
        AppendInstruction(Code, MakeInstruction(Opcode::SELECT, Column));
    AppendInstruction(Code, MakeInstruction(Opcode::PUSH, Table->TableName));
    return Code;
}

Bytecode InsertAST::EmitBytecode() const {
    Bytecode Code;
    AppendInstruction(Code, MakeInstruction(Opcode::PUSH, Table->TableName));
    for(const auto &Column : Columns)
        AppendInstruction(Code, MakeInstruction(Opcode::PUSH, Column));
    for(const auto &Value : Values)
        AppendInstruction(Code, MakeInstruction(Opcode::PUSH, Value));
    return Code;
}

Bytecode UpdateAST::EmitBytecode() const {
    Bytecode Code;
    AppendInstruction(Code, MakeInstruction(Opcode::UPDATE, TableName));
    for(const auto &Assignment : Assignments)
        AppendInstruction(Code, MakeInstruction(Opcode::SET, Assignment.first, Assignment.second));
    if(Condition) {
        AppendInstruction(Code, MakeInstruction(Opcode::WHERE));
        Bytecode CondCode = Condition->EmitBytecode();
        Code.insert(Code.end(), CondCode.begin(), CondCode.end());
    }
    AppendInstruction(Code, MakeInstruction(Opcode::HALT));
    return Code;
}

Bytecode DeleteAST::EmitBytecode() const {
    Bytecode Code;
    AppendInstruction(Code, MakeInstruction(Opcode::DELETE, TableName));
    if(Condition) {
        AppendInstruction(Code, MakeInstruction(Opcode::WHERE));
        Bytecode CondCode = Condition->EmitBytecode();
        Code.insert(Code.end(), CondCode.begin(), CondCode.end());
    }
    AppendInstruction(Code, MakeInstruction(Opcode::HALT));
    return Code;
}

Bytecode BinaryOpAST::EmitBytecode() const {
    Bytecode Code;
    Bytecode LHSCode = LHS->EmitBytecode();
    Code.insert(Code.end(), LHSCode.begin(), LHSCode.end());
    Bytecode RHSCode = RHS->EmitBytecode();
    Code.insert(Code.end(), RHSCode.begin(), RHSCode.end());
    Opcode OpCode;
    if(Op == "+")
        OpCode = Opcode::ADD;
    else if(Op == "-")
        OpCode = Opcode::SUB;
    else if(Op == "*")
        OpCode = Opcode::MUL;
    else if(Op == "/")
        OpCode = Opcode::DIV;
    else if(Op == "%")
        OpCode = Opcode::MOD;
    else if(Op == "==")
        OpCode = Opcode::EQ;
    else if(Op == "!=")
        OpCode = Opcode::NE;
    else if(Op == "<")
        OpCode = Opcode::LT;
    else if(Op == "<=")
        OpCode = Opcode::LE;
    else if(Op == ">")
        OpCode = Opcode::GT;
    else if(Op == ">=")
        OpCode = Opcode::GE;
    else
        throw std::runtime_error("Unsupported binary operator: " + Op);
    AppendInstruction(Code, MakeInstruction(OpCode));
    return Code;
}

Bytecode BuildBytecode() {
    Bytecode Result;
    int Counter = 0;
    std::vector<Bytecode> BatchBytecodes;
    std::vector<ExpressionAST*> BatchAST;
    for(auto &Statement : AST) {
        Bytecode Dummy = Statement->EmitBytecode();
        for(auto &Inst : Dummy)
            AppendInstruction(Result, Inst);
        BatchBytecodes.push_back(Dummy);
        BatchAST.push_back(Statement.get());
        if(Counter >= 35) {
            BPlusTree<Bytecode, ExpressionAST*> Tree;
            for(size_t i = 0; i < BatchBytecodes.size(); i++)
                Tree.Insert(BatchBytecodes[i], BatchAST[i]);
            std::vector<Bytecode> SortedBytecodes = Tree.GetAllKeys();
            for(auto &BC : SortedBytecodes) {
                for(auto &Inst : BC)
                    AppendInstruction(Result, Inst);
            }
            BatchBytecodes.clear();
            BatchAST.clear();
            Counter = 0;
        } else Counter++;
    }
    if(!BatchBytecodes.empty()) {
        BPlusTree<Bytecode, ExpressionAST*> Tree;
        for(size_t i = 0; i < BatchBytecodes.size(); i++)
            Tree.Insert(BatchBytecodes[i], BatchAST[i]);
        std::vector<Bytecode> SortedBytecodes = Tree.GetAllKeys();
        for(auto &BC : SortedBytecodes) {
            for(auto &Inst : BC)
                AppendInstruction(Result, Inst);
        }
    }
    return Result;
}
}
}