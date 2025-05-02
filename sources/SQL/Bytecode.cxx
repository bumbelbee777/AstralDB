// Implements BytecodeInterpreter methods
#include <SQL/BytecodeInterpreter.hxx>
#include <SQL/Bytecode.hxx>
#include <iostream>
#include <stdexcept>

namespace AstralDB {
namespace SQL {

void BytecodeInterpreter::Execute(const Bytecode &Code) {
    Reset();
    while (Ic < Code.size()) {
        if (!Step(Code)) break;
    }
}

bool BytecodeInterpreter::Step(const Bytecode &Code) {
    if (Ic >= Code.size()) return false;
    const Instruction &inst = Code[Ic];
    switch (inst.Opcode) {
        case Opcode::NOP:
            ++Ic;
            break;
        case Opcode::HALT:
            return false;
        case Opcode::PUSH: {
            if (inst.Operands.empty()) throw std::runtime_error("PUSH requires operand");
            // Only int64_t supported for now
            if (auto val = std::get_if<int64_t>(&inst.Operands[0])) {
                Push(static_cast<uint64_t>(*val));
            } else {
                throw std::runtime_error("PUSH only supports int64_t operand");
            }
            ++Ic;
            break;
        }
        case Opcode::POP: {
            Pop();
            ++Ic;
            break;
        }
        // Arithmetic example: ADD
        case Opcode::ADD: {
            uint64_t b = Pop();
            uint64_t a = Pop();
            Push(a + b);
            ++Ic;
            break;
        }
        // Database operations
        case Opcode::CREATE_TABLE: {
            if (inst.Operands.empty()) throw std::runtime_error("CREATE_TABLE requires table name operand");
            if (auto tableName = std::get_if<std::string>(&inst.Operands[0])) {
                if (Databases_.empty()) Databases_.emplace_back("astral.db");
                // For demo: no columns support yet
                Databases_[0].CreateTable(*tableName, {}).get();
            } else {
                throw std::runtime_error("CREATE_TABLE expects string operand");
            }
            ++Ic;
            break;
        }
        case Opcode::DROP_TABLE: {
            if (inst.Operands.empty()) throw std::runtime_error("DROP_TABLE requires table name operand");
            if (auto tableName = std::get_if<std::string>(&inst.Operands[0])) {
                if (Databases_.empty()) Databases_.emplace_back("astral.db");
                Databases_[0].DropTable(*tableName).get();
            } else {
                throw std::runtime_error("DROP_TABLE expects string operand");
            }
            ++Ic;
            break;
        }
        case Opcode::INSERT: {
            if (inst.Operands.size() < 2) throw std::runtime_error("INSERT requires table name and value operand");
            if (auto tableName = std::get_if<std::string>(&inst.Operands[0])) {
                if (auto value = std::get_if<std::string>(&inst.Operands[1])) {
                    if (Databases_.empty()) Databases_.emplace_back("astral.db");
                    std::unordered_map<std::string, std::string> row;
                    row["value"] = *value; // Demo: single column
                    Databases_[0].Insert(*tableName, row).get();
                } else {
                    throw std::runtime_error("INSERT expects string value operand");
                }
            } else {
                throw std::runtime_error("INSERT expects string table name operand");
            }
            ++Ic;
            break;
        }
        case Opcode::DELETE: {
            if (inst.Operands.empty()) throw std::runtime_error("DELETE requires table name operand");
            if (auto tableName = std::get_if<std::string>(&inst.Operands[0])) {
                if (Databases_.empty()) Databases_.emplace_back("astral.db");
                Databases_[0].Delete(*tableName, [](const std::unordered_map<std::string, std::string>&) { return true; }).get();
            } else {
                throw std::runtime_error("DELETE expects string operand");
            }
            ++Ic;
            break;
        }
        case Opcode::UPDATE: {
            if (inst.Operands.size() < 3) throw std::runtime_error("UPDATE requires table name, column, and value operands");
            if (auto tableName = std::get_if<std::string>(&inst.Operands[0])) {
                if (auto column = std::get_if<std::string>(&inst.Operands[1])) {
                    if (auto value = std::get_if<std::string>(&inst.Operands[2])) {
                        if (Databases_.empty()) Databases_.emplace_back("astral.db");
                        std::unordered_map<std::string, std::string> newValues;
                        newValues[*column] = *value;
                        Databases_[0].Update(*tableName, [](const std::unordered_map<std::string, std::string>&) { return true; }, newValues).get();
                    } else {
                        throw std::runtime_error("UPDATE expects string value operand");
                    }
                } else {
                    throw std::runtime_error("UPDATE expects string column operand");
                }
            } else {
                throw std::runtime_error("UPDATE expects string table name operand");
            }
            ++Ic;
            break;
        }
        case Opcode::SELECT: {
            if (inst.Operands.empty()) throw std::runtime_error("SELECT requires table name operand");
            if (auto tableName = std::get_if<std::string>(&inst.Operands[0])) {
                if (Databases_.empty()) Databases_.emplace_back("astral.db");
                auto result = Databases_[0].Select(*tableName, [](const std::unordered_map<std::string, std::string>&) { return true; }).get();
                std::cout << "SELECT result from table '" << *tableName << "':\n";
                for (const auto& row : result) {
                    for (const auto& [col, val] : row) {
                        std::cout << col << ": " << val << ", ";
                    }
                    std::cout << std::endl;
                }
            } else {
                throw std::runtime_error("SELECT expects string operand");
            }
            ++Ic;
            break;
        }
        // TODO: Implement more opcodes, including database operations
        default:
            std::cerr << "Unimplemented opcode: " << static_cast<int>(inst.Opcode) << std::endl;
            throw std::runtime_error("Unimplemented opcode");
    }
    return true;
}

} // namespace SQL
} // namespace AstralDB
