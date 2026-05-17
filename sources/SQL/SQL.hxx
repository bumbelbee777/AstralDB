#pragma once

#include <IO/Logger.hxx>
#include <Database/User.hxx>
#include <Database/HybridStorageScheduler.hxx>
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
#include <type_traits>

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
    std::vector<std::string> RefColumns;
    /** 0=RESTRICT, 1=CASCADE, 2=SET NULL (ON DELETE). */
    int OnDeleteAction = 0;
    std::string CheckSql;
};

enum class AlterTableKind { AddColumn, DropColumn, RenameColumn, SetStorage };

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

/** SQL boolean literal (\c TRUE / \c FALSE keywords). */
struct BooleanLiteralAST : public ExpressionAST {
    bool Value = false;

    explicit BooleanLiteralAST(bool ValueIn) : Value(ValueIn) {}
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
enum class SqlCastTarget : int8_t { Text = 0, Integer = 1, Real = 2, Boolean = 3, Advanced = 4 };

/** Built-in scalar functions allowed in SELECT projections (evaluated via \c Opcode::SCALAR_FUNC_EVAL ). */
enum class ScalarSqlFn : int16_t {
	Upper = 0,
	Lower = 1,
	CharLength = 2,
	/** SQL-99 \c SUBSTRING(expr FROM start [FOR length]) or three-argument comma form; two args = suffix from start. */
	SubstringFromFor = 3,
	PositionIn = 4,
	TrimBoth = 5,
	TrimLeading = 6,
	TrimTrailing = 7,
	ConcatVariadic = 8,
	ExtractYear = 9,
	ExtractMonth = 10,
	ExtractDay = 11,
	DateAddDays = 12,
	DateSubDays = 13,
	DateDiffDays = 14,
	Grouping = 15,
	ExtractHour = 16,
	ExtractMinute = 17,
	ExtractSecond = 18,
	ExtractEpoch = 19,
	TimeBucketSeconds = 20,
	DateTrunc = 21,
	TimestampDiffSeconds = 22,
	DateAddSeconds = 23,
	StructField = 24,
	MapGet = 25,
	ComplexReal = 26,
	ComplexImag = 27,
	ComplexMul = 28,
	VectorDot = 29,
	VectorAdd = 30,
	VectorNorm = 31,
	MatrixVec = 32,
	Abs = 33,
	Sqrt = 34,
	Cbrt = 35,
	Exp = 36,
	Ln = 37,
	Log10 = 38,
	Log2 = 39,
	Sin = 40,
	Cos = 41,
	Tan = 42,
	Asin = 43,
	Acos = 44,
	Atan = 45,
	Sinh = 46,
	Cosh = 47,
	Tanh = 48,
	Floor = 49,
	Ceil = 50,
	Round = 51,
	Trunc = 52,
	Sign = 53,
	Degrees = 54,
	Radians = 55,
	Pow = 56,
	Atan2 = 57,
	Mod = 58,
	Hypot = 59,
	Lerp = 60,
	Clamp = 61,
	Mean = 62,
	VarPop = 63,
	VarSamp = 64,
	StdPop = 65,
	StdSamp = 66,
	Median = 67,
	Entropy = 68,
	NormL1 = 69,
	NormL2 = 70,
	ListSum = 71,
	Corr = 72,
	CovPop = 73,
	CovSamp = 74,
	Sigmoid = 75,
	Relu = 76,
	Softmax = 77,
	MinMaxScale = 78,
	ZScore = 79,
	ListLen = 80,
	ListGet = 81,
	ListAppend = 82,
	ListConcat = 83,
	ListContains = 84,
	ListSlice = 85,
	Logistic = 86,
	Logit = 87,
	Softplus = 88,
	LeakyRelu = 89,
	MseLoss = 90,
	MaeLoss = 91,
	RmseLoss = 92,
	BceLoss = 93,
	HingeLoss = 94,
	HuberLoss = 95,
	CeLoss = 96,
	Random = 97,
	RandomNormal = 98,
	RandomInt = 99,
	SetSeed = 100,
	CosineSim = 101,
	EuclideanDist = 102,
	ManhattanDist = 103,
	MatVec = 104,
	ListSort = 105,
	ListSortDesc = 106,
	ListReverse = 107,
	JsonExtract = 108,
	JsonContains = 109,
	JsonMerge = 110,
	JsonArrayLength = 111,
	JsonKeys = 112,
	NullIf = 113,
	Greatest = 114,
	Least = 115,
	TextContains = 116,
	TextMatch = 117,
	GroupingId = 118,
	XmlExtract = 119,
	XmlSerialize = 120,
	XmlValid = 121,
	TextRank = 122,
	VectorTopK = 123,
	Fft = 124,
	Ifft = 125,
	Dct = 126,
	Idct = 127,
	Conv1d = 128,
	Conv1dSame = 129,
	Laplacian1d = 130,
	AdGradAdd = 131,
	AdGradMulLhs = 132,
	AdGradMulRhs = 133,
	AdGradRelu = 134,
	AdGradSigmoid = 135,
	AdGradConv1dIn = 136,
	AdGradConv1dK = 137,
	AdChain = 138,
	AdHessian = 139,
	AdHessianRelu = 140,
	AdHessianSigmoid = 141,
	AdHessianSquare = 142,
	AdWirtingerMulLhs = 143,
	AdWirtingerMulRhs = 144,
	AdWirtingerAbs2 = 145,
	AdWirtingerChain = 146,
	AdWirtingerDz = 147,
	AdWirtingerDzBar = 148,
	OdeEuler = 149,
	OdeRk4 = 150,
	SdeEuler = 151,
	SdeGbm = 152,
	SdeOu = 153,
	PdeHeatStep = 154,
	PdePoissonStep = 155,
};

static_assert(sizeof(std::underlying_type_t<ScalarSqlFn>) >= 2,
              "ScalarSqlFn must use at least int16 (builtins exceed 127)");

/** VM / bytecode tag for \c ScalarSqlFn (always non-negative \c int64 ). */
inline constexpr int64_t ScalarSqlFnTag(ScalarSqlFn Fn) noexcept {
	return static_cast<int64_t>(static_cast<std::underlying_type_t<ScalarSqlFn>>(Fn));
}

enum class SecondaryIndexKind : int8_t { Fts = 0, Vector = 1 };

struct MatchRecognizeDefine {
	std::string Symbol;
	std::unique_ptr<ExpressionAST> Predicate;
	std::string PredicatePackedDnf;
};

struct MatchRecognizeSpec {
	std::string OrderColumn;
	std::string Pattern;
	std::vector<MatchRecognizeDefine> Defines;
};

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
	/** Populated when \c Target is \c Advanced (STRUCT/MAP/VECTOR/MATRIX/COMPLEX spellings). */
	std::string TargetTypeSql;

    CastExprAST(std::unique_ptr<ExpressionAST> OperandIn, SqlCastTarget T, std::string TargetTypeSqlIn = {})
        : Operand(std::move(OperandIn)), Target(T), TargetTypeSql(std::move(TargetTypeSqlIn)) {}

    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

/** SQL-99-style scalar builtin in a SELECT list (\c UPPER , \c SUBSTRING … FOR … , etc.). */
struct ScalarFuncExprAST : public ExpressionAST {
	ScalarSqlFn Fn = ScalarSqlFn::Upper;
	std::vector<std::unique_ptr<ExpressionAST>> Args;

	ScalarFuncExprAST(ScalarSqlFn Kind, std::vector<std::unique_ptr<ExpressionAST>> Arguments)
	    : Fn(Kind), Args(std::move(Arguments)) {}

	void EmitBytecode(BytecodeScratch &Instructions) const override;
};

struct ColumnRefAST : public ExpressionAST {
    std::string Name;

    explicit ColumnRefAST(std::string Name) : Name(std::move(Name)) {}
    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

/** Qualified column for SET / MERGE (\c EXCLUDED.col , \c source_alias.col ). */
struct QualifiedRefAST : public ExpressionAST {
	enum class Role { Target, Excluded, Source };
	Role RefRole = Role::Target;
	std::string Column;

	QualifiedRefAST(Role RefRole, std::string Column) : RefRole(RefRole), Column(std::move(Column)) {}
	void EmitBytecode(BytecodeScratch &Instructions) const override;
};

/** \c GROUPING(col) in SELECT (requires OLAP \c GROUP BY). */
struct GroupingExprAST : public ExpressionAST {
	std::string Column;

	explicit GroupingExprAST(std::string Column) : Column(std::move(Column)) {}
	void EmitBytecode(BytecodeScratch &Instructions) const override;
};

/** SQL:2003 \c GROUPING_ID(col, …) bitmask over \c _grouping_* metadata. */
struct GroupingIdExprAST : public ExpressionAST {
	std::vector<std::string> Columns;

	explicit GroupingIdExprAST(std::vector<std::string> Columns) : Columns(std::move(Columns)) {}
	void EmitBytecode(BytecodeScratch &Instructions) const override;
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
	StorageLayout StoragePolicy = StorageLayout::Row;

    explicit CreateAST(std::string TableName, std::vector<ColumnDefinition> Columns, bool IfNotExists = false)
        : TableName(std::move(TableName)), Columns(std::move(Columns)), IfNotExists(IfNotExists) {}
    CreateAST(std::string TableName, std::vector<ColumnDefinition> Columns,
              std::vector<TableConstraintDef> TableConstraints, bool IfNotExists = false,
              StorageLayout StoragePolicyIn = StorageLayout::Row)
        : TableName(std::move(TableName))
        , Columns(std::move(Columns))
        , TableConstraints(std::move(TableConstraints))
        , IfNotExists(IfNotExists)
        , StoragePolicy(StoragePolicyIn) {}
    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

struct AlterTableAST : public StatementAST {
    AlterTableKind Kind = AlterTableKind::AddColumn;
    std::string TableName;
    ColumnDefinition AddedColumn = ColumnDefinition("", "TEXT");
    std::string DropColumnName;
    std::string RenameFrom;
    std::string RenameTo;
	StorageLayout StoragePolicy = StorageLayout::Row;

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
	bool Cascade = false;

    explicit DropAST(std::string TableName, bool IfExists = false, bool CascadeIn = false)
        : TableName(std::move(TableName)), IfExists(IfExists), Cascade(CascadeIn) {}
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

/** OLAP modifier after \c GROUP BY column list (\c WITH ROLLUP / \c WITH CUBE). */
enum class GroupOlapModifier { None, Rollup, Cube, GroupingSets };

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

enum class WindowFnKind : int8_t {
    RowNumber = 0,
    Rank = 1,
    DenseRank = 2,
    Sum = 3,
    Min = 4,
    Max = 5,
    Avg = 6,
    Lag = 7,
    Lead = 8
};

enum class WindowFrameBoundKind : int8_t {
    UnboundedPreceding = 0,
    Preceding = 1,
    CurrentRow = 2,
    Following = 3,
    UnboundedFollowing = 4
};

struct WindowFrameBound {
    WindowFrameBoundKind Kind = WindowFrameBoundKind::CurrentRow;
    int64_t Offset = 0;
};

struct WindowSpec {
    WindowFnKind Kind = WindowFnKind::RowNumber;
    std::string SourceColumn;
    std::vector<std::string> PartitionBy;
    std::string OrderColumn;
    bool OrderAscending = true;
    std::string OutputColumn;
    int64_t FrameOffset = 1;
    bool HasExplicitRowsFrame = false;
    WindowFrameBound FrameStart{WindowFrameBoundKind::UnboundedPreceding};
    WindowFrameBound FrameEnd{WindowFrameBoundKind::CurrentRow};
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
              std::vector<WindowSpec> WindowSpecs = {},
              std::vector<JoinClause> Joins = {},
              std::vector<GroupCombAgg> CombinedAggs = {},
              std::vector<std::unique_ptr<ExpressionAST>> ProjectionExprsIn = {},
              std::string CountAggregateOutputColumn = {},
              GroupOlapModifier OlapModifier = GroupOlapModifier::None,
              std::vector<std::vector<std::string>> GroupingSetsList = {})
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
        , WindowSpecs_(std::move(WindowSpecs))
        , Joins_(std::move(Joins))
        , CombinedAggs_(std::move(CombinedAggs))
        , CountAggregateOutputColumn_(std::move(CountAggregateOutputColumn))
        , OlapModifier_(OlapModifier)
        , GroupingSetsList_(std::move(GroupingSetsList)) {
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
    const std::vector<WindowSpec> &WindowSpecs() const { return WindowSpecs_; }
    bool DistinctSelected() const { return Distinct_; }
    const std::vector<std::pair<std::string, bool>> &OrderBySpecs() const { return OrderByColumns_; }
    int64_t SelectLimitValue() const { return Limit_; }
    int64_t SelectOffsetValue() const { return Offset_; }
    const ExpressionAST *WhereRoot() const { return WhereClause_.get(); }
    const ExpressionAST *HavingRoot() const { return HavingClause_.get(); }
    const std::vector<JoinClause> &JoinSpecs() const { return Joins_; }
    const std::vector<GroupCombAgg> &ComboAggs() const { return CombinedAggs_; }
    GroupOlapModifier OlapKind() const { return OlapModifier_; }
    const std::vector<std::vector<std::string>> &GroupingSets() const { return GroupingSetsList_; }
    const std::vector<std::string> &ProjectionColumns() const { return Columns_; }
    const std::vector<std::unique_ptr<ExpressionAST>> &ProjectionExprs() const { return ProjectionExprs_; }
	std::optional<StorageLayout> StorageHint() const { return StorageHint_; }
	void SetStorageHint(std::optional<StorageLayout> Hint) { StorageHint_ = Hint; }
	const std::optional<std::string> &AsOfTimestamp() const { return AsOfTimestamp_; }
	void SetAsOfTimestamp(std::optional<std::string> Ts) { AsOfTimestamp_ = std::move(Ts); }
	const std::optional<MatchRecognizeSpec> &MatchRecognize() const { return MatchRecognize_; }
	void SetMatchRecognize(std::optional<MatchRecognizeSpec> Spec) { MatchRecognize_ = std::move(Spec); }

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
    std::vector<WindowSpec> WindowSpecs_;
    std::vector<JoinClause> Joins_;
    std::vector<GroupCombAgg> CombinedAggs_;
    /** Parallel to Columns_: nullptr means a plain projected column name; otherwise CASE (etc.) lowered after pipeline. */
    std::vector<std::unique_ptr<ExpressionAST>> ProjectionExprs_;
	/** Non-empty when \c AggMode_ is \c CountStar or \c CountDistinct : \c AS alias or default \c cnt . */
	std::string CountAggregateOutputColumn_;
	GroupOlapModifier OlapModifier_ = GroupOlapModifier::None;
	std::vector<std::vector<std::string>> GroupingSetsList_;
	std::optional<StorageLayout> StorageHint_;
	std::optional<std::string> AsOfTimestamp_;
	std::optional<MatchRecognizeSpec> MatchRecognize_;
};

enum class CompoundSetOpKind : int8_t {
	UnionDistinct = 0,
	UnionAll = 1,
	Intersect = 2,
	IntersectAll = 3,
	Except = 4,
	ExceptAll = 5
};

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

/** \c CREATE PROCEDURE name AS ( stmt; … ) — body is compiled and cached as \c .abc on disk. */
struct CreateProcedureAST : public StatementAST {
	std::string ProcedureName;
	std::string BodySql_;
	bool IfNotExists = false;

	CreateProcedureAST(std::string Name_, std::string BodySql_, bool IfNotExists_ = false)
	    : ProcedureName(std::move(Name_)), BodySql_(std::move(BodySql_)), IfNotExists(IfNotExists_) {}

	void EmitBytecode(BytecodeScratch &Instructions) const override;
};

struct DropProcedureAST : public StatementAST {
	std::string ProcedureName;
	bool IfExists = false;

	explicit DropProcedureAST(std::string Name_, bool IfExists_ = false)
	    : ProcedureName(std::move(Name_)), IfExists(IfExists_) {}

	void EmitBytecode(BytecodeScratch &Instructions) const override;
};

/** \c CALL name or \c EXECUTE PROCEDURE name . */
struct CallProcedureAST : public StatementAST {
	std::string ProcedureName;

	explicit CallProcedureAST(std::string Name_) : ProcedureName(std::move(Name_)) {}

	void EmitBytecode(BytecodeScratch &Instructions) const override;
};

struct CteClause {
	std::string Alias;
	std::string PhysicalTable;
	std::unique_ptr<SelectAST> Anchor;
	/** Non-null for \c WITH RECURSIVE … anchor \c UNION ALL recursive_step . */
	std::unique_ptr<SelectAST> RecursiveStep;

	bool IsRecursive() const { return RecursiveStep != nullptr; }
};

struct WithSelectAST : public StatementAST {
    std::vector<CteClause> Clauses;
    std::unique_ptr<StatementAST> Main;
    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

/** One \c DO UPDATE SET assignment in \c INSERT … ON CONFLICT (literal or \c EXCLUDED.col). */
struct UpsertAssign {
	std::string Column;
	std::unique_ptr<ExpressionAST> Value;
};

/** INSERT … ON CONFLICT … DO UPDATE / DO NOTHING (upsert). */
struct UpsertSpec {
	enum class OnConflict { Update, Nothing };
	OnConflict Mode = OnConflict::Update;
	std::vector<std::string> ConflictColumns;
	std::vector<UpsertAssign> UpdateAssignments;
};

struct InsertAST : public ExpressionAST {
    std::unique_ptr<TableAST> Table;
    std::vector<std::string> Columns;
    std::vector<std::vector<std::string>> Values;
	std::optional<UpsertSpec> Upsert_;
public:
    InsertAST(std::unique_ptr<TableAST> Table, std::vector<std::string> Columns,
              std::vector<std::vector<std::string>> Values, std::optional<UpsertSpec> Upsert = std::nullopt)
        : Table(std::move(Table)), Columns(std::move(Columns)), Values(std::move(Values)), Upsert_(std::move(Upsert)) {}
    void EmitBytecode(BytecodeScratch& Instructions) const override;
};

struct MergeMatchedSet {
	std::string TargetColumn;
	std::unique_ptr<ExpressionAST> Value;
};

struct MergeInsertField {
	std::string Column;
	std::unique_ptr<ExpressionAST> Value;
};

struct MergeAST : public StatementAST {
	std::string TargetTable;
	std::string TargetAlias;
	std::string SourceTable;
	std::string SourceAlias;
	std::vector<std::pair<std::string, std::string>> OnKeyPairs;
	std::vector<MergeMatchedSet> Matched;
	std::vector<MergeInsertField> NotMatched;
	bool HasMatchedBranch = false;
	bool HasNotMatchedBranch = false;
	void EmitBytecode(BytecodeScratch &Instructions) const override;
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
    std::vector<std::pair<std::string, std::unique_ptr<ExpressionAST>>> Assignments;
    std::unique_ptr<ExpressionAST> Condition;

    UpdateAST(std::string TableName,
              std::vector<std::pair<std::string, std::unique_ptr<ExpressionAST>>> Assignments,
              std::unique_ptr<ExpressionAST> Condition)
        : TableName(std::move(TableName)), Assignments(std::move(Assignments)),
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
	bool WithGrantOption = false;
    GrantAST(std::string GranteeIn, Permissions PermsIn, std::string Table = {},
             std::vector<std::string> ColumnsIn = {}, bool GranteeIsRoleIn = false,
             bool WithGrantOptionIn = false)
        : Grantee(std::move(GranteeIn)), Perms(PermsIn), TableName(std::move(Table)),
          Columns(std::move(ColumnsIn)), GranteeIsRole(GranteeIsRoleIn),
          WithGrantOption(WithGrantOptionIn) {}
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

struct CreateSequenceAST : public StatementAST {
	std::string SequenceName;
	int64_t Start = 1;
	int64_t Increment = 1;
	bool IfNotExists = false;

	CreateSequenceAST(std::string Name, int64_t Start, int64_t Increment, bool IfNotExists)
	    : SequenceName(std::move(Name)), Start(Start), Increment(Increment), IfNotExists(IfNotExists) {}
	void EmitBytecode(BytecodeScratch &Instructions) const override;
};

struct DropSequenceAST : public StatementAST {
	std::string SequenceName;
	bool IfExists = false;

	DropSequenceAST(std::string Name, bool IfExists) : SequenceName(std::move(Name)), IfExists(IfExists) {}
	void EmitBytecode(BytecodeScratch &Instructions) const override;
};

struct CreateIndexAST : public StatementAST {
	std::string IndexName;
	std::string TableName;
	std::string ColumnName;
	SecondaryIndexKind Kind = SecondaryIndexKind::Fts;
	int64_t VectorMetricTag = 1;

	CreateIndexAST(std::string Name, std::string Table, std::string Column, SecondaryIndexKind Kind,
	               int64_t MetricTag = 1)
	    : IndexName(std::move(Name)), TableName(std::move(Table)), ColumnName(std::move(Column)), Kind(Kind),
	      VectorMetricTag(MetricTag) {}
	void EmitBytecode(BytecodeScratch &Instructions) const override;
};

struct DropIndexAST : public StatementAST {
	std::string IndexName;
	bool IfExists = false;

	DropIndexAST(std::string Name, bool IfExists) : IndexName(std::move(Name)), IfExists(IfExists) {}
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
    unsigned NextAnonScalarSqlFnAlias_ = 0;
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
	ASTNode ParseCreateIndexStatement();
	ASTNode ParseCreateViewStatement();
	ASTNode ParseCreateProcedureStatement();
	ASTNode ParseDropProcedureStatement();
	ASTNode ParseCallProcedureStatement();
    ASTNode ParseSelectStatement();
    /** After consuming the SELECT keyword: one arm through HAVING (no ORDER BY / LIMIT). */
    std::unique_ptr<SelectAST> ParseSelectArmThroughHaving();
    std::unique_ptr<StatementAST> ParseWithStatement();
    void ApplyCteSubstitution(std::string &TableName) const;
    std::unique_ptr<CaseExprAST> ParseSearchedCaseExpression();
    std::unique_ptr<CaseExprAST> ParseCoalesceExpression();
    std::unique_ptr<ExpressionAST> ParseCaseScalarResult();
    /** RHS for UPDATE / UPSERT / MERGE SET (literals, qualified refs, + - * /). */
    std::unique_ptr<ExpressionAST> ParseSetValueExpression(const std::optional<std::string> &TargetAlias,
                                                         const std::optional<std::string> &SourceAlias,
                                                         bool AllowExcluded);
    /** If the lookahead starts a supported scalar builtin, consumes it and leaves the cursor past ')'; else nullptr. */
    std::unique_ptr<ScalarFuncExprAST> TryParseScalarSqlBuiltinSelectExpr();
    /** `ParseDataType()` result → cast family; \c ParseFail on unsupported `CAST` targets. */
    SqlCastTarget ParseCastTargetFromDataType(std::string ParsedType);
    ASTNode ParseExistsPredicate(bool Negated);
    ASTNode ParseInsertStatement();
	ASTNode ParseMergeStatement();
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
    void ParseWindowOverClause(WindowSpec &Ws);
	void ParseSequenceOptions(int64_t &Start, int64_t &Increment);

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