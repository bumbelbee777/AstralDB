#include <SQL/SetExprEval.hxx>
#include <IO/Error.hxx>
#include <cctype>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace AstralDB::SQL {
namespace {

[[noreturn]] void FailSetExpr(std::string Msg) {
	throw std::runtime_error(AstralDB::Err::Prefixed("SQL set expression", std::move(Msg)));
}

std::string CellOrEmpty(const Database::Item *Row, const std::string &Col) {
	if(!Row)
		return {};
	auto It = Row->find(Col);
	return It == Row->end() ? std::string() : It->second;
}

bool TryParseDouble(const std::string &S, double &Out) {
	try {
		size_t Pos = 0;
		Out = std::stod(S, &Pos);
		return Pos == S.size() || std::isspace(static_cast<unsigned char>(S[Pos]));
	} catch(...) {
		return false;
	}
}

std::optional<std::string> EvalLeaf(int Kind, std::string_view Payload, const RowEvalContext &Ctx) {
	switch(Kind) {
	case 0:
		return std::string(Payload);
	case 1:
		return CellOrEmpty(Ctx.Target, std::string(Payload));
	case 2:
		return std::string();
	case 3:
		return CellOrEmpty(Ctx.Excluded, std::string(Payload));
	case 4:
		return CellOrEmpty(Ctx.Source, std::string(Payload));
	default:
		return std::nullopt;
	}
}

std::optional<std::string> EvalNode(std::string_view &In, const RowEvalContext &Ctx);

std::optional<std::string> EvalNode(std::string_view &In, const RowEvalContext &Ctx) {
	if(In.empty())
		return std::nullopt;
	const char Tag = In.front();
	In.remove_prefix(1);
	if(Tag == 'L') {
		const auto Bar = In.find('|');
		if(Bar == std::string_view::npos)
			return std::nullopt;
		const std::string Lit(In.substr(0, Bar));
		In.remove_prefix(Bar + 1);
		return Lit;
	}
	if(Tag == 'N') {
		if(!In.empty() && In.front() == '|')
			In.remove_prefix(1);
		return std::string();
	}
	if(Tag == 'T' || Tag == 'E' || Tag == 'S') {
		const auto Bar = In.find('|');
		if(Bar == std::string_view::npos)
			return std::nullopt;
		const int Kind = Tag == 'T' ? 1 : (Tag == 'E' ? 3 : 4);
		auto V = EvalLeaf(Kind, In.substr(0, Bar), Ctx);
		In.remove_prefix(Bar + 1);
		return V;
	}
	if(Tag == 'B') {
		const auto Bar = In.find('|');
		if(Bar == std::string_view::npos)
			return std::nullopt;
		const std::string Op(In.substr(0, Bar));
		In.remove_prefix(Bar + 1);
		auto L = EvalNode(In, Ctx);
		auto R = EvalNode(In, Ctx);
		if(!L || !R)
			return std::nullopt;
		double A = 0, B = 0;
		const bool Na = TryParseDouble(*L, A);
		const bool Nb = TryParseDouble(*R, B);
		if(Op == "+") {
			if(Na && Nb)
				return std::to_string(static_cast<long long>(std::llround(A + B)));
			return *L + *R;
		}
		if(Op == "-" && Na && Nb)
			return std::to_string(static_cast<long long>(std::llround(A - B)));
		if(Op == "*" && Na && Nb)
			return std::to_string(static_cast<long long>(std::llround(A * B)));
		if(Op == "/" && Na && Nb && B != 0)
			return std::to_string(static_cast<long long>(std::llround(A / B)));
		if(Op == "//" && Na && Nb && B != 0) {
			const long long Ai = static_cast<long long>(std::llround(A));
			const long long Bi = static_cast<long long>(std::llround(B));
			return std::to_string(Ai / Bi);
		}
		return std::nullopt;
	}
	return std::nullopt;
}

void SerializeNode(const ExpressionAST *Expr, std::ostringstream &Out);

void SerializeNode(const ExpressionAST *Expr, std::ostringstream &Out) {
	if(!Expr)
		FailSetExpr("missing expression");
	if(const auto *L = dynamic_cast<const LiteralAST *>(Expr)) {
		Out << 'L' << L->Value << '|';
		return;
	}
	if(dynamic_cast<const NullLiteralAST *>(Expr)) {
		Out << 'N' << '|';
		return;
	}
	if(const auto *C = dynamic_cast<const ColumnRefAST *>(Expr)) {
		Out << 'T' << C->Name << '|';
		return;
	}
	if(const auto *Q = dynamic_cast<const QualifiedRefAST *>(Expr)) {
		switch(Q->RefRole) {
		case QualifiedRefAST::Role::Target:
			Out << 'T' << Q->Column << '|';
			break;
		case QualifiedRefAST::Role::Excluded:
			Out << 'E' << Q->Column << '|';
			break;
		case QualifiedRefAST::Role::Source:
			Out << 'S' << Q->Column << '|';
			break;
		}
		return;
	}
	if(const auto *B = dynamic_cast<const BinaryOpAST *>(Expr)) {
		if(B->Op != "+" && B->Op != "-" && B->Op != "*" && B->Op != "/" && B->Op != "//")
			FailSetExpr("SET expression supports only + - * / //");
		Out << 'B' << B->Op << '|';
		SerializeNode(B->LHS.get(), Out);
		SerializeNode(B->RHS.get(), Out);
		return;
	}
	FailSetExpr("unsupported expression in SET assignment");
}

} // namespace

std::optional<std::string> EvalSetValueExpr(const ExpressionAST *Expr, const RowEvalContext &Ctx) {
	if(!Expr)
		return std::nullopt;
	if(const auto *L = dynamic_cast<const LiteralAST *>(Expr))
		return L->Value;
	if(dynamic_cast<const NullLiteralAST *>(Expr))
		return std::string();
	if(const auto *C = dynamic_cast<const ColumnRefAST *>(Expr))
		return CellOrEmpty(Ctx.Target, C->Name);
	if(const auto *Q = dynamic_cast<const QualifiedRefAST *>(Expr)) {
		switch(Q->RefRole) {
		case QualifiedRefAST::Role::Target:
			return CellOrEmpty(Ctx.Target, Q->Column);
		case QualifiedRefAST::Role::Excluded:
			return CellOrEmpty(Ctx.Excluded, Q->Column);
		case QualifiedRefAST::Role::Source:
			return CellOrEmpty(Ctx.Source, Q->Column);
		}
	}
	if(const auto *B = dynamic_cast<const BinaryOpAST *>(Expr)) {
		auto L = EvalSetValueExpr(B->LHS.get(), Ctx);
		auto R = EvalSetValueExpr(B->RHS.get(), Ctx);
		if(!L || !R)
			return std::nullopt;
		double A = 0, Bv = 0;
		if(TryParseDouble(*L, A) && TryParseDouble(*R, Bv)) {
			if(B->Op == "+")
				return std::to_string(static_cast<long long>(std::llround(A + Bv)));
			if(B->Op == "-")
				return std::to_string(static_cast<long long>(std::llround(A - Bv)));
			if(B->Op == "*")
				return std::to_string(static_cast<long long>(std::llround(A * Bv)));
			if(B->Op == "/" && Bv != 0)
				return std::to_string(static_cast<long long>(std::llround(A / Bv)));
			if(B->Op == "//" && Bv != 0) {
				const long long Ai = static_cast<long long>(std::llround(A));
				const long long Bi = static_cast<long long>(std::llround(Bv));
				return std::to_string(Ai / Bi);
			}
		}
		if(B->Op == "+")
			return *L + *R;
		return std::nullopt;
	}
	return std::nullopt;
}

std::unique_ptr<ExpressionAST> BindLambdaParameter(const ExpressionAST *Root, std::string_view Param,
                                                   std::string_view BindingCol) {
	if(!Root)
		return nullptr;
	if(const auto *C = dynamic_cast<const ColumnRefAST *>(Root)) {
		if(C->Name == Param)
			return std::make_unique<ColumnRefAST>(std::string(BindingCol));
		return std::make_unique<ColumnRefAST>(C->Name);
	}
	if(const auto *L = dynamic_cast<const LiteralAST *>(Root))
		return std::make_unique<LiteralAST>(L->Value);
	if(dynamic_cast<const NullLiteralAST *>(Root))
		return std::make_unique<NullLiteralAST>();
	if(const auto *B = dynamic_cast<const BinaryOpAST *>(Root)) {
		auto L = BindLambdaParameter(B->LHS.get(), Param, BindingCol);
		auto R = BindLambdaParameter(B->RHS.get(), Param, BindingCol);
		return std::make_unique<BinaryOpAST>(std::move(L), B->Op, std::move(R));
	}
	return nullptr;
}

bool EvalExpressionTruthy(const ExpressionAST *Expr, const RowEvalContext &Ctx) {
	const auto V = EvalSetValueExpr(Expr, Ctx);
	if(!V || V->empty())
		return false;
	if(*V == "0" || *V == "false" || *V == "FALSE")
		return false;
	return true;
}

std::string SerializeSetValueExpr(const ExpressionAST *Expr) {
	std::ostringstream Out;
	SerializeNode(Expr, Out);
	return Out.str();
}

std::string SerializeLambdaExpr(const LambdaExprAST &Lambda) {
	std::ostringstream Out;
	Out << 'M' << Lambda.Params.size() << '|';
	for(const auto &P : Lambda.Params)
		Out << P << '|';
	Out << SerializeSetValueExpr(Lambda.Body.get());
	return Out.str();
}

std::optional<std::string> EvalSerializedSetValueExpr(std::string_view Blob, const RowEvalContext &Ctx) {
	std::string_view View = Blob;
	return EvalNode(View, Ctx);
}

} // namespace AstralDB::SQL
