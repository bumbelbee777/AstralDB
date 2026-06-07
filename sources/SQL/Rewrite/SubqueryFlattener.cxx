#include <SQL/Rewrite/RewritePipeline.hxx>

#include <memory>
#include <unordered_set>

namespace AstralDB {
namespace SQL {

namespace {

void CollectColumnRefs(const ExpressionAST *E, std::vector<std::string> &Out) {
	if(!E)
		return;
	if(const auto *C = dynamic_cast<const ColumnRefAST *>(E)) {
		Out.push_back(C->Name);
		return;
	}
	if(const auto *B = dynamic_cast<const BinaryOpAST *>(E)) {
		CollectColumnRefs(B->LHS.get(), Out);
		CollectColumnRefs(B->RHS.get(), Out);
		return;
	}
	if(const auto *In = dynamic_cast<const InSubqueryPredAST *>(E)) {
		Out.push_back(In->LhsColumn);
		CollectColumnRefs(In->InnerWhere.get(), Out);
		return;
	}
	if(const auto *Ex = dynamic_cast<const ExistsPredAST *>(E))
		CollectColumnRefs(Ex->InnerWhere.get(), Out);
}

bool CollectOuterRefs(const ExpressionAST *SubWhere, const std::unordered_set<std::string> &OuterCols) {
	std::vector<std::string> InnerRefs;
	CollectColumnRefs(SubWhere, InnerRefs);
	for(const std::string &R : InnerRefs) {
		if(OuterCols.count(R))
			return true;
	}
	return false;
}

std::unique_ptr<ExpressionAST> CloneExpr(const ExpressionAST *E) {
	if(!E)
		return nullptr;
	if(const auto *C = dynamic_cast<const ColumnRefAST *>(E))
		return std::make_unique<ColumnRefAST>(C->Name);
	if(const auto *L = dynamic_cast<const LiteralAST *>(E))
		return std::make_unique<LiteralAST>(L->Value);
	if(const auto *B = dynamic_cast<const BinaryOpAST *>(E))
		return std::make_unique<BinaryOpAST>(CloneExpr(B->LHS.get()), B->Op, CloneExpr(B->RHS.get()));
	if(const auto *In = dynamic_cast<const InSubqueryPredAST *>(E)) {
		return std::make_unique<InSubqueryPredAST>(In->Negated, In->LhsColumn, In->InnerTable, In->InnerValueColumn,
		                                           CloneExpr(In->InnerWhere.get()));
	}
	if(const auto *Ex = dynamic_cast<const ExistsPredAST *>(E)) {
		auto Out = std::make_unique<ExistsPredAST>(Ex->Negated, Ex->InnerTable, CloneExpr(Ex->InnerWhere.get()));
		Out->SemiJoinOuterKey = Ex->SemiJoinOuterKey;
		Out->SemiJoinInnerKey = Ex->SemiJoinInnerKey;
		return Out;
	}
	return nullptr;
}

std::unique_ptr<ExpressionAST> RewriteWhereTree(ExpressionAST *Root, SelectAST &Sel, bool &Changed) {
	if(!Root)
		return nullptr;
	if(auto *In = dynamic_cast<InSubqueryPredAST *>(Root)) {
		std::unordered_set<std::string> OuterCols(Sel.ProjectionColumns().begin(), Sel.ProjectionColumns().end());
		for(const auto &J : Sel.JoinSpecs()) {
			(void)J;
		}
		if(!CollectOuterRefs(In->InnerWhere.get(), OuterCols)) {
			const std::string Derived = std::string("__astral_flat_") + In->InnerTable;
			JoinClause Jc;
			Jc.Kind = SqlJoinKind::Inner;
			Jc.RightTable = Derived;
			Jc.OnPairs.emplace_back(In->LhsColumn, In->InnerValueColumn);
			Sel.RewriteJoins().push_back(std::move(Jc));
			Changed = true;
			return std::make_unique<BooleanLiteralAST>(!In->Negated);
		}
	}
	if(auto *B = dynamic_cast<BinaryOpAST *>(Root)) {
		auto L = RewriteWhereTree(B->LHS.get(), Sel, Changed);
		auto R = RewriteWhereTree(B->RHS.get(), Sel, Changed);
		return std::make_unique<BinaryOpAST>(std::move(L), B->Op, std::move(R));
	}
	return CloneExpr(Root);
}

void FlattenSelect(SelectAST &Sel, bool &Changed) {
	if(!Sel.WhereRoot())
		return;
	auto NewWhere = RewriteWhereTree(const_cast<ExpressionAST *>(Sel.WhereRoot()), Sel, Changed);
	if(Changed)
		Sel.RewriteWhere(std::move(NewWhere));
}

void WalkStatement(StatementAST &Root, bool &Changed) {
	if(auto *Sel = dynamic_cast<SelectAST *>(&Root)) {
		FlattenSelect(*Sel, Changed);
		return;
	}
	if(auto *With = dynamic_cast<WithSelectAST *>(&Root)) {
		for(CteClause &C : With->Clauses) {
			if(C.Anchor)
				FlattenSelect(*C.Anchor, Changed);
			if(C.RecursiveStep)
				FlattenSelect(*C.RecursiveStep, Changed);
		}
		if(With->Main)
			WalkStatement(*With->Main, Changed);
		return;
	}
	if(auto *Cmp = dynamic_cast<CompoundSelectAST *>(&Root)) {
		for(auto &Arm : Cmp->Arms)
			if(Arm)
				FlattenSelect(*Arm, Changed);
	}
}

} // namespace

bool RunSubqueryFlattener(StatementAST &Root) {
	bool Changed = false;
	WalkStatement(Root, Changed);
	return Changed;
}

} // namespace SQL
} // namespace AstralDB
