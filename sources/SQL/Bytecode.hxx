#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <variant>
#include <sstream>
#include <iostream>
#include <initializer_list>
#include <utility>
#include <concepts>

namespace AstralDB {
namespace SQL {
enum class Opcode : uint8_t {
    SELECT, INSERT, UPDATE, DELETE, CREATE_TABLE, DROP_TABLE,
    SET, WHERE, ORDER_BY, GROUP_BY, LIMIT, OFFSET,
    AND, OR, NOT, EQ, NE, LT, LE, GT, GE,
    ADD, SUB, MUL, DIV, MOD,
    PUSH, POP, LOAD, STORE,
    CALL, RET, JMP, NOP, HALT,
    GRANT, REVOKE // Permission management
};

using Value = std::variant<int64_t, double, std::string>;

struct Instruction {
    Opcode Opcode;
    std::vector<Value> Operands;

    constexpr Instruction(enum Opcode Op, std::initializer_list<Value> Ops)
        : Opcode(Op), Operands(Ops) {}

    bool operator==(const Instruction &Other) const {
        return Opcode == Other.Opcode && Operands == Other.Operands;
    }
};

using Bytecode = std::vector<Instruction>;

struct BytecodeComparator {
    bool operator()(const Bytecode &A, const Bytecode &B) const {
        if(A.size() != B.size())
            return A.size() < B.size();
        for (size_t i = 0; i < A.size(); ++i) {
            // Compare Opcode.
            if (A[i].Opcode != B[i].Opcode)
                return A[i].Opcode < B[i].Opcode;
            if (A[i].Operands.size() != B[i].Operands.size())
                return A[i].Operands.size() < B[i].Operands.size();
            for (size_t j = 0; j < A[i].Operands.size(); ++j) {
                if (A[i].Operands[j] != B[i].Operands[j])
                    return A[i].Operands[j] < B[i].Operands[j];
            }
        }
        return false;
    }
};

template<typename... Args> requires (std::convertible_to<Args, Value> && ...)
constexpr Instruction MakeInstruction(Opcode Op, Args&&... Operands) {
    return Instruction{Op, {std::forward<Args>(Operands)...}};
}

inline void AppendInstruction(Bytecode &Code, const Instruction &Inst) {
    Code.push_back(Inst);
}

inline std::string Disassemble(const Bytecode &Code) {
    std::ostringstream Out;
    for(size_t i = 0; i < Code.size(); ++i) {
        const Instruction &Inst = Code[i];
        Out << i << ": " << static_cast<int>(Inst.Opcode);
        if(!Inst.Operands.empty()) {
            Out << " [";
            for(const auto &Operand : Inst.Operands) {
                std::visit([&Out](auto &&arg) {
                    Out << arg << " ";
                }, Operand);
            }
            Out << "]";
        }
        Out << "\n";
    }
    return Out.str();
}
} 
}
