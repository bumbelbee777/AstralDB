#pragma once

#include <Database/Execution/BytecodeTypes.hxx>
#include <Database/Storage/WhereDnfEval.hxx>
#include <Database/Storage/ShapeFingerprint.hxx>
#include <DS/BPlusTree.hxx>
#include <DS/HashTable.hxx>
#include <IO/Logger.hxx>

namespace AstralDB {

class Database;

namespace SQL {
enum class OptimizationLevel : uint8_t {
	None = 0,
	Basic = 1,
	Advanced = 2,
	Aggressive = 3,
	Maximum = 4
};

// Basic block structure for control flow analysis
struct BasicBlock {
    size_t Start;
    size_t End;
    std::vector<size_t> Predecessors;
    std::vector<size_t> Successors;
    std::vector<Instruction> Instructions;
};

// Optimization pass interface
class OptimizationPass {
public:
    virtual ~OptimizationPass() = default;
    virtual bool Run(Bytecode& Code, Logger* Logger = nullptr) = 0;
    virtual const char* GetName() const = 0;
};

// Constant folding optimization
class ConstantFoldingPass : public OptimizationPass {
public:
    bool Run(Bytecode& Code, Logger* Logger = nullptr) override;
    const char* GetName() const override { return "ConstantFolding"; }
};

/** Forward stack constant propagation and fold pure ops with known operands. */
class ConstantPropagationPass : public OptimizationPass {
public:
    bool Run(Bytecode& Code, Logger* Logger = nullptr) override;
    const char* GetName() const override { return "ConstantPropagation"; }
};

/** Algebraic identities (x-x, x/x, reassociation) on PUSH/PUSH/op windows. */
class AlgebraicSimplificationPass : public OptimizationPass {
public:
    bool Run(Bytecode& Code, Logger* Logger = nullptr) override;
    const char* GetName() const override { return "AlgebraicSimplification"; }
};

/** Logical short-circuit and idempotence on scalar stack bytecode. */
class LogicalSimplificationPass : public OptimizationPass {
public:
    bool Run(Bytecode& Code, Logger* Logger = nullptr) override;
    const char* GetName() const override { return "LogicalSimplification"; }
};

/** Neutral elements: x+0, x*1, x-0, x/1, x&&1, x||0, etc. */
class IdentityEliminationPass : public OptimizationPass {
public:
    bool Run(Bytecode& Code, Logger* Logger = nullptr) override;
    const char* GetName() const override { return "IdentityElimination"; }
};

/** Strength reduction (e.g. multiply by power-of-two to add/shift patterns). */
class StrengthReductionPass : public OptimizationPass {
public:
    bool Run(Bytecode& Code, Logger* Logger = nullptr) override;
    const char* GetName() const override { return "StrengthReduction"; }
};

/** Merge consecutive FILTER_DNF ops; keep predicates close to table scans. */
class PredicatePushdownPass : public OptimizationPass {
public:
    bool Run(Bytecode& Code, Logger* Logger = nullptr) override;
    const char* GetName() const override { return "PredicatePushdown"; }
};

// Dead code elimination (conservative: NOPs, unreachable tails; preserves CTE loop bodies)
class DeadCodeEliminationPass : public OptimizationPass {
public:
    bool Run(Bytecode& Code, Logger* Logger = nullptr) override;
    const char* GetName() const override { return "DeadCodeElimination"; }
};

/** Stack-machine peephole: fold PUSH/PUSH/op triples, drop redundant NOPs. */
class PeepholePass : public OptimizationPass {
public:
    bool Run(Bytecode& Code, Logger* Logger = nullptr) override;
    const char* GetName() const override { return "Peephole"; }
};

/** Re-map \c JMP/\c CALL/\c RECURSIVE_CTE_FIXPOINT IP operands after bytecode compaction. */
void RemapBytecodeIpOperands(Bytecode &Code);

/** Verify fixpoint/jump targets are in range (after optimization). */
bool ValidateBytecodeControlFlow(const Bytecode &Code) noexcept;

/** Run the optimization pipeline for \a OptLevel (reverts on broken control flow). */
void RunOptimizerPipeline(Bytecode &Code, OptimizationLevel OptLevel, Logger *Logger = nullptr);

/** Lower INSERT BULK query shapes to dedicated *_BULK opcodes (Advanced+). */
void RunBulkOpcodePass(Bytecode &Code, OptimizationLevel OptLevel, Logger *Logger = nullptr);

// Instruction combining (advanced peephole patterns)
class InstructionCombiningPass : public OptimizationPass {
public:
    bool Run(Bytecode& Code, Logger* Logger = nullptr) override;
    const char* GetName() const override { return "InstructionCombining"; }
};

class JumpThreadingPass : public OptimizationPass {
public:
    bool Run(Bytecode& Code, Logger* Logger = nullptr) override;
    const char* GetName() const override { return "JumpThreading"; }
};

class UnreachableBlockPass : public OptimizationPass {
public:
    bool Run(Bytecode& Code, Logger* Logger = nullptr) override;
    const char* GetName() const override { return "UnreachableBlock"; }
};

struct SqlSessionConfig;

Bytecode BuildBytecode(Logger *Logger, OptimizationLevel OptLevel = OptimizationLevel::Basic,
                       const Database *ViewCatalogDb = nullptr, const SqlSessionConfig *SessionCfg = nullptr);

struct CompiledBytecode {
	Bytecode Instructions;
	std::vector<std::string> StringPool;
	QueryShapeFingerprint128 ShapeFingerprint{};
};

void DedupBytecodeStringImmediates(Bytecode &Code, std::vector<std::string> &PoolOut);
CompiledBytecode BuildCompiledBytecode(Logger *Logger, OptimizationLevel OptLevel = OptimizationLevel::Basic,
                                       const Database *ViewCatalogDb = nullptr);

} 
}

