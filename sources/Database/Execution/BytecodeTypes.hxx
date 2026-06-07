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

namespace AstralDB {

class Database;

namespace SQL {
enum class Opcode : uint8_t {
    SELECT, INSERT, UPDATE, DELETE, CREATE_TABLE, DROP_TABLE, CREATE_TYPE, DROP_TYPE,
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
    ADD, SUB, MUL, DIV, MOD, INT_DIV,
    PUSH, POP, LOAD, STORE,
    CALL, RET, JMP, NOP, HALT,
    GRANT, REVOKE,
	CREATE_ROLE, DROP_ROLE, CREATE_USER, DROP_USER, ALTER_USER_PASSWORD, CREATE_SEQUENCE, DROP_SEQUENCE,
	GRANT_ROLE_MEMBERSHIP, REVOKE_ROLE_MEMBERSHIP, GRANT_COLUMN, REVOKE_COLUMN,

    // Transaction control
    BEGIN, COMMIT, ROLLBACK, SET_TRANSACTION_ISOLATION,
    
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
    CREATE_VIEW, DROP_VIEW, COMMENT_ON, SHOW_TABLES, DESCRIBE_TABLE,

	/** Operands: procedure name, body SQL string, if-not-exists flag (int64). Compiles body, caches \c .abc beside session DB. */
	CREATE_PROCEDURE,
	/** Operands: procedure name, if-exists flag (int64). */
	DROP_PROCEDURE,
	/** Operands: procedure name, optional invoke kind (\c call, \c execute, \c exec). Loads cached \c .abc. */
	CALL_PROCEDURE,
	/** Procedure exception region: savepoint, end IP, then \c (handler_ip, condition)* pairs. */
	PROC_TRY,
	/** End try: savepoint name, end IP (release savepoint and jump past handlers). */
	PROC_END_TRY,
	/** Procedure branch: table name, jump IP if the table is missing or has zero rows. */
	PROC_JUMP_IF_TABLE_EMPTY,

	/** Operands: name, table, timing, event, for_each_row, action_kind, procedure, body_sql, if_not_exists, or_replace. */
	CREATE_TRIGGER,
	DROP_TRIGGER,
	/** Operands: name, enabled (int64 0/1). */
	ALTER_TRIGGER,

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
	/** Operands: table name, parent column, child column, prior_on_parent (int64), start_with DNF blob (may be empty),
	 *  no_cycle (int64). Expands rows in hierarchical order (depth-first) in-place. */
	CONNECT_BY_EXPAND,
    /** Operands: PARTITION count (int64, 0=no partition), PARTITION col names..., ORDER BY col, asc (int64), out col name,
     *  kind (int64: 0-2 ordinals, 3-6 running SUM/MIN/MAX/AVG, 7-8 LAG/LEAD, 9-11 FIRST/LAST/NTH_VALUE,
     *  12-13 PERCENT_RANK/CUME_DIST, 14 NTILE), source column, frame offset (int64),
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
	/** Operands: output column, op char (1-char string), left column, right column. Integer division uses \c INT_DIV . */
	SCALAR_ARITH_EVAL,
	/** Operands: work table, mode (\c ColumnsPickMode ), glob or serialized lambda, static select count.
	 *  Pushes matching column names onto the VM stack (like \c SELECT col ). */
	COLUMNS_EXPAND,
	/** \c INSERT … BULK fixture rows at runtime (torture-shaped five columns). Operands: table name, count, start id,
	 *  step (all int64 except table string). Avoids expanding millions of \c INSERT instructions at compile time. */
	INSERT_BULK,
	/** Operands: scratch dest, source table, COUNT(*) output column. Pushes dest table name. */
	COUNT_BULK,
	/** Operands: dest, left table, right table, left key, right key, output column. Inner-join match count. */
	JOIN_COUNT_BULK,
	/** Operands: dest, table, filter count, (col, op, lit)*, output column. Filtered COUNT on lazy bulk. */
	FILTER_COUNT_BULK,
	/** Operands: dest, table, then full \c GROUP_BY operand tail. Single-table or star GROUP BY fast path. */
	GROUP_BY_BULK,
	/** Operands: dest, customers, orders, products, filter col/op/lit, group keys, sum cols, order col, limit. */
	STAR_GROUP_BY_BULK,
	/** Operands: dest, customers, orders, products, filter tail, cube keys×4, sum/avg/count outs, having min count. */
	STAR_JOIN_CUBE_BULK,
	/** Operands: dest, fact, N dims, filter, passthrough cols, projection specs, order keys, limit. Lazy star SELECT+top-K. */
	STAR_JOIN_SELECT_BULK,
	/** Operands: dest, fact, N dims, filter, GROUP_BY operand tail, order col, desc flag, limit. Lazy star GROUP BY. */
	STAR_JOIN_GROUP_BULK,
	/** Operands: table, filter tail, projection specs, order column, asc flag, limit. Lazy-bulk semistructured top-K. */
	SEMISTRUCTURED_TOPK_BULK,
	/** Same operand layout as \c SEMISTRUCTURED_TOPK_BULK; runs semistructured bytecode VM (fused batch kernels). */
	FUSED_SEMISTRUCTURED_SCAN,
	/** Operands: dataset name, kind (int64), source table, bulk count, bulk start, bulk step. */
	REGISTER_DATASET,
	/** Operands: dataset name, target table name, version id (0 = latest). */
	LOAD_DATASET,
	/** Operands: dataset name. */
	DROP_DATASET,
	/** Operands: embedding name, table, token column, vector column. */
	REGISTER_EMBEDDING,
	/** Operands: embedding name. */
	DROP_EMBEDDING,
	/** Operands: optional table name (empty string = whole database). */
	VACUUM,
	/** Operands: table name. */
	REPACK_CONCURRENTLY,
	/** Operands: graph name, vertex table, vertex id col, edge table, src col, dst col, label col, weight col,
	 *  undirected (0/1). */
	GRAPH_REGISTER,
	/** Operands: graph name. */
	GRAPH_DROP,
	/** Operands: graph name, start id, max depth, mode (0=BFS,1=DFS), result table. */
	GRAPH_TRAVERSE,
	/** Operands: graph, label filter, result, min hops, max hops, anchor from, reverse (0/1). */
	GRAPH_MATCH,
	/** Operands: graph, result, anchor, label filter, segment_count, (min,max,reverse,end_on_prod)*N. */
	GRAPH_MATCH_BULK,
	/** Operands: graph, from id, to id, weighted (0/1), result table. */
	GRAPH_SHORTEST_PATH,
	/** Operands: graph, damping_millis, iterations, result table. */
	GRAPH_PAGERANK,
	/** Operands: projection name, base graph, edge label filter. */
	GRAPH_REGISTER_PROJECTION,

	/** Operands: work_table, delta_table, max_iterations (int64), loop_start_ip (int64). Level-batch BFS fixpoint. */
	RECURSIVE_CTE_BFS,
	/** Operands: outer_table, inner_table, outer_key, inner_key, negated (int64), inner_filter_dnf blob. */
	SEMI_JOIN_HASH,
	/** Operands: same layout as SEMI_JOIN_HASH; delegates to parallel hash join build/probe. */
	FUSED_SEMI_JOIN_EXISTS,
	/** Operands: dest_table, source_table, id_col, parent_col, path_prefix (may be empty). */
	HIERARCHY_PATH_SCAN,

	FUSED_SCAN_FILTER,
	FUSED_SCAN_FILTER_AGG,
	FUSED_SCAN_PROJECT_LIMIT,
	FUSED_JOIN_FILTER,
	FUSED_PRECOMPUTED_AGG,
};

using Value = std::variant<int64_t, double, std::string>;

struct Instruction {
    Opcode Opcode_;
    std::vector<Value> Operands;

    // Default constructor
    constexpr Instruction() : Opcode_(Opcode::NOP) {}

    // Constructor with opcode and initializer list
    constexpr Instruction(enum Opcode Op, std::initializer_list<Value> Ops)
        : Opcode_(Op), Operands(Ops) {}

    bool operator==(const Instruction &Other) const {
        return Opcode_ == Other.Opcode_ && Operands == Other.Operands;
    }

    bool IsPure() const {
        switch (Opcode_) {
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
        switch (Opcode_) {
            case Opcode::CREATE_TABLE:
            case Opcode::DROP_TABLE:
            case Opcode::CREATE_TYPE:
            case Opcode::DROP_TYPE:
            case Opcode::CREATE_VIEW:
            case Opcode::DROP_VIEW:
			case Opcode::CREATE_PROCEDURE:
			case Opcode::DROP_PROCEDURE:
			case Opcode::CALL_PROCEDURE:
			case Opcode::PROC_TRY:
			case Opcode::PROC_END_TRY:
			case Opcode::PROC_JUMP_IF_TABLE_EMPTY:
            case Opcode::INSERT:
			case Opcode::INSERT_BULK:
			case Opcode::COUNT_BULK:
			case Opcode::JOIN_COUNT_BULK:
			case Opcode::FILTER_COUNT_BULK:
			case Opcode::GROUP_BY_BULK:
			case Opcode::STAR_GROUP_BY_BULK:
			case Opcode::STAR_JOIN_CUBE_BULK:
			case Opcode::STAR_JOIN_SELECT_BULK:
			case Opcode::STAR_JOIN_GROUP_BULK:
			case Opcode::SEMISTRUCTURED_TOPK_BULK:
			case Opcode::FUSED_SEMISTRUCTURED_SCAN:
			case Opcode::REGISTER_DATASET:
			case Opcode::LOAD_DATASET:
			case Opcode::DROP_DATASET:
			case Opcode::REGISTER_EMBEDDING:
			case Opcode::DROP_EMBEDDING:
			case Opcode::VACUUM:
			case Opcode::REPACK_CONCURRENTLY:
			case Opcode::GRAPH_REGISTER:
			case Opcode::GRAPH_DROP:
			case Opcode::GRAPH_TRAVERSE:
			case Opcode::GRAPH_MATCH:
			case Opcode::GRAPH_MATCH_BULK:
			case Opcode::GRAPH_SHORTEST_PATH:
			case Opcode::GRAPH_PAGERANK:
			case Opcode::GRAPH_REGISTER_PROJECTION:
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
			case Opcode::CREATE_USER:
			case Opcode::DROP_USER:
			case Opcode::ALTER_USER_PASSWORD:
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
			case Opcode::RECURSIVE_CTE_BFS:
			case Opcode::SEMI_JOIN_HASH:
			case Opcode::FUSED_SEMI_JOIN_EXISTS:
			case Opcode::HIERARCHY_PATH_SCAN:
			case Opcode::FUSED_SCAN_FILTER:
			case Opcode::FUSED_SCAN_FILTER_AGG:
			case Opcode::FUSED_SCAN_PROJECT_LIMIT:
			case Opcode::FUSED_JOIN_FILTER:
			case Opcode::FUSED_PRECOMPUTED_AGG:
            case Opcode::CONNECT_BY_EXPAND:
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
        switch (Opcode_) {
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
            if (A[i].Opcode_ != B[i].Opcode_)
                return A[i].Opcode_ < B[i].Opcode_;
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
        Out << i << ": " << static_cast<int>(Inst.Opcode_);
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
        os << "[" << static_cast<int>(instr.Opcode_) << " ";
        for (const auto& operand : instr.Operands) {
            std::visit([&os](auto &&arg) {
                os << arg << " ";
            }, operand);
        }
        os << "] ";
    }
    return os;
}

} // namespace SQL
} // namespace AstralDB
