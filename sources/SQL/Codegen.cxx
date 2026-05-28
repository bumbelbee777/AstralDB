#include <SQL/Bytecode.hxx>
#include <SQL/GraphOptimizer.hxx>
#include <SQL/MathSciOptimizer.hxx>
#include <SQL/SetExprEval.hxx>
#include <Database/MathSci.hxx>
#include <SQL/SQL.hxx>
#include <SQL/BytecodeProcedures.hxx>
#include <SQL/BytecodeTriggers.hxx>
#include <SQL/DialectCompat.hxx>
#include <Database/Database.hxx>
#include <Database/MathSci.hxx>
#include <IO/Limits.hxx>
#include <IO/ArenaAllocator.hxx>
#include <IO/Error.hxx>
#include <DS/HashTable.hxx>
#include <DS/LZ4.hxx>
#include <IO/Logger.hxx>
#include <memory>
#include <memory_resource>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cctype>
#include <atomic>

namespace AstralDB::SQL {

namespace {

using RowTriple = std::tuple<std::string, std::string, std::string>;

[[noreturn]] inline void FailCodegen(std::string Message) {
	throw std::runtime_error(AstralDB::Err::Prefixed("SQL codegen", std::move(Message)));
}

bool TryParseDoubleCodegen(const std::string &S, double &Out) {
	try {
		size_t Pos = 0;
		Out = std::stod(S, &Pos);
		return Pos == S.size() || std::isspace(static_cast<unsigned char>(S[Pos]));
	} catch(...) {
		return false;
	}
}

/** Fold INSERT VALUES expressions that are literals or constant scalar calls (e.g. ST_MESH(...)). */
std::optional<std::string> TryConstEvalInsertExpr(const ExpressionAST *E) {
	if(!E)
		return std::nullopt;
	if(const auto *L = dynamic_cast<const LiteralAST *>(E))
		return L->Value;
	if(dynamic_cast<const NullLiteralAST *>(E))
		return std::string();
	if(const auto *Sf = dynamic_cast<const ScalarFuncExprAST *>(E)) {
		std::vector<std::string> Cells;
		Cells.reserve(Sf->Args.size());
		for(const auto &A : Sf->Args) {
			const auto V = TryConstEvalInsertExpr(A.get());
			if(!V)
				return std::nullopt;
			Cells.push_back(*V);
		}
		if(MathSci::IsMathSciScalarFn(Sf->Fn))
			return MathSci::EvalScalar(Sf->Fn, Cells, nullptr);
		return std::nullopt;
	}
	if(const auto *C = dynamic_cast<const CastExprAST *>(E))
		return TryConstEvalInsertExpr(C->Operand.get());
	if(const auto *B = dynamic_cast<const BinaryOpAST *>(E)) {
		const auto L = TryConstEvalInsertExpr(B->LHS.get());
		const auto R = TryConstEvalInsertExpr(B->RHS.get());
		if(!L || !R)
			return std::nullopt;
		double A = 0, Bv = 0;
		if(B->Op == "+" && TryParseDoubleCodegen(*L, A) && TryParseDoubleCodegen(*R, Bv))
			return std::to_string(static_cast<long long>(std::llround(A + Bv)));
		if(B->Op == "-" && TryParseDoubleCodegen(*L, A) && TryParseDoubleCodegen(*R, Bv))
			return std::to_string(static_cast<long long>(std::llround(A - Bv)));
		if(B->Op == "*" && TryParseDoubleCodegen(*L, A) && TryParseDoubleCodegen(*R, Bv))
			return std::to_string(static_cast<long long>(std::llround(A * Bv)));
		if(B->Op == "/" && TryParseDoubleCodegen(*L, A) && TryParseDoubleCodegen(*R, Bv) && Bv != 0)
			return std::to_string(static_cast<long long>(std::llround(A / Bv)));
		if(B->Op == "+")
			return *L + *R;
		return std::nullopt;
	}
	return std::nullopt;
}

struct SqlViewCodegenState {
	std::unordered_map<std::string, std::string> *LiveBodies = nullptr;
	std::unordered_map<std::string, const StatementAST *> *SessionParsedBodies = nullptr;
	std::unordered_map<std::string, std::unique_ptr<StatementAST>> WalReparsedBodies;
	unsigned ExpansionDepth = 0;
	uint64_t ScratchCounter = 0;
};

thread_local SqlViewCodegenState *GViewCodegen = nullptr;

class ScopedViewCodegen {
	SqlViewCodegenState *Prev_;

public:
	explicit ScopedViewCodegen(SqlViewCodegenState *Cx) : Prev_(GViewCodegen) { GViewCodegen = Cx; }
	~ScopedViewCodegen() { GViewCodegen = Prev_; }
};

std::string ResolveRelationLogicalName(const std::string &Logical, BytecodeScratch &Instructions);

const StatementAST *ResolveViewAstForExpansion(const std::string &ViewName, SqlViewCodegenState &St) {
	auto ItS = St.SessionParsedBodies->find(ViewName);
	if(ItS != St.SessionParsedBodies->end())
		return ItS->second;
	auto ItW = St.WalReparsedBodies.find(ViewName);
	if(ItW != St.WalReparsedBodies.end())
		return ItW->second.get();
	auto ItB = St.LiveBodies->find(ViewName);
	if(ItB == St.LiveBodies->end())
		return nullptr;
	Parser Sub(ItB->second, ParserTokenizeOnly);
	auto Node = Sub.ParseStandaloneSelectForViewExpansion();
	if(!Node || !dynamic_cast<const SelectAST *>(Node.get()))
		FailCodegen(
		    "View \"" + ViewName + "\" has an invalid or unsupported definition (expected a single SELECT).");
	if(!Sub.StandaloneSelectConsumedAllTokens())
		FailCodegen("View \"" + ViewName + "\" body must be a single SELECT with no trailing tokens.");
	const StatementAST *Raw = Node.get();
	St.WalReparsedBodies[ItB->first] = std::move(Node);
	return Raw;
}

void EmitExpandedViewToScratch(const std::string &Scratch, const std::string &ViewName, BytecodeScratch &Instr) {
	auto *G = GViewCodegen;
	if(!G)
		FailCodegen("Internal error: missing view codegen context.");
	const StatementAST *Raw = ResolveViewAstForExpansion(ViewName, *G);
	const auto *Sel = dynamic_cast<const SelectAST *>(Raw);
	if(!Sel)
		FailCodegen("Internal error: expanding non-SELECT view \"" + ViewName + "\".");
	const std::string Base = ResolveRelationLogicalName(Sel->SourceTableName(), Instr);
	AppendInstruction(Instr, MakeInstruction(Opcode::CLONE_TABLE, Scratch, Base));
	Sel->EmitBytecodeForScratchTable(Scratch, Instr);
}

std::string ResolveRelationLogicalName(const std::string &Logical, BytecodeScratch &Instructions) {
	auto *G = GViewCodegen;
	if(!G || !G->LiveBodies || G->LiveBodies->find(Logical) == G->LiveBodies->end())
		return Logical;
	if(++G->ExpansionDepth > Limits::MaxSqlViewExpansionDepth)
		FailCodegen("View expansion exceeded maximum depth (" +
		            std::to_string(static_cast<int>(Limits::MaxSqlViewExpansionDepth)) + ").");
	const std::string Scratch = std::string("__AstView_") + std::to_string(++G->ScratchCounter);
	EmitExpandedViewToScratch(Scratch, Logical, Instructions);
	--G->ExpansionDepth;
	return Scratch;
}

static constexpr const char *kExistCol = "__ASTRAL_EXISTS__";
static constexpr const char *kInSubCol = "__ASTRAL_IN_SUBQUERY__";
/** Stored as predicate RHS bytes; MatchOnePredicate resolves against the enclosing+inner merged row (correlation). */
static constexpr const char kAstRhsColMarker[] = "__AST_RHS_COL__:";

static constexpr const char *kOpIsNull = "__IS_NULL__";
static constexpr const char *kOpIsNotNull = "__IS_NOT_NULL__";
static constexpr const char *kOpIn = "__IN__";
static constexpr const char *kOpNotIn = "__NOT_IN__";
static constexpr const char *kBoolConstCol = "__ASTRAL_BOOL__";
static constexpr const char *kOpBoolConst = "__BOOL_CONST__";
static constexpr const char *kQuantifiedSubqueryCol = "__ASTRAL_QSUBQ__";
static constexpr const char *kRowCompareCol = "__ASTRAL_ROWCMP__";

static std::string MapHavingAggOutputColumn(const FuncCallExprAST &F, const SelectAST &Sel) {
	switch(F.BuiltinKind) {
	case FuncCallExprAST::Kind::CountStar:
		if(Sel.AggKind() != GroupAggMode::CountStar)
			FailCodegen("HAVING COUNT(*) requires COUNT(*) in the grouped SELECT list.");
		if(!Sel.CountAggregateOutputColumn().empty())
			return Sel.CountAggregateOutputColumn();
		return "cnt";
	case FuncCallExprAST::Kind::CountDistinct:
		if(Sel.AggKind() != GroupAggMode::CountDistinct)
			FailCodegen("HAVING COUNT(DISTINCT …) requires COUNT(DISTINCT …) in the grouped SELECT list.");
		if(!Sel.CountDistinctColumn().has_value() || *Sel.CountDistinctColumn() != F.ArgColumn)
			FailCodegen("HAVING COUNT(DISTINCT column) must match the SELECT COUNT(DISTINCT column).");
		if(!Sel.CountAggregateOutputColumn().empty())
			return Sel.CountAggregateOutputColumn();
		return "cnt";
	case FuncCallExprAST::Kind::Sum:
	case FuncCallExprAST::Kind::Min:
	case FuncCallExprAST::Kind::Max:
	case FuncCallExprAST::Kind::Avg: {
		GroupCombAggKind Want = GroupCombAggKind::Sum;
		if(F.BuiltinKind == FuncCallExprAST::Kind::Min)
			Want = GroupCombAggKind::Min;
		else if(F.BuiltinKind == FuncCallExprAST::Kind::Max)
			Want = GroupCombAggKind::Max;
		else if(F.BuiltinKind == FuncCallExprAST::Kind::Avg)
			Want = GroupCombAggKind::Avg;
		for(const auto &Ca : Sel.ComboAggs()) {
			if(Ca.Kind == Want && Ca.SourceColumn == F.ArgColumn)
				return Ca.OutputColumn;
		}
		FailCodegen("HAVING aggregate does not match a grouped SUM/MIN/MAX/AVG(column) in the SELECT list.");
	}
	default:
		FailCodegen("Internal error: unknown aggregate in HAVING lowering.");
	}
}

static std::unique_ptr<ExpressionAST> NormalizeHavingExpr(const ExpressionAST *Root, const SelectAST &Sel) {
	if(!Root)
		return nullptr;
	if(const auto *F = dynamic_cast<const FuncCallExprAST *>(Root))
		return std::make_unique<ColumnRefAST>(MapHavingAggOutputColumn(*F, Sel));
	if(const auto *B = dynamic_cast<const BinaryOpAST *>(Root)) {
		auto L = NormalizeHavingExpr(B->LHS.get(), Sel);
		auto R = NormalizeHavingExpr(B->RHS.get(), Sel);
		return std::make_unique<BinaryOpAST>(std::move(L), B->Op, std::move(R));
	}
	if(const auto *C = dynamic_cast<const ColumnRefAST *>(Root))
		return std::make_unique<ColumnRefAST>(C->Name);
	if(const auto *L = dynamic_cast<const LiteralAST *>(Root))
		return std::make_unique<LiteralAST>(L->Value);
	if(dynamic_cast<const NullLiteralAST *>(Root))
		return std::make_unique<NullLiteralAST>();
	if(const auto *N = dynamic_cast<const IsNullPredAST *>(Root)) {
		auto Subj = NormalizeHavingExpr(N->Subject.get(), Sel);
		return std::make_unique<IsNullPredAST>(std::move(Subj), N->Negated);
	}
	if(const auto *Btw = dynamic_cast<const BetweenAST *>(Root)) {
		return std::make_unique<BetweenAST>(NormalizeHavingExpr(Btw->Subject.get(), Sel),
		                                    NormalizeHavingExpr(Btw->Low.get(), Sel),
		                                    NormalizeHavingExpr(Btw->High.get(), Sel));
	}
	if(const auto *Ep = dynamic_cast<const ExistsPredAST *>(Root)) {
		auto InnerW = Ep->InnerWhere ? NormalizeHavingExpr(Ep->InnerWhere.get(), Sel) : nullptr;
		return std::make_unique<ExistsPredAST>(Ep->Negated, Ep->InnerTable, std::move(InnerW));
	}
	if(const auto *Ip = dynamic_cast<const InSubqueryPredAST *>(Root)) {
		auto InnerW = Ip->InnerWhere ? NormalizeHavingExpr(Ip->InnerWhere.get(), Sel) : nullptr;
		return std::make_unique<InSubqueryPredAST>(Ip->Negated, Ip->LhsColumn, Ip->InnerTable, Ip->InnerValueColumn,
		                                           std::move(InnerW));
	}
	if(const auto *Iv = dynamic_cast<const InValuesAST *>(Root))
		return std::make_unique<InValuesAST>(Iv->Values);
	FailCodegen("HAVING uses an expression shape that cannot be lowered for grouped columns.");
}

bool BuildWhereDnf(const ExpressionAST *WhereRoot, std::vector<std::vector<RowTriple>> &OutBranches);

static void PutLeI64(std::string &S, int64_t V) {
	for(int B = 0; B < 8; ++B)
		S.push_back(
		    static_cast<char>(static_cast<unsigned char>((static_cast<uint64_t>(V) >> (8 * B)) & 0xFF)));
}

static void PackLenStr(std::string &S, const std::string &X) {
	PutLeI64(S, static_cast<int64_t>(X.size()));
	S.append(X);
}

void AppendDnfOperands(std::vector<Value> &Ops, const std::vector<std::vector<RowTriple>> &Dnf) {
	Ops.push_back(static_cast<int64_t>(Dnf.size()));
	for(const auto &Branch : Dnf) {
		Ops.push_back(static_cast<int64_t>(Branch.size()));
		for(const auto &[Col, O, V] : Branch) {
			Ops.push_back(Col);
			Ops.push_back(O);
			Ops.push_back(V);
		}
	}
}

static void PackDnfOperandsBlob(const std::vector<std::vector<RowTriple>> &Dnf, std::string &Out) {
	Out.clear();
	std::vector<Value> Ops;
	AppendDnfOperands(Ops, Dnf);
	for(const auto &Elem : Ops) {
		if(const auto *Iv = std::get_if<int64_t>(&Elem)) {
			Out.push_back('Q');
			PutLeI64(Out, *Iv);
		} else if(const auto *Sv = std::get_if<std::string>(&Elem)) {
			Out.push_back('S');
			PackLenStr(Out, *Sv);
		} else
			FailCodegen("Internal error: malformed DNF operand while packing bytecode.");
	}
}

bool ColumnNameExpr(const ExpressionAST *E, std::string &Out) {
	const auto *C = dynamic_cast<const ColumnRefAST *>(E);
	if(!C)
		return false;
	Out = C->Name;
	return true;
}

bool LiteralOrNullCsv(const ExpressionAST *E, std::string &Out) {
	if(const auto *L = dynamic_cast<const LiteralAST *>(E)) {
		Out = L->Value;
		return true;
	}
	if(dynamic_cast<const NullLiteralAST *>(E)) {
		Out.clear();
		return true;
	}
	return false;
}

static bool SerializeRowExprAtom(const ExpressionAST *E, std::string &Out) {
	if(const auto *C = dynamic_cast<const ColumnRefAST *>(E)) {
		Out.clear();
		Out.push_back('C');
		PackLenStr(Out, C->Name);
		return true;
	}
	if(const auto *L = dynamic_cast<const LiteralAST *>(E)) {
		Out.clear();
		Out.push_back('L');
		PackLenStr(Out, L->Value);
		return true;
	}
	if(dynamic_cast<const NullLiteralAST *>(E)) {
		Out.clear();
		Out.push_back('N');
		PackLenStr(Out, std::string());
		return true;
	}
	return false;
}

static bool SerializeRowComparePayload(const RowConstructorExprAST *Lr, const RowConstructorExprAST *Rr,
                                       std::string &Out) {
	if(!Lr || !Rr || Lr->Elements.empty() || Lr->Elements.size() != Rr->Elements.size())
		return false;
	Out.clear();
	PutLeI64(Out, static_cast<int64_t>(Lr->Elements.size()));
	for(size_t I = 0; I < Lr->Elements.size(); ++I) {
		std::string Ls;
		std::string Rs;
		if(!SerializeRowExprAtom(Lr->Elements[I].get(), Ls) || !SerializeRowExprAtom(Rr->Elements[I].get(), Rs))
			return false;
		PackLenStr(Out, Ls);
		PackLenStr(Out, Rs);
	}
	return true;
}

static bool SerializeQuantifiedSubqueryLhs(const ExpressionAST *Lhs, std::string &Out) {
	return SerializeRowExprAtom(Lhs, Out);
}

static bool SerializeQuantifiedSubqueryPayload(const ExpressionAST *Lhs, const QuantifiedSubqueryAST *Q,
                                               std::string &Out) {
	if(!Q)
		return false;
	std::string LhsSer;
	if(!SerializeQuantifiedSubqueryLhs(Lhs, LhsSer))
		return false;
	std::vector<std::vector<RowTriple>> Inner;
	if(Q->InnerWhere) {
		if(!BuildWhereDnf(Q->InnerWhere.get(), Inner))
			return false;
	} else
		Inner.push_back({});
	std::string DnfBlob;
	PackDnfOperandsBlob(Inner, DnfBlob);
	Out.clear();
	PutLeI64(Out, static_cast<int64_t>(Q->Kind));
	PackLenStr(Out, LhsSer);
	PackLenStr(Out, Q->InnerTable);
	PackLenStr(Out, Q->InnerValueColumn);
	PackLenStr(Out, DnfBlob);
	return true;
}

bool ExtractOneComparison(const ExpressionAST *Expr, RowTriple &Out) {
	if(!Expr)
		return false;
	const auto *Bin = dynamic_cast<const BinaryOpAST *>(Expr);
	if(!Bin || Bin->Op == "IN" || Bin->Op == "LIKE" || Bin->Op == "NOT LIKE" || Bin->Op == "ILIKE" ||
	    Bin->Op == "NOT ILIKE" || Bin->Op == "GLOB" || Bin->Op == "NOT GLOB" || Bin->Op == "REGEXP" || Bin->Op == "NOT REGEXP" || Bin->Op == "REGEXP_ICASE" || Bin->Op == "NOT REGEXP_ICASE" || Bin->Op == "~" || Bin->Op == "!~" || Bin->Op == "~*" || Bin->Op == "!~*" || Bin->Op == "MATCH" ||
	    Bin->Op == "NOT MATCH")
		return false;

	const auto *LCol = dynamic_cast<const ColumnRefAST *>(Bin->LHS.get());
	const auto *RLit = dynamic_cast<const LiteralAST *>(Bin->RHS.get());
	const auto *RBool = dynamic_cast<const BooleanLiteralAST *>(Bin->RHS.get());
	const auto *RNullRhs = dynamic_cast<const NullLiteralAST *>(Bin->RHS.get());
	const auto *LLit = dynamic_cast<const LiteralAST *>(Bin->LHS.get());
	const auto *LBool = dynamic_cast<const BooleanLiteralAST *>(Bin->LHS.get());
	const auto *LNullLhs = dynamic_cast<const NullLiteralAST *>(Bin->LHS.get());
	const auto *RCol = dynamic_cast<const ColumnRefAST *>(Bin->RHS.get());
	const auto *LRow = dynamic_cast<const RowConstructorExprAST *>(Bin->LHS.get());
	const auto *RRow = dynamic_cast<const RowConstructorExprAST *>(Bin->RHS.get());
	const auto *QSub = dynamic_cast<const QuantifiedSubqueryAST *>(Bin->RHS.get());

	std::string Col;
	std::string Op = Bin->Op;
	std::string Rhs;

	if(LCol && (RLit || RBool || RNullRhs)) {
		Col = LCol->Name;
		if(RBool)
			Rhs = RBool->Value ? "1" : "0";
		else
			Rhs = RLit ? RLit->Value : std::string();
	} else if((LLit || LBool || LNullLhs) && RCol) {
		Col = RCol->Name;
		if(LBool)
			Rhs = LBool->Value ? "1" : "0";
		else
			Rhs = LLit ? LLit->Value : std::string();
		if(Op == "<")
			Op = ">";
		else if(Op == "<=")
			Op = ">=";
		else if(Op == ">")
			Op = "<";
		else if(Op == ">=")
			Op = "<=";
		else if(Op != "=" && Op != "==" && Op != "!=")
			return false;
	} else if(LCol && RCol) {
		Col = LCol->Name;
		if(Op == "==")
			Op = "=";
		Rhs = std::string(kAstRhsColMarker) + RCol->Name;
	} else if(QSub) {
		if(Op != "=" && Op != "==" && Op != "!=" && Op != "<" && Op != "<=" && Op != ">" && Op != ">=")
			return false;
		std::string Payload;
		if(!SerializeQuantifiedSubqueryPayload(Bin->LHS.get(), QSub, Payload))
			return false;
		Out = RowTriple(std::string(kQuantifiedSubqueryCol), std::move(Op), std::move(Payload));
		return true;
	} else if(LRow && RRow) {
		if(Op != "=" && Op != "==" && Op != "!=" && Op != "<" && Op != "<=" && Op != ">" && Op != ">=")
			return false;
		std::string Payload;
		if(!SerializeRowComparePayload(LRow, RRow, Payload))
			return false;
		Out = RowTriple(std::string(kRowCompareCol), std::move(Op), std::move(Payload));
		return true;
	} else
		return false;

	if(Col.empty() || Op.empty())
		return false;
	if(Op != "=" && Op != "==" && Op != "!=" && Op != "<" && Op != "<=" && Op != ">" && Op != ">=")
		return false;
	Out = RowTriple(std::move(Col), std::move(Op), std::move(Rhs));
	return true;
}

bool AtomToTriples(const ExpressionAST *Expr, std::vector<RowTriple> &Out) {
	RowTriple One;
	if(ExtractOneComparison(Expr, One)) {
		Out.push_back(std::move(One));
		return true;
	}

	if(const auto *Sf = dynamic_cast<const ScalarFuncExprAST *>(Expr)) {
		if(Sf->Fn == ScalarSqlFn::RegexpMatch && Sf->Args.size() == 2) {
			std::string Col, Pat;
			if(!ColumnNameExpr(Sf->Args[0].get(), Col))
				return false;
			if(!LiteralOrNullCsv(Sf->Args[1].get(), Pat))
				return false;
			Out.emplace_back(Col, "REGEXP", Pat);
			return true;
		}
	}
	if(const auto *Bl = dynamic_cast<const BooleanLiteralAST *>(Expr)) {
		Out.emplace_back(std::string(kBoolConstCol), std::string(kOpBoolConst), Bl->Value ? "1" : "0");
		return true;
	}

	if(const auto *Ep = dynamic_cast<const ExistsPredAST *>(Expr)) {
		std::vector<std::vector<RowTriple>> Inner;
		if(Ep->InnerWhere) {
			if(!BuildWhereDnf(Ep->InnerWhere.get(), Inner))
				return false;
		} else
			Inner.push_back({});
		std::string Blob;
		PackDnfOperandsBlob(Inner, Blob);
		std::string Payload;
		Payload.reserve(2 + Blob.size());
		Payload.push_back(Ep->Negated ? '1' : '0');
		Payload.push_back('\x1E');
		Payload.append(Blob);
		Out.emplace_back(std::string(kExistCol), Ep->InnerTable, std::move(Payload));
		return true;
	}

	if(const auto *Ip = dynamic_cast<const InSubqueryPredAST *>(Expr)) {
		std::vector<std::vector<RowTriple>> Inner;
		if(Ip->InnerWhere) {
			if(!BuildWhereDnf(Ip->InnerWhere.get(), Inner))
				return false;
		} else
			Inner.push_back({});
		std::string Blob;
		PackDnfOperandsBlob(Inner, Blob);
		std::string Payload;
		Payload.reserve(3 + Ip->InnerValueColumn.size() + Blob.size());
		Payload.push_back(Ip->Negated ? '1' : '0');
		Payload.push_back('\x1E');
		Payload.append(Ip->InnerValueColumn);
		Payload.push_back('\x1E');
		Payload.append(Blob);
		Out.emplace_back(Ip->LhsColumn, std::string(kInSubCol), Ip->InnerTable + "\x1E" + std::move(Payload));
		return true;
	}

	if(const auto *Btw = dynamic_cast<const BetweenAST *>(Expr)) {
		std::string Col;
		std::string Lo, Hi;
		if(!ColumnNameExpr(Btw->Subject.get(), Col))
			return false;
		if(!LiteralOrNullCsv(Btw->Low.get(), Lo) || !LiteralOrNullCsv(Btw->High.get(), Hi))
			return false;
		Out.emplace_back(Col, ">=", Lo);
		Out.emplace_back(Col, "<=", Hi);
		return true;
	}

	if(const auto *Nl = dynamic_cast<const IsNullPredAST *>(Expr)) {
		std::string Col;
		if(!ColumnNameExpr(Nl->Subject.get(), Col))
			return false;
		Out.emplace_back(Col, Nl->Negated ? kOpIsNotNull : kOpIsNull, std::string());
		return true;
	}

	const auto *Bin = dynamic_cast<const BinaryOpAST *>(Expr);
	if(Bin && (Bin->Op == "IN" || Bin->Op == "NOT IN")) {
		std::string Col;
		if(!ColumnNameExpr(Bin->LHS.get(), Col))
			return false;
		const auto *Rhs = dynamic_cast<const InValuesAST *>(Bin->RHS.get());
		if(!Rhs || Rhs->Values.empty())
			return false;
		std::ostringstream O;
		for(size_t I = 0; I < Rhs->Values.size(); ++I) {
			if(I)
				O << '\x1E';
			O << Rhs->Values[I];
		}
		Out.emplace_back(Col, Bin->Op == "NOT IN" ? kOpNotIn : kOpIn, std::move(O).str());
		return true;
	}

	if(Bin && (Bin->Op == "LIKE" || Bin->Op == "NOT LIKE" || Bin->Op == "ILIKE" || Bin->Op == "NOT ILIKE" ||
	           Bin->Op == "GLOB" || Bin->Op == "NOT GLOB" || Bin->Op == "REGEXP" || Bin->Op == "NOT REGEXP" ||
	           Bin->Op == "REGEXP_ICASE" || Bin->Op == "NOT REGEXP_ICASE" || Bin->Op == "~" || Bin->Op == "!~" ||
	           Bin->Op == "~*" || Bin->Op == "!~*")) {
		std::string Col, Pat;
		if(!ColumnNameExpr(Bin->LHS.get(), Col))
			return false;
		if(!LiteralOrNullCsv(Bin->RHS.get(), Pat))
			return false;
		Out.emplace_back(Col, Bin->Op, Pat);
		return true;
	}

	if(Bin && (Bin->Op == "MATCH" || Bin->Op == "NOT MATCH")) {
		std::string Col, Query;
		if(!ColumnNameExpr(Bin->LHS.get(), Col))
			return false;
		if(!LiteralOrNullCsv(Bin->RHS.get(), Query))
			return false;
		Out.emplace_back(Col, Bin->Op, Query);
		return true;
	}
	return false;
}

void FlattenOrBranches(const ExpressionAST *Root, std::vector<const ExpressionAST *> &Out) {
	if(!Root)
		return;
	const auto *B = dynamic_cast<const BinaryOpAST *>(Root);
	if(B && B->Op == "OR") {
		FlattenOrBranches(B->LHS.get(), Out);
		FlattenOrBranches(B->RHS.get(), Out);
		return;
	}
	Out.push_back(Root);
}

void FlattenAndAtoms(const ExpressionAST *Root, std::vector<const ExpressionAST *> &Out) {
	if(!Root)
		return;
	const auto *B = dynamic_cast<const BinaryOpAST *>(Root);
	if(B && B->Op == "AND") {
		FlattenAndAtoms(B->LHS.get(), Out);
		FlattenAndAtoms(B->RHS.get(), Out);
		return;
	}
	Out.push_back(Root);
}

bool BuildWhereDnf(const ExpressionAST *WhereRoot, std::vector<std::vector<RowTriple>> &OutBranches) {
	std::vector<const ExpressionAST *> OrBranches;
	FlattenOrBranches(WhereRoot, OrBranches);
	if(OrBranches.empty())
		return false;
	for(const ExpressionAST *OrNode : OrBranches) {
		std::vector<const ExpressionAST *> Atoms;
		FlattenAndAtoms(OrNode, Atoms);
		std::vector<RowTriple> Conj;
		for(const ExpressionAST *A : Atoms) {
			if(!AtomToTriples(A, Conj))
				return false;
		}
		OutBranches.push_back(std::move(Conj));
	}
	return true;
}

Instruction MakeFilterDnf(const std::vector<std::vector<RowTriple>> &Dnf) {
	Instruction I;
	I.Opcode_ = Opcode::FILTER_DNF;
	AppendDnfOperands(I.Operands, Dnf);
	return I;
}

Instruction MakeDeleteDnf(const std::string &Table, const std::vector<std::vector<RowTriple>> &Dnf) {
	Instruction I;
	I.Opcode_ = Opcode::DELETE_MATCHING;
	I.Operands.push_back(Table);
	AppendDnfOperands(I.Operands, Dnf);
	return I;
}

Instruction MakeUpdateDnf(const std::string &Table,
                            const std::vector<std::pair<std::string, std::unique_ptr<ExpressionAST>>> &Assigns,
                            const std::vector<std::vector<RowTriple>> &Dnf) {
	Instruction I;
	I.Opcode_ = Opcode::UPDATE_MATCHING;
	I.Operands.push_back(Table);
	I.Operands.push_back(static_cast<int64_t>(Assigns.size()));
	for(const auto &[C, E] : Assigns) {
		I.Operands.push_back(C);
		I.Operands.push_back(SerializeSetValueExpr(E.get()));
	}
	AppendDnfOperands(I.Operands, Dnf);
	return I;
}

static void AppendCaseScalarPayload(Instruction &Inst, const ExpressionAST *E) {
	if(const auto *L = dynamic_cast<const LiteralAST *>(E)) {
		Inst.Operands.push_back(static_cast<int64_t>(0));
		Inst.Operands.push_back(L->Value);
	} else if(const auto *C = dynamic_cast<const ColumnRefAST *>(E)) {
		Inst.Operands.push_back(static_cast<int64_t>(1));
		Inst.Operands.push_back(C->Name);
	} else if(dynamic_cast<const NullLiteralAST *>(E)) {
		Inst.Operands.push_back(static_cast<int64_t>(2));
		Inst.Operands.push_back(std::string());
	} else if(const auto *Lam = dynamic_cast<const LambdaExprAST *>(E)) {
		Inst.Operands.push_back(static_cast<int64_t>(5));
		Inst.Operands.push_back(SerializeLambdaExpr(*Lam));
	} else
		FailCodegen("CASE THEN/ELSE must be a literal, column, NULL, or lambda in this dialect.");
}

static unsigned GScalarMaterializeCounter = 0;

static void EmitCaseEvalInstruction(const CaseExprAST &C, const std::string &OutCol,
                                    BytecodeScratch &Instructions);

static void EmitCoalesceForColumn(const CoalesceExprAST &C, const std::string &OutCol,
                                  BytecodeScratch &Instructions);
static void EmitNvl2ForColumn(const Nvl2ExprAST &N, const std::string &OutCol, BytecodeScratch &Instructions);
static void EmitCastEvalInstruction(const CastExprAST &C, const std::string &OutCol,
                                    BytecodeScratch &Instructions);
static void EmitScalarFuncEvalInstruction(const ScalarFuncExprAST &F, const std::string &OutCol,
                                          BytecodeScratch &Instructions);
static void EmitBinaryArithForColumn(const BinaryOpAST &B, const std::string &OutCol,
                                     BytecodeScratch &Instructions);

static std::string MaterializeScalarToColumn(const ExpressionAST *E, BytecodeScratch &Instructions) {
	if(const auto *C = dynamic_cast<const ColumnRefAST *>(E))
		return C->Name;
	const std::string Tmp = std::string("_sxt") + std::to_string(GScalarMaterializeCounter++);
	if(const auto *L = dynamic_cast<const LiteralAST *>(E)) {
		std::vector<CaseExprAST::Arm> NoArms;
		EmitCaseEvalInstruction(CaseExprAST(std::move(NoArms), std::make_unique<LiteralAST>(L->Value)), Tmp,
		                        Instructions);
		return Tmp;
	}
	if(dynamic_cast<const NullLiteralAST *>(E)) {
		std::vector<CaseExprAST::Arm> NoArms;
		EmitCaseEvalInstruction(CaseExprAST(std::move(NoArms), std::make_unique<NullLiteralAST>()), Tmp,
		                        Instructions);
		return Tmp;
	}
	if(const auto *Ca = dynamic_cast<const CastExprAST *>(E)) {
		EmitCastEvalInstruction(*Ca, Tmp, Instructions);
		return Tmp;
	}
	if(const auto *Sf = dynamic_cast<const ScalarFuncExprAST *>(E)) {
		EmitScalarFuncEvalInstruction(*Sf, Tmp, Instructions);
		return Tmp;
	}
	if(const auto *Co = dynamic_cast<const CoalesceExprAST *>(E)) {
		EmitCoalesceForColumn(*Co, Tmp, Instructions);
		return Tmp;
	}
	if(const auto *Nv = dynamic_cast<const Nvl2ExprAST *>(E)) {
		EmitNvl2ForColumn(*Nv, Tmp, Instructions);
		return Tmp;
	}
	if(const auto *Ce = dynamic_cast<const CaseExprAST *>(E)) {
		EmitCaseEvalInstruction(*Ce, Tmp, Instructions);
		return Tmp;
	}
	if(const auto *B = dynamic_cast<const BinaryOpAST *>(E)) {
		EmitBinaryArithForColumn(*B, Tmp, Instructions);
		return Tmp;
	}
	if(const auto *Lam = dynamic_cast<const LambdaExprAST *>(E)) {
		std::vector<CaseExprAST::Arm> NoArms;
		EmitCaseEvalInstruction(
		    CaseExprAST(std::move(NoArms), std::make_unique<LiteralAST>(SerializeLambdaExpr(*Lam))), Tmp,
		    Instructions);
		return Tmp;
	}
	FailCodegen("Unsupported scalar expression in COALESCE/NVL2/CAST projection.");
}

static void EmitBinaryArithForColumn(const BinaryOpAST &B, const std::string &OutCol,
                                     BytecodeScratch &Instructions) {
	if(B.Op != "+" && B.Op != "-" && B.Op != "*" && B.Op != "/" && B.Op != "//")
		FailCodegen("Unsupported arithmetic operator in SELECT projection.");
	const std::string L = MaterializeScalarToColumn(B.LHS.get(), Instructions);
	const std::string R = MaterializeScalarToColumn(B.RHS.get(), Instructions);
	Instruction I;
	I.Opcode_ = Opcode::SCALAR_ARITH_EVAL;
	I.Operands.push_back(OutCol);
	I.Operands.push_back(B.Op);
	I.Operands.push_back(L);
	I.Operands.push_back(R);
	Instructions.push_back(std::move(I));
}

static void EmitCoalesceForColumn(const CoalesceExprAST &C, const std::string &OutCol,
                                  BytecodeScratch &Instructions) {
	if(C.Args.empty())
		FailCodegen("COALESCE requires at least one argument.");
	if(C.Args.size() == 1) {
		if(const auto *Col = dynamic_cast<const ColumnRefAST *>(C.Args[0].get())) {
			std::vector<CaseExprAST::Arm> NoArms;
			EmitCaseEvalInstruction(
			    CaseExprAST(std::move(NoArms), std::make_unique<ColumnRefAST>(Col->Name)), OutCol, Instructions);
		} else {
			const std::string Tmp = MaterializeScalarToColumn(C.Args[0].get(), Instructions);
			std::vector<CaseExprAST::Arm> NoArms;
			EmitCaseEvalInstruction(
			    CaseExprAST(std::move(NoArms), std::make_unique<ColumnRefAST>(Tmp)), OutCol, Instructions);
		}
		return;
	}
	std::vector<CaseExprAST::Arm> Arms;
	for(size_t I = 0; I + 1 < C.Args.size(); ++I) {
		const std::string Tmp = MaterializeScalarToColumn(C.Args[I].get(), Instructions);
		CaseExprAST::Arm A;
		A.When = std::make_unique<IsNullPredAST>(std::make_unique<ColumnRefAST>(Tmp), true);
		A.Then = std::make_unique<ColumnRefAST>(Tmp);
		Arms.push_back(std::move(A));
	}
	const std::string ElseTmp = MaterializeScalarToColumn(C.Args.back().get(), Instructions);
	EmitCaseEvalInstruction(
	    CaseExprAST(std::move(Arms), std::make_unique<ColumnRefAST>(ElseTmp)), OutCol, Instructions);
}

static void EmitNvl2ForColumn(const Nvl2ExprAST &N, const std::string &OutCol, BytecodeScratch &Instructions) {
	const std::string Subj = MaterializeScalarToColumn(N.Subject.get(), Instructions);
	const std::string NotNull = MaterializeScalarToColumn(N.NotNullVal.get(), Instructions);
	const std::string NullBr = MaterializeScalarToColumn(N.NullVal.get(), Instructions);
	CaseExprAST::Arm A;
	A.When = std::make_unique<IsNullPredAST>(std::make_unique<ColumnRefAST>(Subj), true);
	A.Then = std::make_unique<ColumnRefAST>(NotNull);
	std::vector<CaseExprAST::Arm> Arms;
	Arms.push_back(std::move(A));
	EmitCaseEvalInstruction(CaseExprAST(std::move(Arms), std::make_unique<ColumnRefAST>(NullBr)), OutCol,
	                        Instructions);
}

static void EmitCaseEvalInstruction(const CaseExprAST &C, const std::string &OutCol,
                                    BytecodeScratch &Instructions) {
	if(C.Arms.empty() && !C.ElseResult)
		FailCodegen("CASE requires at least one WHEN arm or ELSE.");
	if(C.Arms.size() > Limits::MaxCaseWhenArms)
		FailCodegen("CASE WHEN arm count exceeds configured limit (Limits::MaxCaseWhenArms).");
	Instruction I;
	I.Opcode_ = Opcode::CASE_EVAL;
	I.Operands.push_back(OutCol);
	I.Operands.push_back(static_cast<int64_t>(C.Arms.size()));
	for(const auto &Arm : C.Arms) {
		if(!Arm.When)
			FailCodegen("CASE WHEN arm missing predicate.");
		std::vector<std::vector<RowTriple>> Branches;
		if(!BuildWhereDnf(Arm.When.get(), Branches))
			FailCodegen(
			    "CASE WHEN predicate shape is not supported for this statement (simplify or extend the WHERE grammar).");
		std::string Blob;
		PackDnfOperandsBlob(Branches, Blob);
		I.Operands.push_back(std::move(Blob));
		if(!Arm.Then)
			FailCodegen("CASE WHEN arm missing THEN expression.");
		AppendCaseScalarPayload(I, Arm.Then.get());
	}
	if(C.ElseResult)
		AppendCaseScalarPayload(I, C.ElseResult.get());
	else {
		I.Operands.push_back(static_cast<int64_t>(3));
		I.Operands.push_back(std::string());
	}
	Instructions.push_back(std::move(I));
}

static void EmitCastEvalInstruction(const CastExprAST &C, const std::string &OutCol, BytecodeScratch &Instructions) {
	if(!C.Operand)
		FailCodegen("CAST missing operand expression.");
	if(!dynamic_cast<const LiteralAST *>(C.Operand.get()) &&
	   !dynamic_cast<const ColumnRefAST *>(C.Operand.get()) &&
	   !dynamic_cast<const NullLiteralAST *>(C.Operand.get())) {
		const std::string Tmp = MaterializeScalarToColumn(C.Operand.get(), Instructions);
		CastExprAST Simple(std::make_unique<ColumnRefAST>(Tmp), C.Target, C.TargetTypeSql);
		EmitCastEvalInstruction(Simple, OutCol, Instructions);
		return;
	}
	Instruction I;
	I.Opcode_ = Opcode::CAST_EVAL;
	I.Operands.push_back(OutCol);
	AppendCaseScalarPayload(I, C.Operand.get());
	I.Operands.push_back(static_cast<int64_t>(C.Target));
	if(C.Target == SqlCastTarget::Advanced) {
		if(C.TargetTypeSql.empty())
			FailCodegen("CAST to advanced type missing type spelling.");
		I.Operands.push_back(C.TargetTypeSql);
	}
	Instructions.push_back(std::move(I));
}

static void EmitScalarFuncEvalInstruction(const ScalarFuncExprAST &F, const std::string &OutCol,
                                          BytecodeScratch &Instructions) {
	if(F.Args.size() > Limits::MaxScalarSqlFuncArgs)
		FailCodegen("Too many scalar function arguments (see Limits::MaxScalarSqlFuncArgs).");
	size_t Need = 0;
	switch(F.Fn) {
	case ScalarSqlFn::Now:
	case ScalarSqlFn::CurrentDate:
	case ScalarSqlFn::CurrentTime:
	case ScalarSqlFn::CurrentTimestamp:
		Need = 0;
		break;
	case ScalarSqlFn::Upper:
	case ScalarSqlFn::Lower:
	case ScalarSqlFn::CharLength:
	case ScalarSqlFn::TrimBoth:
	case ScalarSqlFn::TrimLeading:
	case ScalarSqlFn::TrimTrailing:
	case ScalarSqlFn::ExtractYear:
	case ScalarSqlFn::ExtractMonth:
	case ScalarSqlFn::ExtractDay:
	case ScalarSqlFn::ExtractHour:
	case ScalarSqlFn::ExtractMinute:
	case ScalarSqlFn::ExtractSecond:
	case ScalarSqlFn::ExtractEpoch:
		Need = 1;
		break;
	case ScalarSqlFn::SubstringFromFor:
		if(F.Args.size() != 2 && F.Args.size() != 3)
			FailCodegen("SUBSTRING expects two or three arguments.");
		Need = F.Args.size();
		break;
	case ScalarSqlFn::PositionIn:
	case ScalarSqlFn::DateAddDays:
	case ScalarSqlFn::DateSubDays:
	case ScalarSqlFn::DateDiffDays:
	case ScalarSqlFn::TimeBucketSeconds:
	case ScalarSqlFn::DateTrunc:
	case ScalarSqlFn::TimestampDiffSeconds:
	case ScalarSqlFn::DateAddSeconds:
		Need = 2;
		break;
	case ScalarSqlFn::AtTimeZone:
		Need = 2;
		break;
	case ScalarSqlFn::ConvertTimezone:
		Need = 3;
		break;
	case ScalarSqlFn::ConcatVariadic:
		if(F.Args.size() < 2)
			FailCodegen("CONCAT expects at least two arguments.");
		Need = F.Args.size();
		break;
	case ScalarSqlFn::Grouping:
	case ScalarSqlFn::ComplexReal:
	case ScalarSqlFn::ComplexImag:
	case ScalarSqlFn::VectorNorm:
		Need = 1;
		break;
	case ScalarSqlFn::StructField:
	case ScalarSqlFn::MapGet:
	case ScalarSqlFn::ComplexMul:
	case ScalarSqlFn::VectorDot:
	case ScalarSqlFn::VectorAdd:
	case ScalarSqlFn::MatrixVec:
		Need = 2;
		break;
	case ScalarSqlFn::RegexpMatch:
		Need = 2;
		break;
	case ScalarSqlFn::ListTransform:
		Need = 2;
		break;
	default:
		if(MathSci::IsMathSciScalarFn(F.Fn)) {
			const auto Ar = MathSci::ArityFor(F.Fn);
			if(Ar.Min == 0 && Ar.Max == 0) {
				if(!F.Args.empty())
					FailCodegen("Math/sci builtin expects zero arguments.");
				Need = 0;
			} else {
				if(static_cast<int>(F.Args.size()) < Ar.Min || static_cast<int>(F.Args.size()) > Ar.Max)
					FailCodegen("Math/sci builtin argument count mismatch.");
				Need = F.Args.size();
			}
			break;
		}
		FailCodegen("Internal: unsupported scalar SQL function.");
	}
	if(F.Fn != ScalarSqlFn::ConcatVariadic && F.Args.size() != Need)
		FailCodegen("Scalar builtin argument count mismatch.");
	Instruction I;
	I.Opcode_ = Opcode::SCALAR_FUNC_EVAL;
	I.Operands.push_back(OutCol);
	I.Operands.push_back(ScalarSqlFnTag(F.Fn));
	I.Operands.push_back(static_cast<int64_t>(F.Args.size()));
	for(const auto &A : F.Args) {
		if(!A)
			FailCodegen("Internal: scalar builtin null argument.");
		if(dynamic_cast<const ScalarFuncExprAST *>(A.get())) {
			const std::string SrcCol = MaterializeScalarToColumn(A.get(), Instructions);
			I.Operands.push_back(static_cast<int64_t>(1));
			I.Operands.push_back(SrcCol);
		} else {
			AppendCaseScalarPayload(I, A.get());
		}
	}
	Instructions.push_back(std::move(I));
}

static void EmitGroupingEvalInstruction(const GroupingExprAST &G, const std::string &OutCol,
                                      BytecodeScratch &Instructions) {
	Instruction I;
	I.Opcode_ = Opcode::SCALAR_FUNC_EVAL;
	I.Operands.push_back(OutCol);
	I.Operands.push_back(ScalarSqlFnTag(ScalarSqlFn::Grouping));
	I.Operands.push_back(1LL);
	I.Operands.push_back(0LL);
	I.Operands.push_back(G.Column);
	Instructions.push_back(std::move(I));
}

static void EmitGroupingIdEvalInstruction(const GroupingIdExprAST &G, const std::string &OutCol,
                                          BytecodeScratch &Instructions) {
	Instruction I;
	I.Opcode_ = Opcode::SCALAR_FUNC_EVAL;
	I.Operands.push_back(OutCol);
	I.Operands.push_back(ScalarSqlFnTag(ScalarSqlFn::GroupingId));
	I.Operands.push_back(static_cast<int64_t>(G.Columns.size()));
	for(const std::string &C : G.Columns) {
		I.Operands.push_back(0LL);
		I.Operands.push_back(C);
	}
	Instructions.push_back(std::move(I));
}

static void CompileCheckSqlToDnfPackedOrThrowImpl(std::string_view Sql, std::string &OutPacked) {
	if(Sql.empty())
		FailCodegen("CHECK constraint has an empty predicate.");
	Parser ParserCtx(Sql, ParserTokenizeOnly);
	auto Ex = ParserCtx.ParseStandalonePredicateExpression();
	std::vector<std::vector<RowTriple>> Branches;
	if(!BuildWhereDnf(Ex.get(), Branches))
		FailCodegen(
		    "CHECK constraint is not supported for this bytecode path (predicate must fold like a WHERE clause).");
	PackDnfOperandsBlob(Branches, OutPacked);
}

/** Emits constraint PUSH tokens possibly expanding CHECK → CHECK + CHECKDNF (two pseudotokens per CHECK). */
static void EmitCodegenColumnConstraints(const std::vector<std::string> &Constraints,
                                         BytecodeScratch &Instructions) {
	int64_t NConsEmitted = 0;
	for(const auto &Constraint : Constraints) {
		if(Constraint.rfind("CHECK:", 0) == 0)
			NConsEmitted += 2;
		else
			NConsEmitted += 1;
	}
	if(NConsEmitted > 128)
		FailCodegen("Too many bytecode constraint pseudotokens for one column.");
	Instructions.push_back(MakeInstruction(Opcode::PUSH, static_cast<int64_t>(NConsEmitted)));
	for(const auto &Constraint : Constraints) {
		if(Constraint.rfind("CHECK:", 0) == 0) {
			std::string Blob;
			const std::string Body = Constraint.size() > 6 ? Constraint.substr(6) : std::string();
			CompileCheckSqlToDnfPackedOrThrowImpl(Body, Blob);
			AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, Constraint));
			AppendInstruction(Instructions,
			                  MakeInstruction(Opcode::PUSH, std::string("CHECKDNF:") + std::move(Blob)));
		} else
			AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, Constraint));
	}
}

} // namespace

void CoalesceExprAST::EmitBytecode(BytecodeScratch &Instructions) const {
	(void)Instructions;
	FailCodegen("Internal: COALESCE projection is lowered via Opcode::CASE_EVAL in codegen.");
}

void Nvl2ExprAST::EmitBytecode(BytecodeScratch &Instructions) const {
	(void)Instructions;
	FailCodegen("Internal: NVL2 projection is lowered via Opcode::CASE_EVAL in codegen.");
}

void LambdaExprAST::EmitBytecode(BytecodeScratch &Instructions) const {
	(void)Instructions;
	FailCodegen("Internal: lambda expressions are lowered at their call site.");
}

void ColumnsExprAST::EmitBytecode(BytecodeScratch &Instructions) const {
	(void)Instructions;
	FailCodegen("Internal: COLUMNS(...) is expanded via Opcode::COLUMNS_EXPAND.");
}

void NullLiteralAST::EmitBytecode(BytecodeScratch &) const {
	FailCodegen("Internal: NULL literal should not emit standalone bytecode.");
}

void BooleanLiteralAST::EmitBytecode(BytecodeScratch &) const {
	FailCodegen("Internal: boolean literal should not emit standalone bytecode.");
}

void InValuesAST::EmitBytecode(BytecodeScratch &) const {
	FailCodegen("Internal: IN list should not emit standalone bytecode.");
}

void BetweenAST::EmitBytecode(BytecodeScratch &) const {
	FailCodegen("Internal: BETWEEN should not emit standalone bytecode.");
}

void IsNullPredAST::EmitBytecode(BytecodeScratch &) const {
	FailCodegen("Internal: IS NULL should not emit standalone bytecode.");
}

void ExistsPredAST::EmitBytecode(BytecodeScratch &) const {
	FailCodegen("Internal: EXISTS should not emit standalone bytecode.");
}

void InSubqueryPredAST::EmitBytecode(BytecodeScratch &) const {
	FailCodegen("Internal: IN subquery should not emit standalone bytecode.");
}

void QuantifiedSubqueryAST::EmitBytecode(BytecodeScratch &) const {
	FailCodegen("Internal: quantified subquery predicate should not emit standalone bytecode.");
}

void RowConstructorExprAST::EmitBytecode(BytecodeScratch &) const {
	FailCodegen("Internal: row constructor expression should not emit standalone bytecode.");
}

void CaseExprAST::EmitBytecode(BytecodeScratch &) const {
	FailCodegen("Internal: CASE projection is lowered via Opcode::CASE_EVAL, not standalone bytecode.");
}

void CastExprAST::EmitBytecode(BytecodeScratch &) const {
	FailCodegen("Internal: CAST projection is lowered via Opcode::CAST_EVAL, not standalone bytecode.");
}

void ScalarFuncExprAST::EmitBytecode(BytecodeScratch &) const {
	FailCodegen("Internal: scalar builtin projection is lowered via Opcode::SCALAR_FUNC_EVAL.");
}

void LiteralAST::EmitBytecode(BytecodeScratch& Instructions) const {
	AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, Value));
}

void ColumnRefAST::EmitBytecode(BytecodeScratch& Instructions) const {
	AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, Name));
}

void QualifiedRefAST::EmitBytecode(BytecodeScratch &Instructions) const {
	(void)Instructions;
	FailCodegen("QualifiedRefAST is lowered via SET expression serialization, not standalone bytecode.");
}

void GroupingExprAST::EmitBytecode(BytecodeScratch &Instructions) const {
	(void)Instructions;
	FailCodegen("GROUPING() is lowered via EmitGroupingEvalInstruction in SELECT codegen.");
}

void GroupingIdExprAST::EmitBytecode(BytecodeScratch &Instructions) const {
	(void)Instructions;
	FailCodegen("GROUPING_ID() is lowered via EmitGroupingIdEvalInstruction in SELECT codegen.");
}

void CreateTypeAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions,
	                  MakeInstruction(Opcode::CREATE_TYPE, TypeName, static_cast<int64_t>(Fields.size())));
	for(const auto &F : Fields) {
		AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, F.Name));
		AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, F.Type));
	}
}

void DropTypeAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions, MakeInstruction(Opcode::DROP_TYPE, TypeName, static_cast<int64_t>(IfExists ? 1 : 0)));
}

void DropAST::EmitBytecode(BytecodeScratch& Instructions) const {
	const int64_t Flags = static_cast<int64_t>((IfExists ? 1 : 0) | (Cascade ? 2 : 0));
	AppendInstruction(Instructions, MakeInstruction(Opcode::DROP_TABLE, TableName, Flags));
}

void TableAST::EmitBytecode(BytecodeScratch& Instructions) const {
	AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, TableName));
}

void CreateAST::EmitBytecode(BytecodeScratch& Instructions) const {
	const int64_t Flags = static_cast<int64_t>(IfNotExists ? 1 : 0);
	AppendInstruction(Instructions,
	                   MakeInstruction(Opcode::CREATE_TABLE, TableName, static_cast<int64_t>(Columns.size()), Flags,
	                                   static_cast<int64_t>(TableConstraints.size()),
	                                   static_cast<int64_t>(StoragePolicy), OfTypeName));
	for(const auto &Column : Columns) {
		AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, Column.Name));
		AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, Column.Type));
		EmitCodegenColumnConstraints(Column.Constraints, Instructions);
	}
	for(const auto &TC : TableConstraints) {
		if(TC.Kind == TableConstraintKind::PrimaryKey) {
			AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, std::string("__PK__")));
			AppendInstruction(Instructions,
			                   MakeInstruction(Opcode::PUSH, static_cast<int64_t>(TC.Columns.size())));
			for(const auto &Cn : TC.Columns)
				AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, Cn));
		} else if(TC.Kind == TableConstraintKind::Unique) {
			AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, std::string("__UQ__")));
			AppendInstruction(Instructions,
			                   MakeInstruction(Opcode::PUSH, static_cast<int64_t>(TC.Columns.size())));
			for(const auto &Cn : TC.Columns)
				AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, Cn));
		} else if(TC.Kind == TableConstraintKind::ForeignKey) {
			if(TC.Columns.size() != TC.RefColumns.size())
				FailCodegen("FOREIGN KEY column count must match REFERENCES column count.");
			AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, std::string("__FK__")));
			AppendInstruction(Instructions,
			                   MakeInstruction(Opcode::PUSH, static_cast<int64_t>(TC.Columns.size())));
			for(const auto &Cn : TC.Columns)
				AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, Cn));
			AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, TC.RefTable));
			AppendInstruction(Instructions,
			                   MakeInstruction(Opcode::PUSH, static_cast<int64_t>(TC.RefColumns.size())));
			for(const auto &Rc : TC.RefColumns)
				AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, Rc));
			AppendInstruction(Instructions,
			                   MakeInstruction(Opcode::PUSH, static_cast<int64_t>(TC.OnDeleteAction)));
		} else if(TC.Kind == TableConstraintKind::Check) {
			std::string Blob;
			CompileCheckSqlToDnfPackedOrThrowImpl(TC.CheckSql, Blob);
			AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, std::string("__CK__")));
			AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, TC.CheckSql));
			AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, std::move(Blob)));
		}
	}
}

void AlterTableAST::EmitBytecode(BytecodeScratch& Instructions) const {
	Instruction Alter;
	Alter.Opcode_ = Opcode::ALTER_TABLE;
	if(Kind == AlterTableKind::AddColumn) {
		BytecodeScratch Expanded;
		EmitCodegenColumnConstraints(AddedColumn.Constraints, Expanded);
		std::vector<Value> ExpandedOps;
		for(const auto &Ix : Expanded) {
			if(Ix.Opcode_ != Opcode::PUSH || Ix.Operands.size() != 1)
				FailCodegen("Internal: malformed expanded ALTER constraints.");
			ExpandedOps.push_back(Ix.Operands[0]);
		}
		if(ExpandedOps.empty() || ExpandedOps.size() < 2 ||
		   !std::holds_alternative<int64_t>(ExpandedOps[0]))
			FailCodegen("Internal: malformed ALTER constraint count encoding.");
		Alter.Operands.push_back(static_cast<int64_t>(0));
		Alter.Operands.push_back(TableName);
		Alter.Operands.push_back(AddedColumn.Name);
		Alter.Operands.push_back(AddedColumn.Type);
		Alter.Operands.push_back(std::move(ExpandedOps[0]));
		for(size_t Ei = 1; Ei < ExpandedOps.size(); ++Ei)
			Alter.Operands.push_back(std::move(ExpandedOps[Ei]));
	} else if(Kind == AlterTableKind::SetStorage) {
		Alter.Operands.push_back(static_cast<int64_t>(3));
		Alter.Operands.push_back(TableName);
		Alter.Operands.push_back(static_cast<int64_t>(StoragePolicy));
	} else if(Kind == AlterTableKind::DropColumn) {
		Alter.Operands.push_back(static_cast<int64_t>(1));
		Alter.Operands.push_back(TableName);
		Alter.Operands.push_back(DropColumnName);
	} else {
		Alter.Operands.push_back(static_cast<int64_t>(2));
		Alter.Operands.push_back(TableName);
		Alter.Operands.push_back(RenameFrom);
		Alter.Operands.push_back(RenameTo);
	}
	Instructions.push_back(std::move(Alter));
}

void SavepointAST::EmitBytecode(BytecodeScratch& Instructions) const {
	if(Kind == SavepointStmtKind::Set)
		AppendInstruction(Instructions, MakeInstruction(Opcode::SAVEPOINT, Name));
	else if(Kind == SavepointStmtKind::RollbackTo)
		AppendInstruction(Instructions, MakeInstruction(Opcode::ROLLBACK_TO, Name));
	else if(Kind == SavepointStmtKind::Release)
		AppendInstruction(Instructions, MakeInstruction(Opcode::RELEASE_SAVEPOINT, Name));
}

void DataExchangeAST::EmitBytecode(BytecodeScratch& Instructions) const {
	auto UpFmt = [](std::string V) -> std::string {
		for(char &C : V)
			C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
		return V;
	};
	if(Kind == DataExchangeKind::ExportDatabase)
		AppendInstruction(Instructions,
		                   MakeInstruction(Opcode::EXPORT_DATABASE, Path, UpFmt(Format)));
	else if(Kind == DataExchangeKind::ImportDatabase)
		AppendInstruction(Instructions,
		                   MakeInstruction(Opcode::IMPORT_DATABASE, Path, UpFmt(Format)));
	else
		AppendInstruction(
		    Instructions,
		    MakeInstruction(Opcode::CONVERT_TABULAR_FILES, Path, DestPath, UpFmt(Format), UpFmt(DestFormat)));
}

void SelectAST::EmitBytecode(BytecodeScratch& Instructions) const {
	EmitRelationPipeline(Table_, "__AstralJoin_", Instructions);
	EmitOrderingFinalize(Instructions);
}

void SelectAST::EmitBytecodeForScratchTable(const std::string &ScratchPhysicalName,
                                             BytecodeScratch& Instructions) const {
	EmitRelationPipeline(ScratchPhysicalName, "__AstralJoin_", Instructions);
	EmitOrderingFinalize(Instructions);
}

namespace {

Opcode SqlJoinToOpcode(SqlJoinKind K) {
	switch(K) {
	case SqlJoinKind::Inner:
		return Opcode::INNER_JOIN;
	case SqlJoinKind::Left:
		return Opcode::LEFT_JOIN;
	case SqlJoinKind::Right:
		return Opcode::RIGHT_JOIN;
	case SqlJoinKind::Full:
		return Opcode::FULL_JOIN;
	case SqlJoinKind::Cross:
		return Opcode::CROSS_JOIN;
	}
	return Opcode::INNER_JOIN;
}

} // namespace

void SelectAST::EmitRelationPipeline(const std::string &MaterializedTable, const std::string &JoinDestPrefix,
                                     BytecodeScratch &Instructions) const {
	if(StorageHint_.has_value())
		AppendInstruction(Instructions,
		                  MakeInstruction(Opcode::STORAGE_HINT, static_cast<int64_t>(*StorageHint_)));
	const auto &Grp = GroupKeys();
	/** Fallback if parser lost \c AggMode but projection includes \c cnt with GROUP BY. */
	const bool HeuristicAgg = !Grp.empty() &&
	        std::find(Columns_.begin(), Columns_.end(), std::string("cnt")) != Columns_.end();
	const bool AggCount = AggKind() == GroupAggMode::CountStar || AggKind() == GroupAggMode::CountDistinct ||
	                      HeuristicAgg;
	if(AggKind() == GroupAggMode::CountDistinct && !ComboAggs().empty())
		FailCodegen("COUNT(DISTINCT column) cannot be combined with SUM/MIN/MAX/AVG in this dialect.");
	if(AggKind() == GroupAggMode::CountDistinct && !CountDistinctColumn().has_value())
		FailCodegen("Internal: COUNT(DISTINCT) missing column name.");
	if(AggKind() == GroupAggMode::CountDistinct && Grp.empty())
		FailCodegen("COUNT(DISTINCT column) requires a GROUP BY clause in this SQL dialect.");
	if((AggKind() == GroupAggMode::CountStar || HeuristicAgg) && Grp.empty())
		FailCodegen("COUNT(*) requires a GROUP BY clause in this SQL dialect.");
	if(!ComboAggs().empty() && Grp.empty())
		FailCodegen("SUM, MIN, MAX, and AVG require a GROUP BY clause when used in the SELECT list.");
	if(OlapKind() != GroupOlapModifier::None && Grp.empty())
		FailCodegen("ROLLUP, CUBE, and GROUPING SETS require GROUP BY columns.");
	if(OlapKind() != GroupOlapModifier::None && AggKind() == GroupAggMode::CountDistinct)
		FailCodegen("COUNT(DISTINCT) cannot be combined with ROLLUP, CUBE, or GROUPING SETS.");
	if(OlapKind() == GroupOlapModifier::GroupingSets && GroupingSets().empty())
		FailCodegen("GROUPING SETS requires at least one grouping set.");

	std::string Work = ResolveRelationLogicalName(MaterializedTable, Instructions);
	std::string FromBase = ResolveRelationLogicalName(Table_, Instructions);
	if(IsDialectDualTable(Table_))
		FromBase = std::string(kDialectDualInternal);
	if(Work != FromBase)
		AppendInstruction(Instructions, MakeInstruction(Opcode::CLONE_TABLE, Work, FromBase));
	for(size_t Ij = 0; Ij < JoinSpecs().size(); ++Ij) {
		const JoinClause &Jc = JoinSpecs()[Ij];
		if(Jc.IsLateral)
			FailCodegen("LATERAL JOIN is parsed but this join shape is not lowered yet.");
		const std::string Dst = JoinDestPrefix + std::to_string(Ij);
		Instruction Ji;
		Ji.Opcode_ = SqlJoinToOpcode(Jc.Kind);
		Ji.Operands.push_back(Dst);
		Ji.Operands.push_back(Work);
		Ji.Operands.push_back(ResolveRelationLogicalName(Jc.RightTable, Instructions));
		Ji.Operands.push_back(static_cast<int64_t>(Jc.OnPairs.size()));
		for(const auto &Pr : Jc.OnPairs) {
			Ji.Operands.push_back(Pr.first);
			Ji.Operands.push_back(Pr.second);
		}
		Instructions.push_back(std::move(Ji));
		Work = Dst;
	}

	if(!JoinSpecs().empty() && Work != MaterializedTable && MaterializedTable != FromBase) {
		AppendInstruction(Instructions, MakeInstruction(Opcode::CLONE_TABLE, MaterializedTable, Work));
		Work = MaterializedTable;
	}

	if(AsOfTimestamp()) {
		Instruction AsOfI;
		AsOfI.Opcode_ = Opcode::FILTER_AS_OF;
		AsOfI.Operands.push_back(Work);
		AsOfI.Operands.push_back(*AsOfTimestamp());
		Instructions.push_back(std::move(AsOfI));
	}
	if(MatchRecognize()) {
		const MatchRecognizeSpec &Mr = *MatchRecognize();
		Instruction Mi;
		Mi.Opcode_ = Opcode::MATCH_RECOGNIZE;
		Mi.Operands.push_back(Work);
		Mi.Operands.push_back(Mr.OrderColumn.empty() ? std::string("_rowid") : Mr.OrderColumn);
		Mi.Operands.push_back(Mr.Pattern);
		Mi.Operands.push_back(static_cast<int64_t>(Mr.Defines.size()));
		for(const auto &Def : Mr.Defines) {
			std::vector<std::vector<RowTriple>> Branches;
			if(Def.Predicate && !BuildWhereDnf(Def.Predicate.get(), Branches))
				FailCodegen("MATCH_RECOGNIZE DEFINE predicate is not supported for DNF lowering.");
			std::string Blob;
			if(Def.Predicate)
				PackDnfOperandsBlob(Branches, Blob);
			Mi.Operands.push_back(Def.Symbol);
			Mi.Operands.push_back(Blob);
		}
		Instructions.push_back(std::move(Mi));
	}

	size_t StaticSelectCols = 0;
	for(const auto &Column : Columns_) {
		if(Column.rfind(kColumnsExpandPrefix, 0) == 0) {
			continue;
		}
		++StaticSelectCols;
		AppendInstruction(Instructions, MakeInstruction(Opcode::SELECT, Column));
	}
	for(size_t Pi = 0; Pi < ProjectionExprs_.size(); ++Pi) {
		if(const auto *Cx = dynamic_cast<const ColumnsExprAST *>(ProjectionExprs_[Pi].get())) {
			Instruction Ci;
			Ci.Opcode_ = Opcode::COLUMNS_EXPAND;
			Ci.Operands.push_back(Work);
			Ci.Operands.push_back(static_cast<int64_t>(static_cast<int>(Cx->Mode)));
			if(Cx->Mode == ColumnsPickMode::Glob)
				Ci.Operands.push_back(Cx->GlobPattern);
			else if(Cx->Mode == ColumnsPickMode::Lambda && Cx->Lambda)
				Ci.Operands.push_back(SerializeLambdaExpr(*Cx->Lambda));
			else
				Ci.Operands.push_back(std::string());
			Ci.Operands.push_back(static_cast<int64_t>(StaticSelectCols));
			Instructions.push_back(std::move(Ci));
		}
	}
	AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, Work));

	if(WhereRoot()) {
		std::vector<std::vector<RowTriple>> Branches;
		if(!BuildWhereDnf(WhereRoot(), Branches))
			FailCodegen(
			    "WHERE clause shape is not supported yet for this statement (predicate grammar extension required).");
		Instructions.push_back(MakeFilterDnf(Branches));
	}

	if(ConnectBy()) {
		const ConnectBySpec &Cb = *ConnectBy();
		Instruction Cbi;
		Cbi.Opcode_ = Opcode::CONNECT_BY_EXPAND;
		Cbi.Operands.push_back(Work);
		Cbi.Operands.push_back(Cb.ParentColumn);
		Cbi.Operands.push_back(Cb.ChildColumn);
		Cbi.Operands.push_back(static_cast<int64_t>(Cb.PriorOnParent ? 1 : 0));
		std::string StartBlob;
		if(Cb.StartWith) {
			std::vector<std::vector<RowTriple>> Branches;
			if(!BuildWhereDnf(Cb.StartWith.get(), Branches))
				FailCodegen("START WITH predicate is not supported for CONNECT BY lowering.");
			PackDnfOperandsBlob(Branches, StartBlob);
		}
		Cbi.Operands.push_back(StartBlob);
		Cbi.Operands.push_back(static_cast<int64_t>(Cb.NoCycle ? 1 : 0));
		Instructions.push_back(std::move(Cbi));
	}

	if(DistinctSelected())
		AppendInstruction(Instructions, MakeInstruction(Opcode::DEDUP_ROWS));

	if(!Grp.empty()) {
		Instruction G;
		if(OlapKind() == GroupOlapModifier::Rollup)
			G.Opcode_ = Opcode::ROLLUP;
		else if(OlapKind() == GroupOlapModifier::Cube)
			G.Opcode_ = Opcode::CUBE;
		else if(OlapKind() == GroupOlapModifier::GroupingSets)
			G.Opcode_ = Opcode::GROUPING_SETS;
		else
			G.Opcode_ = Opcode::GROUP_BY;
		const auto CountStarOutCol = [&]() -> std::string {
			return CountAggregateOutputColumn().empty() ? std::string("cnt") : CountAggregateOutputColumn();
		};
		if(ComboAggs().empty()) {
			if(AggKind() == GroupAggMode::CountDistinct) {
				const int64_t kCountDistinctTag = 4;
				G.Operands.push_back(kCountDistinctTag);
				G.Operands.push_back(static_cast<int64_t>(Grp.size()));
				for(const auto &Gc : Grp)
					G.Operands.push_back(Gc);
				G.Operands.push_back(*CountDistinctColumn());
				G.Operands.push_back(CountStarOutCol());
			} else {
				G.Operands.push_back(static_cast<int64_t>(AggCount ? 1 : 0));
				G.Operands.push_back(static_cast<int64_t>(Grp.size()));
				for(const auto &Gc : Grp)
					G.Operands.push_back(Gc);
				if(AggCount)
					G.Operands.push_back(CountStarOutCol());
			}
		} else {
			const int64_t kMultiAggTag = 3;
			G.Operands.push_back(kMultiAggTag);
			G.Operands.push_back(static_cast<int64_t>(Grp.size()));
			for(const auto &Gc : Grp)
				G.Operands.push_back(Gc);
			G.Operands.push_back(static_cast<int64_t>(AggCount ? 1LL : 0LL));
			G.Operands.push_back(static_cast<int64_t>(ComboAggs().size()));
			for(const auto &Ca : ComboAggs()) {
				G.Operands.push_back(static_cast<int64_t>(Ca.Kind));
				G.Operands.push_back(Ca.SourceColumn);
				G.Operands.push_back(Ca.OutputColumn);
			}
			if(AggCount)
				G.Operands.push_back(CountStarOutCol());
		}
		if(OlapKind() == GroupOlapModifier::GroupingSets) {
			const auto &Sets = GroupingSets();
			G.Operands.push_back(static_cast<int64_t>(Sets.size()));
			for(const auto &S : Sets) {
				G.Operands.push_back(static_cast<int64_t>(S.size()));
				for(const auto &C : S)
					G.Operands.push_back(C);
			}
		}
		Instructions.push_back(std::move(G));
	}

	if(HavingRoot()) {
		std::vector<std::vector<RowTriple>> HBranches;
		auto NormHaving = NormalizeHavingExpr(HavingRoot(), *this);
		if(!BuildWhereDnf(NormHaving.get(), HBranches))
			FailCodegen(
			    "HAVING clause shape is not supported yet for this statement (predicate grammar extension required).");
		Instructions.push_back(MakeFilterDnf(HBranches));
	}

	if(WindowSpecs().size() > Limits::MaxWindowFunctionsPerSelect)
		FailCodegen("Window function count exceeds AstralDB limit (Limits::MaxWindowFunctionsPerSelect).");
	for(const WindowSpec &Ws : WindowSpecs()) {
		if(Ws.PartitionBy.size() > Limits::MaxWindowPartitionColumns)
			FailCodegen("PARTITION BY column count exceeds AstralDB limit (Limits::MaxWindowPartitionColumns).");
		if(Ws.OrderColumn.empty() || Ws.OutputColumn.empty())
			FailCodegen("Internal: window spec missing ORDER BY or output column.");
		const int64_t KindTag = static_cast<int64_t>(Ws.Kind);
		if(KindTag < 0 || KindTag > 14)
			FailCodegen("Internal: unsupported window function kind.");
		if((Ws.Kind == WindowFnKind::Sum || Ws.Kind == WindowFnKind::Min || Ws.Kind == WindowFnKind::Max ||
		    Ws.Kind == WindowFnKind::Avg || Ws.Kind == WindowFnKind::Lag || Ws.Kind == WindowFnKind::Lead ||
		    Ws.Kind == WindowFnKind::FirstValue || Ws.Kind == WindowFnKind::LastValue ||
		    Ws.Kind == WindowFnKind::NthValue) &&
		   Ws.SourceColumn.empty())
			FailCodegen("Internal: window aggregate/shift missing source column.");
		Instruction WI;
		WI.Opcode_ = Opcode::WINDOW_ROW_NUMBER;
		WI.Operands.push_back(static_cast<int64_t>(Ws.PartitionBy.size()));
		for(const auto &K : Ws.PartitionBy)
			WI.Operands.push_back(K);
		WI.Operands.push_back(Ws.OrderColumn);
		WI.Operands.push_back(static_cast<int64_t>(Ws.OrderAscending ? 1 : 0));
		WI.Operands.push_back(Ws.OutputColumn);
		WI.Operands.push_back(KindTag);
		WI.Operands.push_back(Ws.SourceColumn);
		WI.Operands.push_back(Ws.FrameOffset);
		WI.Operands.push_back(static_cast<int64_t>(Ws.HasExplicitFrame ? (Ws.FrameUnit == WindowFrameUnit::Range ? 2 : 1) : 0));
		if(Ws.HasExplicitFrame) {
			WI.Operands.push_back(static_cast<int64_t>(Ws.FrameStart.Kind));
			WI.Operands.push_back(Ws.FrameStart.Offset);
			WI.Operands.push_back(static_cast<int64_t>(Ws.FrameEnd.Kind));
			WI.Operands.push_back(Ws.FrameEnd.Offset);
		}
		Instructions.push_back(std::move(WI));
	}

	for(size_t Pi = 0; Pi < ProjectionExprs_.size(); ++Pi) {
		const auto &Ptr = ProjectionExprs_[Pi];
		if(!Ptr)
			continue;
		if(dynamic_cast<const ColumnsExprAST *>(Ptr.get()))
			continue;
		if(const auto *Ca = dynamic_cast<const CastExprAST *>(Ptr.get()))
			EmitCastEvalInstruction(*Ca, Columns_[Pi], Instructions);
		else if(const auto *Ce = dynamic_cast<const CaseExprAST *>(Ptr.get()))
			EmitCaseEvalInstruction(*Ce, Columns_[Pi], Instructions);
		else if(const auto *Co = dynamic_cast<const CoalesceExprAST *>(Ptr.get()))
			EmitCoalesceForColumn(*Co, Columns_[Pi], Instructions);
		else if(const auto *Nv = dynamic_cast<const Nvl2ExprAST *>(Ptr.get()))
			EmitNvl2ForColumn(*Nv, Columns_[Pi], Instructions);
		else if(const auto *Sf = dynamic_cast<const ScalarFuncExprAST *>(Ptr.get()))
			EmitScalarFuncEvalInstruction(*Sf, Columns_[Pi], Instructions);
		else if(const auto *Gx = dynamic_cast<const GroupingExprAST *>(Ptr.get()))
			EmitGroupingEvalInstruction(*Gx, Columns_[Pi], Instructions);
		else if(const auto *Gid = dynamic_cast<const GroupingIdExprAST *>(Ptr.get()))
			EmitGroupingIdEvalInstruction(*Gid, Columns_[Pi], Instructions);
		else if(const auto *Bx = dynamic_cast<const BinaryOpAST *>(Ptr.get()))
			EmitBinaryArithForColumn(*Bx, Columns_[Pi], Instructions);
		else if(const auto *Cr = dynamic_cast<const ColumnRefAST *>(Ptr.get())) {
			Instruction I;
			I.Opcode_ = Opcode::CASE_EVAL;
			I.Operands.push_back(Columns_[Pi]);
			I.Operands.push_back(0LL);
			I.Operands.push_back(1LL);
			I.Operands.push_back(Cr->Name);
			Instructions.push_back(std::move(I));
		} else
			FailCodegen("Internal: unsupported SELECT projection expression.");
	}
}

void SelectAST::EmitOrderingFinalize(BytecodeScratch &Instructions) const {
	for(const auto &[Column, Ascending] : OrderBySpecs()) {
		AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, static_cast<int64_t>(Ascending ? 1 : 0)));
		AppendInstruction(Instructions, MakeInstruction(Opcode::ORDER_BY, Column));
	}

	const int64_t Lim = SelectLimitValue();
	const int64_t Off = SelectOffsetValue();
	if(Off > 0 && Lim >= 0)
		AppendInstruction(Instructions, MakeInstruction(Opcode::SLICE_RANGE, Off, Lim));
	else if(Off > 0)
		AppendInstruction(Instructions, MakeInstruction(Opcode::OFFSET, Off));
	else if(Lim >= 0)
		AppendInstruction(Instructions, MakeInstruction(Opcode::LIMIT, Lim));

	size_t StaticSelectCols = 0;
	bool HasDynamicColumns = false;
	for(const auto &Column : Columns_) {
		if(Column.rfind(kColumnsExpandPrefix, 0) == 0)
			HasDynamicColumns = true;
		else
			++StaticSelectCols;
	}
	if(HasDynamicColumns)
		AppendInstruction(Instructions, MakeInstruction(Opcode::SELECT, static_cast<int64_t>(StaticSelectCols),
		                                                static_cast<int64_t>(1)));
	else
		AppendInstruction(Instructions, MakeInstruction(Opcode::SELECT, static_cast<int64_t>(Columns_.size())));
}

void CompoundSelectAST::EmitBytecode(BytecodeScratch &Instructions) const {
	if(Arms.size() < 2 || Ops.size() + 1 != Arms.size())
		FailCodegen("Internal error: compound SELECT arm/operator mismatch.");

	static std::atomic<uint64_t> BatchSeq{1};
	const uint64_t Batch = BatchSeq.fetch_add(1, std::memory_order_relaxed);

	const std::vector<std::string> &CanonCols = Arms[0]->ProjectionColumns();
	const size_t NC = CanonCols.size();
	if(NC == 0)
		FailCodegen("UNION / INTERSECT / EXCEPT requires a non-empty SELECT list.");
	for(const auto &A : Arms) {
		if(A->ProjectionColumns().size() != NC)
			FailCodegen("All SELECT arms in UNION / INTERSECT / EXCEPT must have the same number of columns.");
		for(const auto &Cn : A->ProjectionColumns()) {
			if(Cn == "*")
				FailCodegen("SELECT * may not appear in UNION / INTERSECT / EXCEPT (name columns explicitly).");
		}
	}

	auto ArmResultTableName = [](const SelectAST &Arm, const std::string &ScratchBase, const std::string &JPfx)
	    -> std::string {
		    if(Arm.JoinSpecs().empty())
			    return ScratchBase;
		    return JPfx + std::to_string(Arm.JoinSpecs().size() - 1);
	    };

	auto StashArm = [&](size_t Idx, std::string &OutStashName) {
		const SelectAST &Arm = *Arms[Idx];
		const std::string Scratch =
		    std::string("__AstralCmp_") + std::to_string(Batch) + "_" + std::to_string(Idx) + "_src";
		const std::string JPfx =
		    std::string("__AstralCmp_") + std::to_string(Batch) + "_" + std::to_string(Idx) + "_J_";
		AppendInstruction(Instructions,
		                   MakeInstruction(Opcode::CLONE_TABLE, Scratch,
		                                 ResolveRelationLogicalName(Arm.SourceTableName(), Instructions)));
		Arm.EmitRelationPipeline(Scratch, JPfx, Instructions);
		const std::string WorkTab = ArmResultTableName(Arm, Scratch, JPfx);
		OutStashName = std::string("__AstralCmp_") + std::to_string(Batch) + "_" + std::to_string(Idx) + "_k";
		AppendInstruction(Instructions, MakeInstruction(Opcode::CLONE_TABLE, OutStashName, WorkTab));
	};

	std::string Acc;
	StashArm(0, Acc);

	for(size_t OpIx = 0; OpIx < Ops.size(); ++OpIx) {
		std::string Rstash;
		StashArm(OpIx + 1, Rstash);
		const std::string OutTab = std::string("__AstralCmp_") + std::to_string(Batch) + "_m_" + std::to_string(OpIx);

		Instruction Comb;
		Comb.Opcode_ = Opcode::SET_COMBINE;
		Comb.Operands.push_back(OutTab);
		Comb.Operands.push_back(Acc);
		Comb.Operands.push_back(Rstash);
		Comb.Operands.push_back(static_cast<int64_t>(Ops[OpIx]));
		Comb.Operands.push_back(static_cast<int64_t>(NC));
		for(size_t Ci = 0; Ci < NC; ++Ci)
			Comb.Operands.push_back(CanonCols[Ci]);

		const std::vector<std::string> &Lsrc =
		    OpIx == 0 ? Arms[0]->ProjectionColumns() : CanonCols;
		const std::vector<std::string> &Rsrc = Arms[OpIx + 1]->ProjectionColumns();

		for(size_t Ci = 0; Ci < NC; ++Ci)
			Comb.Operands.push_back(Lsrc[Ci]);
		for(size_t Ci = 0; Ci < NC; ++Ci)
			Comb.Operands.push_back(Rsrc[Ci]);
		Instructions.push_back(std::move(Comb));
		Acc = OutTab;
	}

	AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, Acc));
	for(const auto &[Column, Ascending] : OrderByColumns) {
		AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, static_cast<int64_t>(Ascending ? 1 : 0)));
		AppendInstruction(Instructions, MakeInstruction(Opcode::ORDER_BY, Column));
	}
	if(Offset > 0 && Limit >= 0)
		AppendInstruction(Instructions, MakeInstruction(Opcode::SLICE_RANGE, Offset, Limit));
	else if(Offset > 0)
		AppendInstruction(Instructions, MakeInstruction(Opcode::OFFSET, Offset));
	else if(Limit >= 0)
		AppendInstruction(Instructions, MakeInstruction(Opcode::LIMIT, Limit));
	AppendInstruction(Instructions, MakeInstruction(Opcode::SELECT, static_cast<int64_t>(NC)));
}

void WithSelectAST::EmitBytecode(BytecodeScratch& Instructions) const {
	for(const auto &Ct : Clauses) {
		const std::string &Work = Ct.PhysicalTable;
		AppendInstruction(Instructions,
		                   MakeInstruction(Opcode::CLONE_TABLE, Work,
		                                 ResolveRelationLogicalName(Ct.Anchor->SourceTableName(), Instructions)));
		Ct.Anchor->EmitBytecodeForScratchTable(Work, Instructions);
		if(!Ct.IsRecursive())
			continue;

		const std::string Delta = Work + "_delta";
		const size_t LoopStart = Instructions.size();
		Ct.RecursiveStep->EmitBytecodeForScratchTable(Delta, Instructions);
		AppendInstruction(Instructions,
		                   MakeInstruction(Opcode::RECURSIVE_CTE_FIXPOINT, Work, Delta,
		                                    static_cast<int64_t>(Limits::MaxCteRecursionDepth),
		                                    static_cast<int64_t>(LoopStart)));
	}
	if(Main)
		Main->EmitBytecode(Instructions);
}

void InsertAST::EmitBytecode(BytecodeScratch& Instructions) const {
	const bool DoReturning = HasReturning;
	const int64_t RetNRet = ReturningAll ? static_cast<int64_t>(-1) : static_cast<int64_t>(ReturningColumns.size());
	const std::string RetTable = "__astral_returning";
	std::size_t RowIx = 0;
	for(const auto &RowValue : Values) {
		const int64_t RetReset = DoReturning && RowIx == 0 ? 1LL : 0LL;
		if(!Columns.empty() && Columns.size() != RowValue.size())
			FailCodegen("INSERT column count does not match VALUES count.");
		const size_t K = RowValue.size();
		AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, Table->TableName));
		for(const auto &Column : Columns)
			AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, Column));
		for(const auto &Val : RowValue) {
			const auto Cell = TryConstEvalInsertExpr(Val.get());
			if(!Cell)
				FailCodegen(
				    "INSERT VALUES must be literals, NULL, NEXTVAL(...), or constant scalar expressions "
				    "(e.g. ST_MESH('V[…]', 'L[…]')) in this dialect.");
			AppendInstruction(Instructions, MakeInstruction(Opcode::PUSH, *Cell));
		}
		const int64_t Explicit = Columns.empty() ? 0LL : 1LL;
		if(Upsert_) {
			const UpsertSpec &U = *Upsert_;
			const int64_t DoNothing = U.Mode == UpsertSpec::OnConflict::Nothing ? 1LL : 0LL;
			std::vector<UpsertAssign> ReplaceBuilt;
			const std::vector<UpsertAssign> *Assignments = &U.UpdateAssignments;
			if(U.SqliteReplace && U.UpdateAssignments.empty() && !Columns.empty()) {
				for(const auto &C : Columns) {
					UpsertAssign Asg;
					Asg.Column = C;
					Asg.Value = std::make_unique<QualifiedRefAST>(QualifiedRefAST::Role::Excluded, C);
					ReplaceBuilt.push_back(std::move(Asg));
				}
				Assignments = &ReplaceBuilt;
			}
			std::vector<Value> Ops;
			Ops.reserve(7 + U.ConflictColumns.size() + Assignments->size() * 2 + 4
			            + (DoReturning ? (3 + (RetNRet >= 0 ? static_cast<size_t>(RetNRet) : 0ULL)) : 0ULL));
			Ops.push_back(static_cast<int64_t>(K));
			Ops.push_back(Explicit);
			Ops.push_back(DoNothing);
			Ops.push_back(static_cast<int64_t>(U.ConflictColumns.size()));
			for(const auto &C : U.ConflictColumns)
				Ops.push_back(C);
			Ops.push_back(U.SqliteReplace ? (U.SqliteReplaceImplicitSchema ? static_cast<int64_t>(2) :
			                                                               static_cast<int64_t>(1)) :
			                              static_cast<int64_t>(0));
			Ops.push_back(static_cast<int64_t>(Assignments->size()));
			for(const auto &Asg : *Assignments) {
				Ops.push_back(Asg.Column);
				Ops.push_back(SerializeSetValueExpr(Asg.Value.get()));
			}
			if(DoReturning) {
				Ops.push_back(RetTable);
				Ops.push_back(RetReset);
				Ops.push_back(RetNRet);
				if(RetNRet >= 0) {
					for(const std::string &C : ReturningColumns)
						Ops.push_back(C);
				}
			}
			Instruction Up;
			Up.Opcode_ = Opcode::UPSERT;
			Up.Operands = std::move(Ops);
			AppendInstruction(Instructions, Up);
		} else {
			if(!DoReturning) {
				AppendInstruction(Instructions, MakeInstruction(Opcode::INSERT, static_cast<int64_t>(K), Explicit));
			} else {
				Instruction Ins;
				Ins.Opcode_ = Opcode::INSERT;
				Ins.Operands.push_back(static_cast<int64_t>(K));
				Ins.Operands.push_back(Explicit);
				Ins.Operands.push_back(RetTable);
				Ins.Operands.push_back(RetReset);
				Ins.Operands.push_back(RetNRet);
				if(RetNRet >= 0) {
					for(const std::string &C : ReturningColumns)
						Ins.Operands.push_back(C);
				}
				AppendInstruction(Instructions, std::move(Ins));
			}
		}
		++RowIx;
	}
}

void MergeAST::EmitBytecode(BytecodeScratch &Instructions) const {
	if(OnKeyPairs.empty())
		FailCodegen("MERGE requires at least one ON key pair.");
	std::vector<Value> Ops;
	Ops.reserve(6 + OnKeyPairs.size() * 2 + Matched.size() * 2 + NotMatched.size() * 2);
	Ops.push_back(TargetTable);
	Ops.push_back(SourceTable);
	Ops.push_back(static_cast<int64_t>(OnKeyPairs.size()));
	for(const auto &[Tc, Sc] : OnKeyPairs) {
		Ops.push_back(Tc);
		Ops.push_back(Sc);
	}
	Ops.push_back(static_cast<int64_t>(Matched.size()));
	for(const auto &M : Matched) {
		Ops.push_back(M.TargetColumn);
		Ops.push_back(SerializeSetValueExpr(M.Value.get()));
	}
	Ops.push_back(static_cast<int64_t>(NotMatched.size()));
	for(const auto &N : NotMatched) {
		Ops.push_back(N.Column);
		Ops.push_back(SerializeSetValueExpr(N.Value.get()));
	}
	Instruction Mg;
	Mg.Opcode_ = Opcode::MERGE_INTO;
	Mg.Operands = std::move(Ops);
	AppendInstruction(Instructions, Mg);
}

void BulkInsertAST::EmitBytecode(BytecodeScratch& Instructions) const {
	if(Count <= 0)
		FailCodegen("BULK INSERT row count must be a positive integer.");
	if(static_cast<std::uint64_t>(Count) > Limits::MaxBulkInsertRowsBench)
		FailCodegen("BULK INSERT row count exceeds the configured maximum (see server limits).");
	(void)Columns;
	AppendInstruction(Instructions,
	                  MakeInstruction(Opcode::INSERT_BULK, TableName, Count, StartId, Step));
}

void UpdateAST::EmitBytecode(BytecodeScratch& Instructions) const {
	std::vector<std::vector<RowTriple>> Branches;
	if(Condition) {
		if(!BuildWhereDnf(Condition.get(), Branches))
			FailCodegen("Unsupported WHERE on UPDATE (extend SQLite-lite predicate grammar).");
	} else
		Branches.push_back({}); /* tautology: UPDATE all rows (consistent with DELETE without WHERE). */
	Instruction I = MakeUpdateDnf(TableName, Assignments, Branches);
	if(HasReturning) {
		const int64_t RetNRet = ReturningAll ? static_cast<int64_t>(-1) : static_cast<int64_t>(ReturningColumns.size());
		I.Operands.push_back(std::string("__astral_returning"));
		I.Operands.push_back(RetNRet);
		if(RetNRet >= 0) {
			for(const std::string &C : ReturningColumns)
				I.Operands.push_back(C);
		}
	}
	Instructions.push_back(std::move(I));
}

void DeleteAST::EmitBytecode(BytecodeScratch& Instructions) const {
	if(Condition) {
		std::vector<std::vector<RowTriple>> Branches;
		if(!BuildWhereDnf(Condition.get(), Branches))
			FailCodegen("Unsupported WHERE on DELETE.");
		Instruction I = MakeDeleteDnf(TableName, Branches);
		if(HasReturning) {
			const int64_t RetNRet = ReturningAll ? static_cast<int64_t>(-1) : static_cast<int64_t>(ReturningColumns.size());
			I.Operands.push_back(std::string("__astral_returning"));
			I.Operands.push_back(RetNRet);
			if(RetNRet >= 0) {
				for(const std::string &C : ReturningColumns)
					I.Operands.push_back(C);
			}
		}
		Instructions.push_back(std::move(I));
		return;
	}
	if(HasReturning) {
		std::vector<std::vector<RowTriple>> Branches;
		Branches.push_back({}); /* tautology: DELETE all rows. */
		Instruction I = MakeDeleteDnf(TableName, Branches);
		const int64_t RetNRet = ReturningAll ? static_cast<int64_t>(-1) : static_cast<int64_t>(ReturningColumns.size());
		I.Operands.push_back(std::string("__astral_returning"));
		I.Operands.push_back(RetNRet);
		if(RetNRet >= 0) {
			for(const std::string &C : ReturningColumns)
				I.Operands.push_back(C);
		}
		Instructions.push_back(std::move(I));
		return;
	}
	AppendInstruction(Instructions, MakeInstruction(Opcode::DELETE, TableName));
}

void BinaryOpAST::EmitBytecode(BytecodeScratch& Instructions) const {
	LHS->EmitBytecode(Instructions);
	RHS->EmitBytecode(Instructions);
	Opcode SqlOp;
	if(Op == "+")
		SqlOp = Opcode::ADD;
	else if(Op == "-")
		SqlOp = Opcode::SUB;
	else if(Op == "*")
		SqlOp = Opcode::MUL;
	else if(Op == "/")
		SqlOp = Opcode::DIV;
	else if(Op == "//")
		SqlOp = Opcode::INT_DIV;
	else if(Op == "%")
		SqlOp = Opcode::MOD;
	else if(Op == "==" || Op == "=")
		SqlOp = Opcode::EQ;
	else if(Op == "!=")
		SqlOp = Opcode::NE;
	else if(Op == "<")
		SqlOp = Opcode::LT;
	else if(Op == "<=")
		SqlOp = Opcode::LE;
	else if(Op == ">")
		SqlOp = Opcode::GT;
	else if(Op == ">=")
		SqlOp = Opcode::GE;
	else
		FailCodegen("Unsupported binary operator: " + Op);
	AppendInstruction(Instructions, MakeInstruction(SqlOp));
}

void FuncCallExprAST::EmitBytecode(BytecodeScratch &) const {
	FailCodegen("Internal: aggregate call in HAVING should be lowered before bytecode emission.");
}

void GrantAST::EmitBytecode(BytecodeScratch& Instructions) const {
	if(!Columns.empty()) {
		for(const std::string &Col : Columns) {
			AppendInstruction(Instructions,
			                  MakeInstruction(Opcode::GRANT_COLUMN, Grantee, static_cast<int64_t>(Perms), TableName,
			                                  Col, static_cast<int64_t>(GranteeIsRole ? 1 : 0)));
		}
		return;
	}
	AppendInstruction(Instructions,
	                  MakeInstruction(Opcode::GRANT, Grantee, static_cast<int64_t>(Perms), TableName,
	                                  static_cast<int64_t>(GranteeIsRole ? 1 : 0)));
}

void RevokeAST::EmitBytecode(BytecodeScratch& Instructions) const {
	if(!Columns.empty()) {
		for(const std::string &Col : Columns) {
			AppendInstruction(Instructions,
			                  MakeInstruction(Opcode::REVOKE_COLUMN, Grantee, static_cast<int64_t>(Perms), TableName,
			                                  Col, static_cast<int64_t>(GranteeIsRole ? 1 : 0)));
		}
		return;
	}
	AppendInstruction(Instructions,
	                  MakeInstruction(Opcode::REVOKE, Grantee, static_cast<int64_t>(Perms), TableName,
	                                  static_cast<int64_t>(GranteeIsRole ? 1 : 0)));
}

void GrantRoleMembershipAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions, MakeInstruction(Opcode::GRANT_ROLE_MEMBERSHIP, RoleName, UserName));
}

void RevokeRoleMembershipAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions, MakeInstruction(Opcode::REVOKE_ROLE_MEMBERSHIP, RoleName, UserName));
}

void CreateRoleAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions, MakeInstruction(Opcode::CREATE_ROLE, RoleName));
}

void DropRoleAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions, MakeInstruction(Opcode::DROP_ROLE, RoleName));
}

void CreateUserAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions,
	                  MakeInstruction(Opcode::CREATE_USER, UserName, Password,
	                                  static_cast<int64_t>(IfNotExists ? 1 : 0)));
}

void DropUserAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions,
	                  MakeInstruction(Opcode::DROP_USER, UserName, static_cast<int64_t>(IfExists ? 1 : 0)));
}

void AlterUserPasswordAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions, MakeInstruction(Opcode::ALTER_USER_PASSWORD, UserName, Password));
}

void CreateSequenceAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions,
	                  MakeInstruction(Opcode::CREATE_SEQUENCE, SequenceName, Start, Increment,
	                                  static_cast<int64_t>(IfNotExists ? 1 : 0)));
}

void DropSequenceAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions,
	                  MakeInstruction(Opcode::DROP_SEQUENCE, SequenceName, static_cast<int64_t>(IfExists ? 1 : 0)));
}

void CreateDatasetAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions,
	                  MakeInstruction(Opcode::REGISTER_DATASET, DatasetName,
	                                  static_cast<int64_t>(Kind), SourceTable, BulkCount, BulkStart, BulkStep));
}

void LoadDatasetAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions,
	                  MakeInstruction(Opcode::LOAD_DATASET, DatasetName, TargetTable, VersionId));
}

void VacuumAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions, MakeInstruction(Opcode::VACUUM, TableName));
}

void RepackConcurrentlyAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions, MakeInstruction(Opcode::REPACK_CONCURRENTLY, TableName));
}

void DropDatasetAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions, MakeInstruction(Opcode::DROP_DATASET, DatasetName));
}

void CreateEmbeddingAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions, MakeInstruction(Opcode::REGISTER_EMBEDDING, EmbeddingName, SourceTable, TokenColumn,
	                                              VectorColumn));
}

void DropEmbeddingAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions, MakeInstruction(Opcode::DROP_EMBEDDING, EmbeddingName));
}

void CreateGraphAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions,
	                  MakeInstruction(Opcode::GRAPH_REGISTER, GraphName, VertexTable, VertexIdCol, EdgeTable,
	                                  EdgeSrcCol, EdgeDstCol, EdgeLabelCol, EdgeWeightCol,
	                                  static_cast<int64_t>(Undirected ? 1 : 0)));
}

void CreateGraphProjectionAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions, MakeInstruction(Opcode::GRAPH_REGISTER_PROJECTION, ProjectionName,
	                                                BaseGraphName, EdgeLabelFilter));
}

void DropGraphAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions, MakeInstruction(Opcode::GRAPH_DROP, GraphName));
}

void GraphTraverseAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions,
	                  MakeInstruction(Opcode::GRAPH_TRAVERSE, GraphName, StartVertexId, MaxDepth, Mode,
	                                  ResultTable));
}

void GraphMatchAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions,
	                  MakeInstruction(Opcode::GRAPH_MATCH, GraphName, EdgeLabelFilter, ResultTable, MinHops, MaxHops,
	                                  AnchorVertexId, static_cast<int64_t>(Reverse ? 1 : 0)));
}

void GraphShortestPathAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions,
	                  MakeInstruction(Opcode::GRAPH_SHORTEST_PATH, GraphName, FromVertexId, ToVertexId,
	                                  static_cast<int64_t>(Weighted ? 1 : 0), ResultTable));
}

void GraphPageRankAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions,
	                  MakeInstruction(Opcode::GRAPH_PAGERANK, GraphName, DampingMillis, Iterations, ResultTable));
}

void CreateIndexAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions,
	                  MakeInstruction(Opcode::CREATE_INDEX, IndexName, TableName, ColumnName,
	                                  static_cast<int64_t>(Kind), VectorMetricTag));
}

void DropIndexAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions,
	                  MakeInstruction(Opcode::DROP_INDEX, IndexName, static_cast<int64_t>(IfExists ? 1 : 0)));
}

void CreateViewAST::EmitBytecode(BytecodeScratch &Instructions) const {
	if(!dynamic_cast<const SelectAST *>(Definition_.get()))
		FailCodegen("CREATE VIEW expects a plain SELECT definition.");
	AppendInstruction(Instructions, MakeInstruction(Opcode::CREATE_VIEW, ViewName, BodySql_));
}

void DropViewAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions,
	                  MakeInstruction(Opcode::DROP_VIEW, ViewName, static_cast<int64_t>(IfExists ? 1 : 0)));
}

void CreateProcedureAST::EmitBytecode(BytecodeScratch &Instructions) const {
	if(ExceptionHandlers_.empty())
		AppendInstruction(Instructions,
		                  MakeInstruction(Opcode::CREATE_PROCEDURE, ProcedureName, BodySql_,
		                                  static_cast<int64_t>(IfNotExists ? 1 : 0),
		                                  static_cast<int64_t>(OrReplace ? 1 : 0), SourceDialect_));
	else
		AppendInstruction(Instructions,
		                  MakeInstruction(Opcode::CREATE_PROCEDURE, ProcedureName, BodySql_,
		                                  static_cast<int64_t>(IfNotExists ? 1 : 0),
		                                  static_cast<int64_t>(OrReplace ? 1 : 0), SourceDialect_,
		                                  EncodeExceptionHandlersJson(ExceptionHandlers_)));
}

void DropProcedureAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions,
	                  MakeInstruction(Opcode::DROP_PROCEDURE, ProcedureName,
	                                  static_cast<int64_t>(IfExists ? 1 : 0)));
}

void CallProcedureAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions, MakeInstruction(Opcode::CALL_PROCEDURE, ProcedureName, InvokeKind));
}

void CreateTriggerAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions,
	                  MakeInstruction(Opcode::CREATE_TRIGGER, TriggerName, TableName,
	                                  static_cast<int64_t>(static_cast<int>(Timing)),
	                                  static_cast<int64_t>(static_cast<int>(Event)),
	                                  static_cast<int64_t>(ForEachRow ? 1 : 0), ActionKind, ProcedureName,
	                                  BodySql_, static_cast<int64_t>(IfNotExists ? 1 : 0),
	                                  static_cast<int64_t>(OrReplace ? 1 : 0)));
}

void DropTriggerAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions,
	                  MakeInstruction(Opcode::DROP_TRIGGER, TriggerName, static_cast<int64_t>(IfExists ? 1 : 0)));
}

void AlterTriggerAST::EmitBytecode(BytecodeScratch &Instructions) const {
	AppendInstruction(Instructions,
	                  MakeInstruction(Opcode::ALTER_TRIGGER, TriggerName, static_cast<int64_t>(Enable ? 1 : 0)));
}

Bytecode BuildBytecode(Logger *Logger, OptimizationLevel OptLevel, const AstralDB::Database *ViewCatalogDb) {
	Bytecode Result;
	std::unordered_map<std::string, std::string> LiveBodies;
	if(ViewCatalogDb)
		LiveBodies = ViewCatalogDb->ViewDefinitionsSnapshot();
	std::unordered_map<std::string, const StatementAST *> SessionParsedBodies;
	SqlViewCodegenState ViewCodegen;
	ViewCodegen.LiveBodies = &LiveBodies;
	ViewCodegen.SessionParsedBodies = &SessionParsedBodies;
	ViewCodegen.ExpansionDepth = 0;
	ViewCodegen.ScratchCounter = 0;
	ViewCodegen.WalReparsedBodies.clear();
	ScopedViewCodegen ViewGuard(&ViewCodegen);
	AstralDB::ArenaAllocator Arena(Limits::SqlBytecodeArenaBytes);

	for(auto &Statement : AST) {
		if(Statement && Statement->Value) {
			if(Logger)
				Logger->Info("Emitting bytecode for AST node");
			Arena.reset();
			std::pmr::monotonic_buffer_resource Pool(
			    Arena.data(), Arena.capacity(), std::pmr::new_delete_resource());
			BytecodeScratchAlloc Alloc(&Pool);
			BytecodeScratch Instructions(Alloc);
			if(auto *Cv = dynamic_cast<CreateViewAST *>(Statement->Value.get())) {
				Cv->EmitBytecode(Instructions);
				LiveBodies[Cv->ViewName] = Cv->BodySql_;
				SessionParsedBodies[Cv->ViewName] = Cv->Definition_.get();
			} else if(auto *Dv = dynamic_cast<DropViewAST *>(Statement->Value.get())) {
				Dv->EmitBytecode(Instructions);
				LiveBodies.erase(Dv->ViewName);
				SessionParsedBodies.erase(Dv->ViewName);
				ViewCodegen.WalReparsedBodies.erase(Dv->ViewName);
			} else
				Statement->Value->EmitBytecode(Instructions);
			const size_t BaseIp = Result.size();
			for(auto &Inst : Instructions) {
				if(Inst.Opcode_ == Opcode::RECURSIVE_CTE_FIXPOINT && Inst.Operands.size() >= 4) {
					if(auto *LoopStart = std::get_if<int64_t>(&Inst.Operands[3]))
						*LoopStart += static_cast<int64_t>(BaseIp);
				}
				Result.push_back(std::move(Inst));
			}
		}
	}

	RunGraphOptimizerPipeline(Result, OptLevel, Logger);
	RunMathSciOptimizerPipeline(Result, OptLevel, Logger);
	RunOptimizerPipeline(Result, OptLevel, Logger);
	RunGraphOptimizerPipeline(Result, OptLevel, Logger);

	if(Logger)
		Logger->Info("Bytecode build complete with " + std::to_string(Result.size()) + " instructions");
	return Result;
}

void CompileCheckSqlToDnfPackedOrThrow(std::string_view CheckBodySql, std::string &OutPackedBlob) {
	CompileCheckSqlToDnfPackedOrThrowImpl(CheckBodySql, OutPackedBlob);
}

void DedupBytecodeStringImmediates(Bytecode &Code, std::vector<std::string> &PoolOut) {
	PoolOut.clear();
	std::unordered_map<std::string, size_t> Pos;
	auto Intern = [&](const std::string &S) -> int64_t {
		auto It = Pos.find(S);
		if(It != Pos.end())
			return static_cast<int64_t>(It->second);
		const size_t Id = PoolOut.size();
		PoolOut.push_back(S);
		Pos.emplace(S, Id);
		return static_cast<int64_t>(Id);
	};

	for(auto &Inst : Code) {
		if(Inst.Opcode_ == Opcode::PUSH && Inst.Operands.size() == 1) {
			if(auto *Sv = std::get_if<std::string>(&Inst.Operands[0])) {
				const int64_t Idx = Intern(*Sv);
				Inst.Opcode_ = Opcode::PUSH_POOL;
				Inst.Operands[0] = Idx;
				continue;
			}
		}
	}
}

CompiledBytecode BuildCompiledBytecode(Logger *Logger, OptimizationLevel OptLevel,
                                       const AstralDB::Database *ViewCatalogDb) {
	CompiledBytecode Out;
	Out.Instructions = BuildBytecode(Logger, OptLevel, ViewCatalogDb);
	DedupBytecodeStringImmediates(Out.Instructions, Out.StringPool);
	return Out;
}

} // namespace AstralDB::SQL
