#pragma once

#include <IO/Logger.hxx>
#include <Database/User.hxx>
#include <SQL/Bytecode.hxx>
#include <DS/Tree.hxx>
#include <DS/BPlusTree.hxx>
#include <DS/RadixTree.hxx>
#include <cstdio>
#include <stdexcept>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <optional>

namespace AstralDB {

class Database;

namespace SQL {
enum class TokenType {
    IDENTIFIER,
    KEYWORD,
    PUNCTUATION,
    LITERAL,
    WHITESPACE,
    EOF_,
    SYMBOL,
    SELECT,
    CREATE,
    INSERT,
    BEGIN,
    COMMIT,
    ROLLBACK,
    COMMENT
};

struct Token {
    TokenType Type;
    std::string Value;
    /** Byte offset in the original query where this token starts (for diagnostics). */
    std::size_t Begin = 0;
};

using TokenStream = std::vector<Token>;

struct ColumnDefinition {
    std::string Name;
    std::string Type;
    std::vector<std::string> Constraints;

    ColumnDefinition(std::string name, std::string type, std::vector<std::string> constraints = {})
        : Name(std::move(name)), Type(std::move(type)), Constraints(std::move(constraints)) {}
};

enum class TableConstraintKind { PrimaryKey, Unique, ForeignKey, Check };

struct TableConstraintDef {
    TableConstraintKind Kind = TableConstraintKind::PrimaryKey;
    /** Optional name from CONSTRAINT symbol */
    std::string Name;
    std::vector<std::string> Columns;
    std::string RefTable;
    std::string RefColumn;
    std::string CheckSql;
};

enum class AlterTableKind { AddColumn, DropColumn, RenameColumn };

enum class SavepointStmtKind { Set, Release, RollbackTo };

struct StatementAST {
    virtual ~StatementAST() = default;
    virtual void EmitBytecode(BytecodeScratch& Instructions) const = 0;
};

struct ExpressionAST : public StatementAST {
    virtual ~ExpressionAST() = default;
    virtual void EmitBytecode(BytecodeScratch& Instructions) const = 0;
};

struct LiteralAST : public ExpressionAST {
    std::string Value;

    explicit LiteralAST(std::string Value) : Value(std::move(Value)) {}
    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

struct NullLiteralAST : public ExpressionAST {
    NullLiteralAST() = default;
    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

struct InValuesAST : public ExpressionAST {
    std::vector<std::string> Values;

    explicit InValuesAST(std::vector<std::string> Values) : Values(std::move(Values)) {}
    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

struct BetweenAST : public ExpressionAST {
    std::unique_ptr<ExpressionAST> Subject;
    std::unique_ptr<ExpressionAST> Low;
    std::unique_ptr<ExpressionAST> High;

    BetweenAST(std::unique_ptr<ExpressionAST> Subject, std::unique_ptr<ExpressionAST> Low,
               std::unique_ptr<ExpressionAST> High)
        : Subject(std::move(Subject)), Low(std::move(Low)), High(std::move(High)) {}
    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

struct IsNullPredAST : public ExpressionAST {
    std::unique_ptr<ExpressionAST> Subject;
    bool Negated = false;

    IsNullPredAST(std::unique_ptr<ExpressionAST> Subject, bool Negated)
        : Subject(std::move(Subject)), Negated(Negated) {}
    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

/** EXISTS (SELECT * FROM T [WHERE ...]); inner WHERE is DNF-folded. Correlated refs use outer row columns; only inner-row keys that appear in T's schema overlay the enclosing row (inner values win on duplicate declared column names). */
struct ExistsPredAST : public ExpressionAST {
    bool Negated = false;
    std::string InnerTable;
    std::unique_ptr<ExpressionAST> InnerWhere;
    ExistsPredAST(bool Neg, std::string Tbl, std::unique_ptr<ExpressionAST> W)
        : Negated(Neg), InnerTable(std::move(Tbl)), InnerWhere(std::move(W)) {}
    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

/** Target family for `CAST(... AS type)` (cells stay strings; selects conversion rules in the VM). */
enum class SqlCastTarget : int8_t { Text = 0, Integer = 1, Real = 2, Boolean = 3 };

/** Searched-only `CASE WHEN … THEN … [ELSE …] END` for SELECT projections (lowered via \c Opcode::CASE_EVAL ). */
struct CaseExprAST : public ExpressionAST {
    struct Arm {
        std::unique_ptr<ExpressionAST> When;
        std::unique_ptr<ExpressionAST> Then;
    };
    std::vector<Arm> Arms;
    std::unique_ptr<ExpressionAST> ElseResult;

    CaseExprAST(std::vector<Arm> A, std::unique_ptr<ExpressionAST> Else)
        : Arms(std::move(A)), ElseResult(std::move(Else)) {}

    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

/** `CAST(expr AS type)` in SELECT projections; lowered via \c Opcode::CAST_EVAL . */
struct CastExprAST : public ExpressionAST {
    std::unique_ptr<ExpressionAST> Operand;
    SqlCastTarget Target = SqlCastTarget::Text;

    CastExprAST(std::unique_ptr<ExpressionAST> OperandIn, SqlCastTarget T)
        : Operand(std::move(OperandIn)), Target(T) {}

    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

struct ColumnRefAST : public ExpressionAST {
    std::string Name;

    explicit ColumnRefAST(std::string Name) : Name(std::move(Name)) {}
    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

struct TableAST : public ExpressionAST {
    std::string TableName;

    explicit TableAST(std::string Name) : TableName(std::move(Name)) {}
    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

struct CreateAST : public ExpressionAST {
    std::string TableName;
    std::vector<ColumnDefinition> Columns;
    std::vector<TableConstraintDef> TableConstraints;
    bool IfNotExists = false;

    explicit CreateAST(std::string TableName, std::vector<ColumnDefinition> Columns, bool IfNotExists = false)
        : TableName(std::move(TableName)), Columns(std::move(Columns)), IfNotExists(IfNotExists) {}
    CreateAST(std::string TableName, std::vector<ColumnDefinition> Columns,
              std::vector<TableConstraintDef> TableConstraints, bool IfNotExists = false)
        : TableName(std::move(TableName))
        , Columns(std::move(Columns))
        , TableConstraints(std::move(TableConstraints))
        , IfNotExists(IfNotExists) {}
    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

struct AlterTableAST : public StatementAST {
    AlterTableKind Kind = AlterTableKind::AddColumn;
    std::string TableName;
    ColumnDefinition AddedColumn = ColumnDefinition("", "TEXT");
    std::string DropColumnName;
    std::string RenameFrom;
    std::string RenameTo;

    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

struct SavepointAST : public StatementAST {
    SavepointStmtKind Kind = SavepointStmtKind::Set;
    std::string Name;

    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

enum class DataExchangeKind { ExportDatabase, ImportDatabase, ConvertFiles };

struct DataExchangeAST : public StatementAST {
    DataExchangeKind Kind = DataExchangeKind::ExportDatabase;
    /** EXPORT / IMPORT target path */
    std::string Path;
    /** FORMAT for EXPORT/IMPORT: JSON, CSV, TSV */
    std::string Format = "JSON";
    /** CONVERT destination path */
    std::string DestPath;
    /** CONVERT destination format */
    std::string DestFormat = "CSV";

    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

struct DropAST : public StatementAST {
    std::string TableName;
    bool IfExists = false;

    explicit DropAST(std::string TableName, bool IfExists = false)
        : TableName(std::move(TableName)), IfExists(IfExists) {}
    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

enum class TransactionType {
    BEGIN,
    COMMIT,
    ROLLBACK
};

class TransactionAST : public StatementAST {
public:
    explicit TransactionAST(TransactionType Type) : Type_(Type) {}
    
    void EmitBytecode(BytecodeScratch& Instructions) const override {
        switch (Type_) {
            case TransactionType::BEGIN:
                Instructions.push_back(MakeInstruction(Opcode::BEGIN));
                break;
            case TransactionType::COMMIT:
                Instructions.push_back(MakeInstruction(Opcode::COMMIT));
                break;
            case TransactionType::ROLLBACK:
                Instructions.push_back(MakeInstruction(Opcode::ROLLBACK));
                break;
        }
    }
    
private:
    TransactionType Type_;
};

enum class GroupAggMode { None, CountStar, CountDistinct };

/** GROUP BY aggregates beyond COUNT(*); codegen packs into Opcode::GROUP_BY extended layout. */
enum class GroupCombAggKind : int { Sum = 0, Min = 1, Max = 2, Avg = 3 };

struct GroupCombAgg {
    GroupCombAggKind Kind = GroupCombAggKind::Sum;
    std::string SourceColumn;
    std::string OutputColumn;
};

enum class SqlJoinKind { Inner, Left, Right, Full, Cross };

struct JoinClause {
    SqlJoinKind Kind = SqlJoinKind::Inner;
    std::string RightTable;
    /** Equality JOIN: left row[key] compares to right row[key] for each pair. */
    std::vector<std::pair<std::string, std::string>> OnPairs;
};

class SelectAST : public StatementAST {
public:
    SelectAST(std::vector<std::string> Columns,
              std::string Table,
              std::unique_ptr<ExpressionAST> WhereClause = nullptr,
              std::unique_ptr<ExpressionAST> HavingClause = nullptr,
              std::vector<std::pair<std::string, bool>> OrderByColumns = {},
              int64_t Limit = -1,
              int64_t Offset = 0,
              bool Distinct = false,
              std::vector<std::string> GroupByColumns = {},
              GroupAggMode AggMode = GroupAggMode::None,
              std::optional<std::string> CountDistinctColumn = std::nullopt,
              std::optional<std::pair<std::string, bool>> RowNumberOverOrderBy = {},
              std::vector<std::string> RowNumberPartitionByColumns = {},
              std::string RowNumberAlias = "rn",
              int WindowOrdinalKind = 0,
              std::vector<JoinClause> Joins = {},
              std::vector<GroupCombAgg> CombinedAggs = {},
              std::vector<std::unique_ptr<ExpressionAST>> ProjectionExprsIn = {},
              std::string CountAggregateOutputColumn = {})
        : Columns_(std::move(Columns))
        , Table_(std::move(Table))
        , WhereClause_(std::move(WhereClause))
        , HavingClause_(std::move(HavingClause))
        , OrderByColumns_(std::move(OrderByColumns))
        , Limit_(Limit)
        , Offset_(Offset)
        , Distinct_(Distinct)
        , GroupByColumns_(std::move(GroupByColumns))
        , AggMode_(AggMode)
        , CountDistinctColumn_(std::move(CountDistinctColumn))
        , RowNumberOverOrderBy_(std::move(RowNumberOverOrderBy))
        , RowNumberPartitionByColumns_(std::move(RowNumberPartitionByColumns))
        , RowNumberAlias_(std::move(RowNumberAlias))
        , WindowOrdinalKind_(WindowOrdinalKind)
        , Joins_(std::move(Joins))
        , CombinedAggs_(std::move(CombinedAggs))
        , CountAggregateOutputColumn_(std::move(CountAggregateOutputColumn)) {
        if(ProjectionExprsIn.empty()) {
            ProjectionExprs_.clear();
            ProjectionExprs_.resize(Columns_.size());
        } else
            ProjectionExprs_ = std::move(ProjectionExprsIn);
        if(ProjectionExprs_.size() != Columns_.size())
            throw std::invalid_argument(
                "SELECT projection expression list must align 1:1 with the SELECT column list.");
    }
    
    void EmitBytecode(BytecodeScratch& Instructions) const override;

    /** Like EmitBytecode but run the destructive SELECT pipeline against ScratchTable (for CTEs). */
    void EmitBytecodeForScratchTable(const std::string &ScratchPhysicalName,
                                      BytecodeScratch& Instructions) const;

    /** FROM table (after parser CTE substitution). Used by WITH materialization. */
    const std::string &SourceTableName() const { return Table_; }
    const std::vector<std::string> &GroupKeys() const { return GroupByColumns_; }
    GroupAggMode AggKind() const { return AggMode_; }
    const std::optional<std::string> &CountDistinctColumn() const { return CountDistinctColumn_; }
    const std::optional<std::pair<std::string, bool>> &RowNumberOrderSpec() const {
        return RowNumberOverOrderBy_;
    }
    int WindowOrdinalKind() const { return WindowOrdinalKind_; }
    const std::vector<std::string> &RowNumberPartitionByColumns() const { return RowNumberPartitionByColumns_; }
    const std::string &RowNumberOutColumn() const { return RowNumberAlias_; }
    bool DistinctSelected() const { return Distinct_; }
    const std::vector<std::pair<std::string, bool>> &OrderBySpecs() const { return OrderByColumns_; }
    int64_t SelectLimitValue() const { return Limit_; }
    int64_t SelectOffsetValue() const { return Offset_; }
    const ExpressionAST *WhereRoot() const { return WhereClause_.get(); }
    const ExpressionAST *HavingRoot() const { return HavingClause_.get(); }
    const std::vector<JoinClause> &JoinSpecs() const { return Joins_; }
    const std::vector<GroupCombAgg> &ComboAggs() const { return CombinedAggs_; }
    const std::vector<std::string> &ProjectionColumns() const { return Columns_; }
    const std::vector<std::unique_ptr<ExpressionAST>> &ProjectionExprs() const { return ProjectionExprs_; }

	/** Output column name for \c COUNT(*) / \c COUNT(DISTINCT…) in this SELECT (empty when no count aggregate). */
	const std::string &CountAggregateOutputColumn() const { return CountAggregateOutputColumn_; }

    /** Parser attaches trailing ORDER BY / LIMIT after the FROM…HAVING clause (single SELECT only). */
    void ApplyQueryOrdering(std::vector<std::pair<std::string, bool>> OrderByColumns, int64_t Limit, int64_t Offset) {
        OrderByColumns_ = std::move(OrderByColumns);
        Limit_ = Limit;
        Offset_ = Offset;
    }

    /** Join/filter/project pipeline through window step; JOIN scratch tables use JoinDestPrefix + index. */
    void EmitRelationPipeline(const std::string &MaterializedTable, const std::string &JoinDestPrefix,
                              BytecodeScratch &Instructions) const;
    /** ORDER BY / LIMIT / OFFSET / stack finalize using current OrderBySpecs / Limit / Offset / projection width. */
    void EmitOrderingFinalize(BytecodeScratch &Instructions) const;

private:
    std::vector<std::string> Columns_;
    std::string Table_;
    std::unique_ptr<ExpressionAST> WhereClause_;
    std::unique_ptr<ExpressionAST> HavingClause_;
    std::vector<std::pair<std::string, bool>> OrderByColumns_;
    int64_t Limit_;
    int64_t Offset_;
    bool Distinct_;
    std::vector<std::string> GroupByColumns_;
    GroupAggMode AggMode_;
    /** When \c AggMode_ is \c CountDistinct , the column whose distinct values are counted per group key. */
    std::optional<std::string> CountDistinctColumn_;
    std::optional<std::pair<std::string, bool>> RowNumberOverOrderBy_;
    /** Empty = whole table is one partition (legacy `OVER (ORDER BY …)` behavior). */
    std::vector<std::string> RowNumberPartitionByColumns_;
    std::string RowNumberAlias_;
    /** With \c RowNumberOverOrderBy_ set: \c 0 = \c ROW_NUMBER() , \c 1 = \c RANK() , \c 2 = \c DENSE_RANK() . */
    int WindowOrdinalKind_ = 0;
    std::vector<JoinClause> Joins_;
    std::vector<GroupCombAgg> CombinedAggs_;
    /** Parallel to Columns_: nullptr means a plain projected column name; otherwise CASE (etc.) lowered after pipeline. */
    std::vector<std::unique_ptr<ExpressionAST>> ProjectionExprs_;
	/** Non-empty when \c AggMode_ is \c CountStar or \c CountDistinct : \c AS alias or default \c cnt . */
	std::string CountAggregateOutputColumn_;
};

enum class CompoundSetOpKind : int8_t { UnionDistinct = 0, UnionAll = 1, Intersect = 2, Except = 3 };

/** UNION / INTERSECT / EXCEPT: each arm is a full SELECT through HAVING (no ORDER/LIMIT on arms). */
struct CompoundSelectAST : public StatementAST {
    std::vector<std::unique_ptr<SelectAST>> Arms;
    std::vector<CompoundSetOpKind> Ops;
    std::vector<std::pair<std::string, bool>> OrderByColumns;
    int64_t Limit = -1;
    int64_t Offset = 0;

    void EmitBytecode(BytecodeScratch &Instructions) const override;
};

struct CreateViewAST : public StatementAST {
	std::string ViewName;
	std::string BodySql_;
	std::unique_ptr<StatementAST> Definition_;

	CreateViewAST(std::string ViewName_, std::string BodySql_,
	              std::unique_ptr<StatementAST> Definition_)
	    : ViewName(std::move(ViewName_)),
	      BodySql_(std::move(BodySql_)),
	      Definition_(std::move(Definition_)) {}

	void EmitBytecode(BytecodeScratch &Instructions) const override;
};

struct DropViewAST : public StatementAST {
	std::string ViewName;
	bool IfExists = false;

	explicit DropViewAST(std::string ViewName_, bool IfExists = false)
	    : ViewName(std::move(ViewName_)), IfExists(IfExists) {}

	void EmitBytecode(BytecodeScratch &Instructions) const override;
};

struct CteClause {
    std::string Alias;
    std::string PhysicalTable;
    std::unique_ptr<SelectAST> Definition;
};

struct WithSelectAST : public StatementAST {
    std::vector<CteClause> Clauses;
    std::unique_ptr<StatementAST> Main;
    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

struct InsertAST : public ExpressionAST {
    std::unique_ptr<TableAST> Table;
    std::vector<std::string> Columns;
    std::vector<std::vector<std::string>> Values;
public:
    InsertAST(std::unique_ptr<TableAST> Table, std::vector<std::string> Columns, std::vector<std::vector<std::string>> Values)
        : Table(std::move(Table)), Columns(std::move(Columns)), Values(std::move(Values)) {}
    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

/** INSERT INTO t [ (cols) ] BULK count [ START n ] [ STEP n ] — codegen expands deterministic rows (testing / fixtures). */
struct BulkInsertAST : public ExpressionAST {
	std::string TableName;
	std::vector<std::string> Columns;
	int64_t Count = 0;
	int64_t StartId = 1;
	int64_t Step = 1;

	BulkInsertAST(std::string TableName, std::vector<std::string> Columns, int64_t Count, int64_t StartId, int64_t Step)
	    : TableName(std::move(TableName)), Columns(std::move(Columns)), Count(Count), StartId(StartId), Step(Step) {}
	void EmitBytecode(BytecodeScratch& Instructions) const override;
};

struct UpdateAST : public ExpressionAST {
    std::string TableName;
    std::vector<std::pair<std::string, std::string>> Assignments;
    std::unique_ptr<ExpressionAST> Condition;

    UpdateAST(std::string TableName, 
              std::vector<std::pair<std::string, std::string>> Assignments,
              std::unique_ptr<ExpressionAST> Condition)
        : TableName(std::move(TableName)),  Assignments(std::move(Assignments)), 
          Condition(std::move(Condition)) {}
    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

struct DeleteAST : public ExpressionAST {
    std::string TableName;
    std::unique_ptr<ExpressionAST> Condition;

    DeleteAST(std::string TableName, std::unique_ptr<ExpressionAST> Condition)
        : TableName(std::move(TableName)), Condition(std::move(Condition)) {}
    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

struct BinaryOpAST : public ExpressionAST {
    std::unique_ptr<ExpressionAST> LHS, RHS;
    std::string Op;

    BinaryOpAST(std::unique_ptr<ExpressionAST> LHS, std::string Op, std::unique_ptr<ExpressionAST> RHS)
        : LHS(std::move(LHS)), RHS(std::move(RHS)), Op(std::move(Op)) {}
    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

/** Builtin aggregate call allowed only in \c HAVING ; lowered to grouped output column names at codegen. */
struct FuncCallExprAST : public ExpressionAST {
	enum class Kind { CountStar, CountDistinct, Sum, Min, Max, Avg };
	Kind BuiltinKind = Kind::CountStar;
	/** \c COUNT(DISTINCT col) or \c SUM/MIN/MAX/AVG column; empty for \c COUNT(*) . */
	std::string ArgColumn;

	FuncCallExprAST(Kind K, std::string ArgCol) : BuiltinKind(K), ArgColumn(std::move(ArgCol)) {}
	void EmitBytecode(BytecodeScratch& Instructions) const override;
};

struct GrantAST : public ExpressionAST {
    std::string Grantee;
    Permissions Perms;
    std::string TableName;
	std::vector<std::string> Columns;
	bool GranteeIsRole = false;
    GrantAST(std::string GranteeIn, Permissions PermsIn, std::string Table = "",
             std::vector<std::string> ColumnsIn = {}, bool GranteeIsRoleIn = false)
        : Grantee(std::move(GranteeIn)), Perms(PermsIn), TableName(std::move(Table)),
          Columns(std::move(ColumnsIn)), GranteeIsRole(GranteeIsRoleIn) {}
    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

struct RevokeAST : public ExpressionAST {
    std::string Grantee;
    Permissions Perms;
    std::string TableName;
	std::vector<std::string> Columns;
	bool GranteeIsRole = false;
    RevokeAST(std::string GranteeIn, Permissions Permissions, std::string Table = "",
              std::vector<std::string> ColumnsIn = {}, bool GranteeIsRoleIn = false)
        : Grantee(std::move(GranteeIn)), Perms(Permissions), TableName(std::move(Table)),
          Columns(std::move(ColumnsIn)), GranteeIsRole(GranteeIsRoleIn) {}
    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

struct GrantRoleMembershipAST : public StatementAST {
	std::string RoleName;
	std::string UserName;
	GrantRoleMembershipAST(std::string Role, std::string User)
	    : RoleName(std::move(Role)), UserName(std::move(User)) {}
	void EmitBytecode(BytecodeScratch &Instructions) const override;
};

struct RevokeRoleMembershipAST : public StatementAST {
	std::string RoleName;
	std::string UserName;
	RevokeRoleMembershipAST(std::string Role, std::string User)
	    : RoleName(std::move(Role)), UserName(std::move(User)) {}
	void EmitBytecode(BytecodeScratch &Instructions) const override;
};

struct CreateRoleAST : public StatementAST {
	std::string RoleName;
	explicit CreateRoleAST(std::string Role) : RoleName(std::move(Role)) {}
	void EmitBytecode(BytecodeScratch &Instructions) const override;
};

struct DropRoleAST : public StatementAST {
	std::string RoleName;
	explicit DropRoleAST(std::string Role) : RoleName(std::move(Role)) {}
	void EmitBytecode(BytecodeScratch &Instructions) const override;
};

using ASTNode = std::unique_ptr<StatementAST>;
using ASTType = std::vector<ASTNode>;

class HybridAST {
	using KeyType = std::string;
	using ValueType = ASTNode;
	using BPTree = BPlusTree<KeyType, ValueType>;
	using RTree = DS::RadixTree<KeyType, ValueType>;

	static constexpr size_t SwitchDepth = 3;
	std::unique_ptr<BPTree> BPTreeRoot_;
	std::unique_ptr<RTree> RadixRoot_;
	bool UseRadix_ = false;

public:
	HybridAST();
	void Add(const KeyType &Key, ValueType Node, size_t Depth = 0);
	template<typename Func>
	void Traverse(Func &&F, size_t Depth = 0) const;
	bool Empty() const;
	ExpressionAST *Find(const KeyType &Key, size_t Depth = 0);
};

extern Tree<ASTNode> AST;

struct ParserTokenizeOnlyTag {};
inline constexpr ParserTokenizeOnlyTag ParserTokenizeOnly{};

bool ParserDiagnosticsEnabled();
void SetParserDiagnostics(bool Enabled);

class Parser {
    std::string_view Query_;
    TokenStream Tokens_;
    size_t CurrentIndex_ = 0;
    /** Monotonic suffix for unnamed `CASE` columns (`_case0`, …). */
    unsigned NextAnonCaseAlias_ = 0;
    unsigned NextAnonCastAlias_ = 0;
    unsigned NextAnonCoalesceAlias_ = 0;
    /** During WITH ... SELECT, map logical CTE alias -> materialized table name (__astral_cte_*). */
    std::unordered_map<std::string, std::string> CteSubstitutions_;
    /** When true, \c ParsePrimary accepts \c COUNT/SUM/MIN/MAX/AVG(…) for \c HAVING only. */
    bool AllowAggCallsInPredicate_ = false;

    int GetTokenPrecedence(const Token &Token);

    bool IsEOF() { return CurrentIndex_ >= Tokens_.size(); }

    TokenStream Tokenize();

    ASTNode ParsePrimary();
    ASTNode TryParseAggregateFuncPrimary();

    bool IsConstraint(const std::string &TokenValue);
    
    bool IsKeyword(const std::string &TokenValue);
    
    std::optional<Token> CurrentToken() const {
        if(CurrentIndex_ < Tokens_.size()) return Tokens_[CurrentIndex_];
        return std::nullopt;
    }

    void AdvanceToken() {
        if (!IsEOF()) ++CurrentIndex_;
    }

    bool MatchToken(TokenType ExpectedType) {
        if(auto Token = CurrentToken(); Token && Token->Type == ExpectedType) {
            AdvanceToken();
            return true;
        }
        return false;
    }

    bool MatchToken(const Token &Other) {
        if(auto Token = CurrentToken(); MatchToken(Other.Type) && MatchKeyword(Other.Value))
            return true;
        return false;
    }

    bool MatchKeyword(const std::string &ExpectedKeyword) {
        if (auto Token = CurrentToken()) {
            /** Keywords are lexer-normalized to ASCII upper case (see Parser::Tokenize). */
            if(Token->Type == TokenType::KEYWORD && Token->Value == ExpectedKeyword) {
                AdvanceToken();
                return true;
            }
        }
        return false;
    }

    bool MatchToken(TokenType ExpectedType, const std::string& ExpectedValue) {
        if (auto Token = CurrentToken()) {
            if (Token->Type == ExpectedType && Token->Value == ExpectedValue) {
                AdvanceToken();
                return true;
            }
        }
        return false;
    }

    Tree<ASTNode> BuildAST() const;

    ASTNode ParseExpression();
    ASTNode ParseCreateStatement();
	ASTNode ParseCreateViewStatement();
    ASTNode ParseSelectStatement();
    /** After consuming the SELECT keyword: one arm through HAVING (no ORDER BY / LIMIT). */
    std::unique_ptr<SelectAST> ParseSelectArmThroughHaving();
    std::unique_ptr<StatementAST> ParseWithStatement();
    void ApplyCteSubstitution(std::string &TableName) const;
    std::unique_ptr<CaseExprAST> ParseSearchedCaseExpression();
    std::unique_ptr<CaseExprAST> ParseCoalesceExpression();
    std::unique_ptr<ExpressionAST> ParseCaseScalarResult();
    /** `ParseDataType()` result → cast family; \c ParseFail on unsupported `CAST` targets. */
    SqlCastTarget ParseCastTargetFromDataType(std::string ParsedType);
    ASTNode ParseExistsPredicate(bool Negated);
    ASTNode ParseInsertStatement();
    ASTNode ParseUpdateStatement();
    ASTNode ParseDeleteStatement();
    ASTNode ParseWhereClause();
    ASTNode ParseBinaryOperation();
    ASTNode ParseBinaryOperation(int MinPrec, ASTNode LHS);
    ASTNode ParseGrantStatement();
    ASTNode ParseRevokeStatement();
    ASTNode ParseTransactionStatement();
    ASTNode ParseDropStatement();
    ASTNode ParseAlterStatement();
    ASTNode ParseSavepointSetStatement();
    ASTNode ParseReleaseSavepointStatement();
	std::unique_ptr<StatementAST> ParseDataExchangeStatement();
    ASTNode ParseRollbackStatement();
    std::string ParseDataType();
    std::vector<std::string> ParseColumnConstraintList();
    TableConstraintDef ParseTableConstraint();
    ASTNode ParseUnaryOrPostfixPredicate();

	[[noreturn]] void ParseFail(std::string Message) const;
	[[noreturn]] void LexFail(std::size_t ByteOffset, std::string Message) const;

public:
	explicit Parser(std::string_view Query);
	explicit Parser(std::string_view Query, ParserTokenizeOnlyTag Tag);
	ASTNode ParseStandaloneSelectForViewExpansion();
	bool StandaloneSelectConsumedAllTokens() const { return CurrentIndex_ >= Tokens_.size(); }

	/** Tokenize-only parser: consumes a single predicate (same grammar as WHERE) until EOF; used by CHECK codegen. */
	std::unique_ptr<ExpressionAST> ParseStandalonePredicateExpression();

    std::unique_ptr<StatementAST> ParseStatement();

    void DumpTokens() const;

    void DumpAST() {
        std::function<void(const Tree<ASTNode>::Node&, int)> DumpNode = [&](const Tree<ASTNode>::Node& Node, int Depth) {
            BytecodeScratch Scratch;
            Node.Value->EmitBytecode(Scratch);
            Bytecode PrintBuf(Scratch.begin(), Scratch.end());
            std::cout << std::string(Depth * 2, ' ') << PrintBuf << '\n';
            for (const auto& Child : Node.Children)
                DumpNode(*Child, Depth + 1);
        };
    }
};

/** Compile a CHECK predicate (same surface as WHERE) into packed DNF for storage / VM; throws on unsupported shape. */
void CompileCheckSqlToDnfPackedOrThrow(std::string_view CheckBodySql, std::string &OutPackedBlob);

}
}