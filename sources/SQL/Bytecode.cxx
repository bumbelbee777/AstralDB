#include <SQL/BytecodeInterpreter.hxx>
#include <SQL/Bytecode.hxx>
#include <iostream>
#include <stdexcept>
#include <memory>

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
            // Support int64_t and string for PUSH
            if (auto val = std::get_if<int64_t>(&inst.Operands[0])) {
                Push(static_cast<uint64_t>(*val));
            } else if (std::holds_alternative<std::string>(inst.Operands[0])) {
                // For string, could push to a string stack or handle as needed
                // For now, ignore (or extend interpreter as needed)
            } else {
                throw std::runtime_error("PUSH only supports int64_t or string operand");
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
        // Control flow: JMP
        case Opcode::JMP: {
            if (inst.Operands.empty()) throw std::runtime_error("JMP requires target operand");
            if (auto target = std::get_if<int64_t>(&inst.Operands[0])) {
                if (*target < 0 || static_cast<size_t>(*target) >= Code.size())
                    throw std::runtime_error("JMP target out of range");
                Ic = static_cast<uintptr_t>(*target);
            } else {
                throw std::runtime_error("JMP expects int64_t operand");
            }
            break;
        }
        // Control flow: CALL (push return address, jump)
        case Opcode::CALL: {
            if (inst.Operands.empty()) throw std::runtime_error("CALL requires target operand");
            if (auto target = std::get_if<int64_t>(&inst.Operands[0])) {
                if (*target < 0 || static_cast<size_t>(*target) >= Code.size())
                    throw std::runtime_error("CALL target out of range");
                Push(Ic + 1); // Push return address
                Ic = static_cast<uintptr_t>(*target);
            } else {
                throw std::runtime_error("CALL expects int64_t operand");
            }
            break;
        }
        // Control flow: RET (pop return address)
        case Opcode::RET: {
            if (Stack_.empty()) throw std::runtime_error("RET with empty stack");
            Ic = static_cast<uintptr_t>(Pop());
            break;
        }
        // Arithmetic: SUB
        case Opcode::SUB: {
            uint64_t b = Pop();
            uint64_t a = Pop();
            Push(a - b);
            ++Ic;
            break;
        }
        // Arithmetic: MUL
        case Opcode::MUL: {
            uint64_t b = Pop();
            uint64_t a = Pop();
            Push(a * b);
            ++Ic;
            break;
        }
        // Arithmetic: DIV
        case Opcode::DIV: {
            uint64_t b = Pop();
            uint64_t a = Pop();
            if (b == 0) throw std::runtime_error("DIV by zero");
            Push(a / b);
            ++Ic;
            break;
        }
        // Arithmetic: MOD
        case Opcode::MOD: {
            uint64_t b = Pop();
            uint64_t a = Pop();
            if (b == 0) throw std::runtime_error("MOD by zero");
            Push(a % b);
            ++Ic;
            break;
        }
        // Database operations
        case Opcode::CREATE_TABLE: {
            if (inst.Operands.empty()) throw std::runtime_error("CREATE_TABLE requires table name operand");
            if (auto tableName = std::get_if<std::string>(&inst.Operands[0])) {
                if (Databases_.empty()) {
                    Databases_.push_back(std::make_unique<Database>(std::filesystem::path("astral.db")));
                }
                Databases_[0]->CreateTable(*tableName, {}).get();
            } else {
                throw std::runtime_error("CREATE_TABLE expects string operand");
            }
            ++Ic;
            break;
        }
        case Opcode::DROP_TABLE: {
            if (inst.Operands.empty()) throw std::runtime_error("DROP_TABLE requires table name operand");
            if (auto tableName = std::get_if<std::string>(&inst.Operands[0])) {
                if (Databases_.empty()) {
                    Databases_.push_back(std::make_unique<Database>(std::filesystem::path("astral.db")));
                }
                Databases_[0]->DropTable(*tableName).get();
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
                    if (Databases_.empty()) {
                        Databases_.push_back(std::make_unique<Database>(std::filesystem::path("astral.db")));
                    }
                    std::unordered_map<std::string, std::string> row;
                    row["value"] = *value; // Demo: single column
                    Databases_[0]->Insert(*tableName, row).get();
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
                if (Databases_.empty()) {
                    Databases_.push_back(std::make_unique<Database>(std::filesystem::path("astral.db")));
                }
                Databases_[0]->Delete(*tableName, [](const std::unordered_map<std::string, std::string>&) { return true; }).get();
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
                        if (Databases_.empty()) {
                            Databases_.push_back(std::make_unique<Database>(std::filesystem::path("astral.db")));
                        }
                        std::unordered_map<std::string, std::string> newValues;
                        newValues[*column] = *value;
                        Databases_[0]->Update(*tableName, [](const std::unordered_map<std::string, std::string>&) { return true; }, newValues).get();
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
                if (Databases_.empty()) {
                    Databases_.push_back(std::make_unique<Database>(std::filesystem::path("astral.db")));
                }
                auto result = Databases_[0]->Select(*tableName, [](const std::unordered_map<std::string, std::string>&) { return true; }).get();
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
        // Database operations: SET, WHERE, ORDER_BY, GROUP_BY, LIMIT, OFFSET
        case Opcode::SET:
        case Opcode::WHERE:
        case Opcode::ORDER_BY:
        case Opcode::GROUP_BY:
        case Opcode::LIMIT:
        case Opcode::OFFSET: {
            // These would typically modify query state or context
            // For now, just print a message and continue
            std::cout << "Opcode not yet implemented: " << static_cast<int>(inst.Opcode) << std::endl;
            ++Ic;
            break;
        }
        // Logical/comparison opcodes
        case Opcode::AND: {
            uint64_t b = Pop();
            uint64_t a = Pop();
            Push((a && b) ? 1 : 0);
            ++Ic;
            break;
        }
        case Opcode::OR: {
            uint64_t b = Pop();
            uint64_t a = Pop();
            Push((a || b) ? 1 : 0);
            ++Ic;
            break;
        }
        case Opcode::NOT: {
            uint64_t a = Pop();
            Push(!a ? 1 : 0);
            ++Ic;
            break;
        }
        case Opcode::EQ: {
            uint64_t b = Pop();
            uint64_t a = Pop();
            Push((a == b) ? 1 : 0);
            ++Ic;
            break;
        }
        case Opcode::NE: {
            uint64_t b = Pop();
            uint64_t a = Pop();
            Push((a != b) ? 1 : 0);
            ++Ic;
            break;
        }
        case Opcode::LT: {
            uint64_t b = Pop();
            uint64_t a = Pop();
            Push((a < b) ? 1 : 0);
            ++Ic;
            break;
        }
        case Opcode::LE: {
            uint64_t b = Pop();
            uint64_t a = Pop();
            Push((a <= b) ? 1 : 0);
            ++Ic;
            break;
        }
        case Opcode::GT: {
            uint64_t b = Pop();
            uint64_t a = Pop();
            Push((a > b) ? 1 : 0);
            ++Ic;
            break;
        }
        case Opcode::GE: {
            uint64_t b = Pop();
            uint64_t a = Pop();
            Push((a >= b) ? 1 : 0);
            ++Ic;
            break;
        }
        // LOAD/STORE (registers)
        case Opcode::LOAD: {
            if (inst.Operands.empty()) throw std::runtime_error("LOAD requires register index operand");
            if (auto reg = std::get_if<int64_t>(&inst.Operands[0])) {
                if (*reg < 0 || static_cast<size_t>(*reg) >= Registers_.size())
                    throw std::runtime_error("LOAD register index out of range");
                Push(Registers_[*reg]);
            } else {
                throw std::runtime_error("LOAD expects int64_t operand");
            }
            ++Ic;
            break;
        }
        case Opcode::STORE: {
            if (inst.Operands.empty()) throw std::runtime_error("STORE requires register index operand");
            if (auto reg = std::get_if<int64_t>(&inst.Operands[0])) {
                if (*reg < 0 || static_cast<size_t>(*reg) >= Registers_.size())
                    throw std::runtime_error("STORE register index out of range");
                Registers_[*reg] = Pop();
            } else {
                throw std::runtime_error("STORE expects int64_t operand");
            }
            ++Ic;
            break;
        }
        // GRANT/REVOKE (permissions)
        case Opcode::GRANT: {
            if (inst.Operands.size() < 2) throw std::runtime_error("GRANT requires user and permission operands");
            if (auto user = std::get_if<std::string>(&inst.Operands[0])) {
                if (auto Perms = std::get_if<int64_t>(&inst.Operands[1])) {
                    if (Databases_.empty()) Databases_.push_back(std::make_unique<Database>(std::filesystem::path("astral.db")));
                    Databases_[0]->GrantPermission(*user, static_cast<Permissions>(*Perms)).get();
                } else {
                    throw std::runtime_error("GRANT expects int64_t permission operand");
                }
            } else {
                throw std::runtime_error("GRANT expects string user operand");
            }
            ++Ic;
            break;
        }
        case Opcode::REVOKE: {
            if (inst.Operands.size() < 2) throw std::runtime_error("REVOKE requires user and permission operands");
            if (auto user = std::get_if<std::string>(&inst.Operands[0])) {
                if (auto Perms = std::get_if<int64_t>(&inst.Operands[1])) {
                    if (Databases_.empty()) Databases_.push_back(std::make_unique<Database>(std::filesystem::path("astral.db")));
                    Databases_[0]->RevokePermission(*user, static_cast<Permissions>(*Perms)).get();
                } else {
                    throw std::runtime_error("REVOKE expects int64_t permission operand");
                }
            } else {
                throw std::runtime_error("REVOKE expects string user operand");
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
