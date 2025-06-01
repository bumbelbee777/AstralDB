#pragma once

#include <Database/Database.hxx>
#include <SQL/Bytecode.hxx>
#include <cstdint>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <memory>
#include <IO/Logger.hxx>

namespace AstralDB {
namespace SQL {
class BytecodeInterpreter {
    uintptr_t Ic;
    uintptr_t Sp;
    uintptr_t Bp;
    uint32_t Flags;
    std::vector<uint64_t> Registers_;
    std::vector<uint64_t> Stack_;

    std::vector<std::unique_ptr<Database>> Databases_;
    Logger* Logger_ = nullptr;

    void CleanupStack() {
        for (auto Value : Stack_)
            if (Value > 0x1000) // Simple heuristic to detect pointers
                delete reinterpret_cast<std::string*>(Value);
        Stack_.clear();
    }

public:
    BytecodeInterpreter(Logger* Logger = nullptr) : Ic(0), Sp(0), Bp(0), Flags(0), Logger_(Logger) {
        Registers_.resize(16, 0);
    }

    ~BytecodeInterpreter() {
        CleanupStack();
    }

    void SetLogger(Logger* Logger) { Logger_ = Logger; }
    Logger* GetLogger() const { return Logger_; }

    void Execute(const Bytecode &Code);

    bool Step(const Bytecode &Code);

    void Reset() {
        CleanupStack();
        Ic = 0;
        Sp = 0;
        Bp = 0;
        Flags = 0;
        Registers_.assign(Registers_.size(), 0);
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