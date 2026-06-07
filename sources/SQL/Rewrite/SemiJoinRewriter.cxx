#include <SQL/Rewrite/RewritePipeline.hxx>

namespace AstralDB {
namespace SQL {

namespace {

bool TryExtractEqCorrelation(const ExpressionAST *Where, std::string &OuterKey, std::string &InnerKey) {
	const auto *B = dynamic_cast<const BinaryOpAST *>(Where);
	if(!B || B->Op != "=")
		return false;
	const auto *L = dynamic_cast<const ColumnRefAST *>(B->LHS.get());
	const auto *R = dynamic_cast<const ColumnRefAST *>(B->RHS.get());
	if(!L || !R)
		return false;
	OuterKey = L->Name;
	InnerKey = R->Name;
	return true;
}

bool RewriteExists(ExistsPredAST &Ex) {
	if(Ex.SemiJoinRewritten())
		return false;
	std::string OuterKey;
	std::string InnerKey;
	if(!TryExtractEqCorrelation(Ex.InnerWhere.get(), OuterKey, InnerKey))
		return false;
	Ex.SemiJoinOuterKey = std::move(OuterKey);
	Ex.SemiJoinInnerKey = std::move(InnerKey);
	return true;
}

void WalkExpr(ExpressionAST *E, bool &Changed) {
	if(!E)
		return;
	if(auto *Ex = dynamic_cast<ExistsPredAST *>(E)) {
		if(RewriteExists(*Ex))
			Changed = true;
		return;
	}
	if(auto *B = dynamic_cast<BinaryOpAST *>(E)) {
		WalkExpr(B->LHS.get(), Changed);
		WalkExpr(B->RHS.get(), Changed);
	}
}

void WalkSelect(SelectAST &Sel, bool &Changed) {
	WalkExpr(const_cast<ExpressionAST *>(Sel.WhereRoot()), Changed);
}

void WalkStatement(StatementAST &Root, bool &Changed) {
	if(auto *Sel = dynamic_cast<SelectAST *>(&Root)) {
		WalkSelect(*Sel, Changed);
		return;
	}
	if(auto *With = dynamic_cast<WithSelectAST *>(&Root)) {
		for(CteClause &C : With->Clauses) {
			if(C.Anchor)
				WalkSelect(*C.Anchor, Changed);
			if(C.RecursiveStep)
				WalkSelect(*C.RecursiveStep, Changed);
		}
		if(With->Main)
			WalkStatement(*With->Main, Changed);
	}
}

} // namespace

bool RunSemiJoinRewriter(StatementAST &Root) {
	bool Changed = false;
	WalkStatement(Root, Changed);
	return Changed;
}

} // namespace SQL
} // namespace AstralDB
