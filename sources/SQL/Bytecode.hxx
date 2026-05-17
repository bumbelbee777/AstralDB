#pragma once

#include <cstdint>
#include <memory_resource>
#include <vector>
#include <string>
#include <variant>
#include <sstream>
#include <iostream>
#include <string_view>
#include <unordered_map>
#include <initializer_list>
#include <utility>
#include <concepts>
#include <DS/BPlusTree.hxx>
#include <DS/HashTable.hxx>
#include <IO/Logger.hxx>

namespace AstralDB {

class Database;

namespace SQL {
enum class Opcode : uint8_t {
    SELECT, INSERT, UPDATE, DELETE, CREATE_TABLE, DROP_TABLE,
    SET, WHERE, ORDER_BY, GROUP_BY, LIMIT, OFFSET,
    KEEP_ROWS, DEDUP_ROWS,
    /** Operands: dst, lhs, rhs, mode (CompoundSetOpKind as int64), ncol, outCols..., lhsCols..., rhsCols... */
    SET_COMBINE,
    DELETE_MATCHING, UPDATE_MATCHING,
	/** Stack layout matches \c INSERT (table, optional col names, values). Operand tail after stack pop:
	 *  \e K, Explicit, DoNothing (int64 0=DO UPDATE, 1=DO NOTHING), \e NConflict, conflict cols...,
	 *  \e NSet, (col, fromExcluded int64, payload)* . */
	UPSERT,
	/** Operands: target table, source table, target key col, source key col, \e Nm, (tgtCol, fromSrc int64, payload)*,
	 *  \e Ni, (insCol, fromSrc int64, payload)*. */
	MERGE_INTO,
    FILTER_DNF,
    PUSH_POOL,
    AND, OR, NOT, EQ, NE, LT, LE, GT, GE,
    ADD, SUB, MUL, DIV, MOD,
    PUSH, POP, LOAD, STORE,
    CALL, RET, JMP, NOP, HALT,
    GRANT, REVOKE,
	CREATE_ROLE, DROP_ROLE, CREATE_SEQUENCE, DROP_SEQUENCE, GRANT_ROLE_MEMBERSHIP, REVOKE_ROLE_MEMBERSHIP,
	GRANT_COLUMN, REVOKE_COLUMN,

    // Transaction control
    BEGIN, COMMIT, ROLLBACK,
    
    // New opcodes for advanced features
    // JOIN types
    INNER_JOIN, LEFT_JOIN, RIGHT_JOIN, FULL_JOIN, CROSS_JOIN,
    
    // Advanced query features
    WITH, // For CTEs
    WINDOW, // For window functions
    PARTITION_BY, // For window partitioning
    OVER, // For window function context
    
    // String operations
    CONCAT, SUBSTRING, TRIM, LTRIM, RTRIM,
    UPPER, LOWER, REPLACE, REGEXP_MATCH,
    
    // Date/Time operations
    DATE_ADD, DATE_SUB, DATE_DIFF,
    EXTRACT_DATE, EXTRACT_TIME,
    
    // JSON operations
    JSON_EXTRACT, JSON_CONTAINS, JSON_MERGE,
    
    // Full-text search
    MATCH, AGAINST,
    
    // Advanced aggregations
    ROLLUP, CUBE, GROUPING_SETS,
    
    // Subquery support
    EXISTS, IN, ANY, ALL,
    
    // Advanced constraints
    CHECK_CONSTRAINT, FOREIGN_KEY,
    
    // Index operations
    CREATE_INDEX, DROP_INDEX,
    
    // View operations
    CREATE_VIEW, DROP_VIEW,

	/** Operands: procedure name, body SQL string, if-not-exists flag (int64). Compiles body, caches \c .abc beside session DB. */
	CREATE_PROCEDURE,
	/** Operands: procedure name, if-exists flag (int64). */
	DROP_PROCEDURE,
	/** Operands: procedure name. Loads cached \c .abc and runs it. */
	CALL_PROCEDURE,
    
    // Schema operations
    CREATE_SCHEMA, DROP_SCHEMA, ALTER_SCHEMA,
    
    // Table operations
    ALTER_TABLE, RENAME_TABLE,
    
    // Transaction control
    SAVEPOINT, ROLLBACK_TO,
    RELEASE_SAVEPOINT,
    EXPORT_DATABASE,
    IMPORT_DATABASE,
    CONVERT_TABULAR_FILES,

    /** Operand strings: dest name, source table (deep copy schema + rows). */
    CLONE_TABLE,
	/** Stack: table name. Operand: AS OF timestamp string. Keeps rows where valid_from <= ts < valid_to. */
	FILTER_AS_OF,
	/** Stack: table name. Operands: order column, pattern, define count, (symbol, packed_dnf)* */
	MATCH_RECOGNIZE,
	/** Operands: work_table, delta_table, max_iterations (int64), loop_start_ip (int64). Executes bytecode
	 *  in \c [loop_start_ip, this instruction) repeatedly, appending only new row signatures from \c delta_table
	 *  into \c work_table until no growth or \c max_iterations exceeded. */
	RECURSIVE_CTE_FIXPOINT,
    /** Operands: PARTITION count (int64, 0=no partition), PARTITION col names..., ORDER BY col, asc (int64), out col name,
     *  kind (int64: 0–2 ordinals, 3–6 running SUM/MIN/MAX/AVG, 7–8 LAG/LEAD), source column, frame offset (int64),
     *  explicit ROWS flag (int64), and when set: start kind, start offset, end kind, end offset. Without an explicit
     *  frame, aggregates use \c ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW . Table taken from stack. */
    WINDOW_ROW_NUMBER,
    /** Operands: offset rows (int64), max rows (int64). Table on stack; applies OFFSET then LIMIT in one step. */
    SLICE_RANGE,
	/** Operand: \c StorageLayout as \e int64 . Sets per-query storage hint for subsequent table scans. */
	STORAGE_HINT,
    /** Searched CASE on current row batch. Operand layout: output column name, arm count \e N (may be \c 0 for ELSE-only),
     *  then \e N repetitions of (WHEN predicate as FILTER_Dnf-style packed blob string, THEN kind \e int64 , THEN payload string),
     *  then ELSE kind \e int64 and ELSE payload string. THEN/ELSE kinds: 0=literal text, 1=copy from column named
     *  by payload, 2=NULL, 3=no-ELSE sentinel (omit column / empty). Table name popped from stack, rows updated in-place,
     *  table name pushed back. */
    CASE_EVAL,
    /** Four operands: output column name; source kind (0 literal, 1 column ref, 2 NULL); source payload string;
     *  target tag (\c SqlCastTarget as \e int64 ). Table name popped from stack; rows updated in-place. */
    CAST_EVAL,
    /** SELECT projection: operands are output column; \c ScalarSqlFn as \e int64 ; argc; then \e argc × (scalar kind,
     *  payload) using the same scalar encoding as \c CAST_EVAL sources. Table popped/pushed like \c CAST_EVAL . */
    SCALAR_FUNC_EVAL,
	/** \c INSERT … BULK fixture rows at runtime (torture-shaped five columns). Operands: table name, count, start id,
	 *  step (all int64 except table string). Avoids expanding millions of \c INSERT instructions at compile time. */
	INSERT_BULK,
};

using Value = std::variant<int64_t, double, std::string>;

struct Instruction {
    Opcode Opcode;
    std::vector<Value> Operands;

    // Default constructor
    constexpr Instruction() : Opcode(Opcode::NOP) {}

    // Constructor with opcode and initializer list
    constexpr Instruction(enum Opcode Op, std::initializer_list<Value> Ops)
        : Opcode(Op), Operands(Ops) {}

    bool operator==(const Instruction &Other) const {
        return Opcode == Other.Opcode && Operands == Other.Operands;
    }

    bool IsPure() const {
        switch (Opcode) {
            case Opcode::ADD:
            case Opcode::SUB:
            case Opcode::MUL:
            case Opcode::DIV:
            case Opcode::MOD:
            case Opcode::AND:
            case Opcode::OR:
            case Opcode::NOT:
            case Opcode::EQ:
            case Opcode::NE:
            case Opcode::LT:
            case Opcode::LE:
            case Opcode::GT:
            case Opcode::GE:
                return true;
            default:
                return false;
        }
    }

    bool HasSideEffects() const {
        switch (Opcode) {
            case Opcode::CREATE_TABLE:
            case Opcode::DROP_TABLE:
            case Opcode::CREATE_VIEW:
            case Opcode::DROP_VIEW:
			case Opcode::CREATE_PROCEDURE:
			case Opcode::DROP_PROCEDURE:
			case Opcode::CALL_PROCEDURE:
            case Opcode::INSERT:
			case Opcode::INSERT_BULK:
            case Opcode::DELETE:
            case Opcode::UPDATE:
            case Opcode::KEEP_ROWS:
            case Opcode::FILTER_DNF:
            case Opcode::DEDUP_ROWS:
            case Opcode::DELETE_MATCHING:
            case Opcode::UPDATE_MATCHING:
			case Opcode::UPSERT:
			case Opcode::MERGE_INTO:
            case Opcode::GRANT:
            case Opcode::REVOKE:
			case Opcode::CREATE_ROLE:
			case Opcode::DROP_ROLE:
			case Opcode::CREATE_SEQUENCE:
			case Opcode::DROP_SEQUENCE:
			case Opcode::GRANT_ROLE_MEMBERSHIP:
			case Opcode::REVOKE_ROLE_MEMBERSHIP:
			case Opcode::GRANT_COLUMN:
			case Opcode::REVOKE_COLUMN:
            case Opcode::ALTER_TABLE:
            case Opcode::SAVEPOINT:
            case Opcode::ROLLBACK_TO:
            case Opcode::RELEASE_SAVEPOINT:
            case Opcode::EXPORT_DATABASE:
            case Opcode::IMPORT_DATABASE:
            case Opcode::CONVERT_TABULAR_FILES:
            case Opcode::CLONE_TABLE:
			case Opcode::RECURSIVE_CTE_FIXPOINT:
            case Opcode::SET_COMBINE:
			case Opcode::GROUP_BY:
			case Opcode::ROLLUP:
			case Opcode::CUBE:
			case Opcode::GROUPING_SETS:
            case Opcode::WINDOW_ROW_NUMBER:
            case Opcode::SLICE_RANGE:
			case Opcode::CASE_EVAL:
			case Opcode::CAST_EVAL:
			case Opcode::SCALAR_FUNC_EVAL:
			case Opcode::INNER_JOIN:
            case Opcode::LEFT_JOIN:
            case Opcode::RIGHT_JOIN:
            case Opcode::FULL_JOIN:
            case Opcode::CROSS_JOIN:
                return true;
            // Stack-machine immediates are consumed by later side-effecting ops; naive DCE must not drop them.
            case Opcode::PUSH:
            case Opcode::PUSH_POOL:
                return true;
            default:
                return false;
        }
    }

    bool IsTerminator() const {
        switch (Opcode) {
            case Opcode::HALT:
            case Opcode::JMP:
            case Opcode::RET:
                return true;
            default:
                return false;
        }
    }
};

using Bytecode = std::vector<Instruction>;

using BytecodeScratchAlloc = std::pmr::polymorphic_allocator<Instruction>;
using BytecodeScratch = std::vector<Instruction, BytecodeScratchAlloc>;

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

inline void AppendInstruction(BytecodeScratch &Code, const Instruction &Inst) {
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

inline std::ostream& operator<<(std::ostream& os, const Bytecode& bc);

inline std::ostream& operator<<(std::ostream& os, const Bytecode& bc) {
    for (const auto& instr : bc) {
        os << "[" << static_cast<int>(instr.Opcode) << " ";
        for (const auto& operand : instr.Operands) {
            std::visit([&os](auto &&arg) {
                os << arg << " ";
            }, operand);
        }
        os << "] ";
    }
    return os;
}

// Optimization flags
enum class OptimizationLevel : uint8_t {
    None = 0,
    Basic = 1,    // Basic optimizations (constant folding, dead code elimination)
    Advanced = 2, // Advanced optimizations (instruction combining, register allocation)
    Aggressive = 3 // Aggressive optimizations (loop unrolling, instruction reordering)
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

// Dead code elimination
class DeadCodeEliminationPass : public OptimizationPass {
public:
    bool Run(Bytecode& Code, Logger* Logger = nullptr) override;
    const char* GetName() const override { return "DeadCodeElimination"; }
};

// Instruction combining
class InstructionCombiningPass : public OptimizationPass {
public:
    bool Run(Bytecode& Code, Logger* Logger = nullptr) override;
    const char* GetName() const override { return "InstructionCombining"; }
};

// Register allocation
class RegisterAllocationPass : public OptimizationPass {
public:
    bool Run(Bytecode& Code, Logger* Logger = nullptr) override;
    const char* GetName() const override { return "RegisterAllocation"; }
};

Bytecode BuildBytecode(Logger *Logger, OptimizationLevel OptLevel = OptimizationLevel::Basic,
                       const Database *ViewCatalogDb = nullptr);

struct CompiledBytecode {
	Bytecode Instructions;
	std::vector<std::string> StringPool;
};

void DedupBytecodeStringImmediates(Bytecode &Code, std::vector<std::string> &PoolOut);
CompiledBytecode BuildCompiledBytecode(Logger *Logger, OptimizationLevel OptLevel = OptimizationLevel::Basic,
                                       const Database *ViewCatalogDb = nullptr);

/** Evaluate a packed FILTER_Dnf blob (Codegen::PackDnfOperandsBlob layout) against a row map. false if blob invalid. */
bool EvaluatePackedWhereDnf(const Database *Db,
                            const std::unordered_map<std::string, std::string> &Row,
                            std::string_view PackedDnfBlob);

} 
}
