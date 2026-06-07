#include <SQL/Fusion/SemistructuredFusionPass.hxx>

namespace AstralDB {
namespace SQL {

namespace {

bool ColumnNameFromExpr(const ExpressionAST *Expr, std::string &Out) {
	if(const auto *C = dynamic_cast<const ColumnRefAST *>(Expr)) {
		Out = C->Name;
		return true;
	}
	return false;
}

bool LiteralFromExpr(const ExpressionAST *Expr, std::string &Out) {
	if(const auto *L = dynamic_cast<const LiteralAST *>(Expr)) {
		Out = L->Value;
		return true;
	}
	if(const auto *B = dynamic_cast<const BooleanLiteralAST *>(Expr)) {
		Out = B->Value ? "1" : "0";
		return true;
	}
	return false;
}

void FlattenAnd(const ExpressionAST *Root, std::vector<const ExpressionAST *> &Out) {
	if(!Root)
		return;
	if(const auto *Bin = dynamic_cast<const BinaryOpAST *>(Root)) {
		if(Bin->Op == "AND") {
			FlattenAnd(Bin->LHS.get(), Out);
			FlattenAnd(Bin->RHS.get(), Out);
			return;
		}
	}
	Out.push_back(Root);
}

bool TripleFromAtom(const ExpressionAST *Atom, FilterTriple &Out) {
	if(!Atom)
		return false;
	if(const auto *Bin = dynamic_cast<const BinaryOpAST *>(Atom)) {
		if(Bin->Op == "MATCH" || Bin->Op == "NOT MATCH") {
			std::string Col, Query;
			if(!ColumnNameFromExpr(Bin->LHS.get(), Col) || !LiteralFromExpr(Bin->RHS.get(), Query))
				return false;
			Out = {Col, Bin->Op, Query};
			return true;
		}
		const auto *Sf = dynamic_cast<const ScalarFuncExprAST *>(Bin->LHS.get());
		std::string Rhs;
		if(Sf && LiteralFromExpr(Bin->RHS.get(), Rhs)) {
			std::string Col;
			if(Sf->Fn == ScalarSqlFn::JsonExtract && Sf->Args.size() == 2 && ColumnNameFromExpr(Sf->Args[0].get(), Col)) {
				std::string Path;
				if(!LiteralFromExpr(Sf->Args[1].get(), Path))
					return false;
				Out = {Col, "JSON_EXTRACT", Path + std::string("\x1E") + Rhs};
				return true;
			}
			if(Sf->Fn == ScalarSqlFn::XmlValid && Sf->Args.size() == 1 && ColumnNameFromExpr(Sf->Args[0].get(), Col)) {
				Out = {Col, "XML_VALID", Rhs};
				return true;
			}
		}
		std::string Col, Rhs2;
		if(ColumnNameFromExpr(Bin->LHS.get(), Col) && LiteralFromExpr(Bin->RHS.get(), Rhs2)) {
			if(Bin->Op == "=" || Bin->Op == "==" || Bin->Op == "!=" || Bin->Op == "<>" || Bin->Op == ">" ||
			   Bin->Op == ">=" || Bin->Op == "<" || Bin->Op == "<=") {
				Out = {Col, Bin->Op, Rhs2};
				return true;
			}
		}
	}
	return false;
}

bool ExtractWhereFilters(const ExpressionAST *WhereRoot, std::vector<FilterTriple> &Out) {
	std::vector<const ExpressionAST *> Atoms;
	FlattenAnd(WhereRoot, Atoms);
	Out.clear();
	for(const ExpressionAST *Atom : Atoms) {
		FilterTriple T;
		if(!TripleFromAtom(Atom, T))
			return false;
		Out.push_back(std::move(T));
	}
	return !Out.empty();
}

bool IsSupportedScalarFn(const ScalarSqlFn Fn) {
	switch(Fn) {
	case ScalarSqlFn::JsonExtract:
	case ScalarSqlFn::XmlValid:
	case ScalarSqlFn::XmlExtract:
	case ScalarSqlFn::RegexpExtract:
	case ScalarSqlFn::CharLength:
	case ScalarSqlFn::TextRank:
	case ScalarSqlFn::TextMatch:
		return true;
	default:
		return false;
	}
}

bool ExtractProjection(const ExpressionAST *Expr, const std::string &OutCol, FusedProjectionSpec &Out) {
	const auto *Sf = dynamic_cast<const ScalarFuncExprAST *>(Expr);
	if(!Sf || !IsSupportedScalarFn(Sf->Fn))
		return false;
	Out.OutColumn = OutCol;
	Out.FnTag = ScalarSqlFnTag(Sf->Fn);
	Out.Args.clear();
	for(const auto &Arg : Sf->Args) {
		if(!Arg)
			return false;
		if(const auto *Lit = dynamic_cast<const LiteralAST *>(Arg.get()))
			Out.Args.emplace_back(0, Lit->Value);
		else if(const auto *Col = dynamic_cast<const ColumnRefAST *>(Arg.get()))
			Out.Args.emplace_back(0, Col->Name);
		else
			return false;
	}
	return true;
}

} // namespace

std::optional<FusionPlan> SemistructuredFusionPass::TryFuse(const SelectAST &Sel) {
	if(Sel.HasFusedPlan() || Sel.SourceTableName().empty() || !Sel.WhereRoot())
		return std::nullopt;
	if(!Sel.JoinSpecs().empty() || !Sel.GroupKeys().empty() || Sel.DistinctSelected())
		return std::nullopt;
	if(Sel.HavingRoot() != nullptr || !Sel.WindowSpecs().empty())
		return std::nullopt;
	if(Sel.SelectLimitValue() < 0 || Sel.OrderBySpecs().size() != 1)
		return std::nullopt;

	std::vector<FusedProjectionSpec> Projs;
	const auto &Cols = Sel.ProjectionColumns();
	const auto &Exprs = Sel.ProjectionExprs();
	if(Cols.size() != Exprs.size())
		return std::nullopt;
	for(std::size_t I = 0; I < Exprs.size(); ++I) {
		if(!Exprs[I] || dynamic_cast<const ColumnRefAST *>(Exprs[I].get()))
			continue;
		FusedProjectionSpec Spec;
		if(!ExtractProjection(Exprs[I].get(), Cols[I], Spec))
			return std::nullopt;
		Projs.push_back(std::move(Spec));
	}
	if(Projs.empty())
		return std::nullopt;

	std::vector<FilterTriple> Filters;
	if(!ExtractWhereFilters(Sel.WhereRoot(), Filters))
		return std::nullopt;

	FusionPlan Plan;
	Plan.Kind = FusionKind::ScanFilterProjectLimit;
	Plan.Table = Sel.SourceTableName();
	Plan.Filters = std::move(Filters);
	Plan.Projections = std::move(Projs);
	Plan.OrderColumn = Sel.OrderBySpecs()[0].Column;
	Plan.OrderAscending = Sel.OrderBySpecs()[0].Ascending;
	Plan.Limit = Sel.SelectLimitValue();
	Plan.Offset = Sel.SelectOffsetValue();
	return Plan;
}

} // namespace SQL
} // namespace AstralDB
