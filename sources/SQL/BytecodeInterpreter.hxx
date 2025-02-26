#pragma once

#include <SQL/Bytecode.hxx>
#include <cstdint>
#include <vector>
#include <iostream>
#include <stdexcept>

namespace AstralDB {
namespace SQL {
class BytecodeInterpreter {
    uintptr_t Ic;
    uintptr_t Sp;
    uintptr_t Bp;
    uint32_t Flags;
    std::vector<uint64_t> Registers_;
    std::vector<uint64_t> Stack_;
public:
    BytecodeInterpreter() : Ic(0), Sp(0), Bp(0), Flags(0) {
        Registers_.resize(16, 0);
    }

    void Execute(const Bytecode &Code);

    bool Step(const Bytecode &Code);

    void Reset() {
        Ic = 0;
        Sp = 0;
        Bp = 0;
        Flags = 0;
        Registers_.assign(Registers_.size(), 0);
        Stack_.clear();
    }

    uintptr_t CurrentInstruction() const { return Ic; }

    uintptr_t StackBase() const { return Bp; }

    uintptr_t StackTop() const { return Sp; }

    std::vector<uint64_t> Registers() const { return Registers_; }

    void Push(uint64_t Value) {
        Stack_.push_back(Value);
        Sp = Stack_.size();
    }

    uint64_t Pop() {
        if(Stack_.empty())
            throw std::runtime_error("Stack underflow");
        uint64_t Value = Stack_.back();
        Stack_.pop_back();
        Sp = Stack_.size();
        return Value;
    }

    void DumpRegs() const {
        std::cout << "Registers:\n";
        std::cout << "IC: " << Ic << "\n";
        std::cout << "SP: " << Sp << "\n";
        std::cout << "BP: " << Bp << "\n";
        std::cout << "Flags:" << Flags << "\n";
        for(size_t i = 0; i < Registers_.size(); ++i)
            std::cout << "R" << i << ": " << Registers_[i] << "\n";
    }
};
}
}