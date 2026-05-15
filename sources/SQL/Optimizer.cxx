#include <SQL/Bytecode.hxx>
#include <IO/Logger.hxx>
#include <DS/BPlusTree.hxx>
#include <DS/HashTable.hxx>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>

namespace AstralDB {
namespace SQL {

bool ConstantFoldingPass::Run(Bytecode& Code, Logger* Logger) {
    if(Logger) Logger->Info("Running constant folding optimization");
    bool Modified = false;
    
    for(size_t i = 0; i < Code.size(); ++i) {
        auto& Inst = Code[i];
        if(!Inst.IsPure()) continue;
        
        // Check if all operands are constants
        bool AllConstants = true;
        std::vector<int64_t> Constants;
        for(const auto& Op : Inst.Operands) {
            if(auto* Val = std::get_if<int64_t>(&Op)) {
                Constants.push_back(*Val);
            } else {
                AllConstants = false;
                break;
            }
        }
        
        if(!AllConstants) continue;
        
        // Perform constant folding
        int64_t Result = 0;
        switch(Inst.Opcode) {
            case Opcode::ADD:
                Result = Constants[0] + Constants[1];
                break;
            case Opcode::SUB:
                Result = Constants[0] - Constants[1];
                break;
            case Opcode::MUL:
                Result = Constants[0] * Constants[1];
                break;
            case Opcode::DIV:
                if(Constants[1] == 0) continue; // Avoid division by zero
                Result = Constants[0] / Constants[1];
                break;
            case Opcode::MOD:
                if(Constants[1] == 0) continue; // Avoid modulo by zero
                Result = Constants[0] % Constants[1];
                break;
            case Opcode::AND:
                Result = Constants[0] && Constants[1];
                break;
            case Opcode::OR:
                Result = Constants[0] || Constants[1];
                break;
            case Opcode::NOT:
                Result = !Constants[0];
                break;
            case Opcode::EQ:
                Result = Constants[0] == Constants[1];
                break;
            case Opcode::NE:
                Result = Constants[0] != Constants[1];
                break;
            case Opcode::LT:
                Result = Constants[0] < Constants[1];
                break;
            case Opcode::LE:
                Result = Constants[0] <= Constants[1];
                break;
            case Opcode::GT:
                Result = Constants[0] > Constants[1];
                break;
            case Opcode::GE:
                Result = Constants[0] >= Constants[1];
                break;
            default:
                continue;
        }
        
        // Replace instruction with constant
        Inst = MakeInstruction(Opcode::PUSH, Result);
        Modified = true;
    }
    
    return Modified;
}

bool DeadCodeEliminationPass::Run(Bytecode& Code, Logger* Logger) {
    if(Logger) Logger->Info("Running dead code elimination");
    bool Modified = false;
    
    // Build control flow graph
    std::vector<BasicBlock> Blocks;
    std::unordered_map<size_t, size_t> InstToBlock;
    size_t CurrentBlock = 0;
    
    for(size_t i = 0; i < Code.size(); ++i) {
        if(i == 0 || Code[i-1].IsTerminator()) {
            Blocks.push_back({i, i, {}, {}, {}});
            CurrentBlock = Blocks.size() - 1;
        }
        InstToBlock[i] = CurrentBlock;
        Blocks[CurrentBlock].End = i;
        Blocks[CurrentBlock].Instructions.push_back(Code[i]);
    }
    
    // Find live instructions
    std::unordered_set<size_t> LiveInstructions;
    for(const auto& Block : Blocks) {
        for(size_t i = Block.Start; i <= Block.End; ++i) {
            if(Code[i].HasSideEffects() || Code[i].IsTerminator()) {
                LiveInstructions.insert(i);
            }
        }
    }
    
    // Remove dead instructions
    Bytecode NewCode;
    for(size_t i = 0; i < Code.size(); ++i) {
        if(LiveInstructions.count(i) > 0) {
            NewCode.push_back(Code[i]);
        } else {
            Modified = true;
        }
    }
    
    if(Modified) {
        Code = std::move(NewCode);
    }
    
    return Modified;
}

bool InstructionCombiningPass::Run(Bytecode& Code, Logger* Logger) {
    if(Logger) Logger->Info("Running instruction combining");
    bool Modified = false;
    
    for(size_t i = 0; i < Code.size() - 1; ++i) {
        auto& Current = Code[i];
        auto& Next = Code[i + 1];
        
        // Combine PUSH + PUSH into a single instruction
        if(Current.Opcode == Opcode::PUSH && Next.Opcode == Opcode::PUSH) {
            if(auto* Val1 = std::get_if<int64_t>(&Current.Operands[0])) {
                if(auto* Val2 = std::get_if<int64_t>(&Next.Operands[0])) {
                    // Combine into a single PUSH with both values
                    Current = MakeInstruction(Opcode::PUSH, *Val1, *Val2);
                    Code.erase(Code.begin() + i + 1);
                    Modified = true;
                    continue;
                }
            }
        }
        
        // Combine arithmetic operations
        if(Current.Opcode == Opcode::PUSH && Next.Opcode == Opcode::PUSH) {
            if(i + 2 < Code.size()) {
                auto& Op = Code[i + 2];
                if(Op.IsPure()) {
                    // Try to combine the operation
                    if(auto* Val1 = std::get_if<int64_t>(&Current.Operands[0])) {
                        if(auto* Val2 = std::get_if<int64_t>(&Next.Operands[0])) {
                            int64_t Result = 0;
                            switch(Op.Opcode) {
                                case Opcode::ADD: Result = *Val1 + *Val2; break;
                                case Opcode::SUB: Result = *Val1 - *Val2; break;
                                case Opcode::MUL: Result = *Val1 * *Val2; break;
                                case Opcode::DIV: 
                                    if(*Val2 != 0) Result = *Val1 / *Val2;
                                    else continue;
                                    break;
                                default: continue;
                            }
                            Current = MakeInstruction(Opcode::PUSH, Result);
                            Code.erase(Code.begin() + i + 1, Code.begin() + i + 3);
                            Modified = true;
                        }
                    }
                }
            }
        }
    }
    
    return Modified;
}

bool RegisterAllocationPass::Run(Bytecode& Code, Logger* Logger) {
	(void)Code;
	/** Stack-based VM: converting arbitrary PUSH/POP to STORE/LOAD is unsound for this interpreter. */
	if(Logger && Logger->IsVerbose())
		Logger->Info("Register allocation pass skipped (stack VM)");
	return false;
}

} // namespace SQL
} // namespace AstralDB 