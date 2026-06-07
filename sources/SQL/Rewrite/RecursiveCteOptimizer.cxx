#include <SQL/Rewrite/RecursiveCteOptimizer.hxx>

#include <cctype>
#include <cstdlib>

namespace AstralDB {
namespace SQL {

namespace {

bool ColumnNameFromExpr(const ExpressionAST *E, std::string &Out) {
	if(!E)
		return false;
	if(const auto *C = dynamic_cast<const ColumnRefAST *>(E)) {
		Out = C->Name;
		return true;
	}
	return false;
}

bool ParseInt64Literal(const ExpressionAST *E, int64_t &Out) {
	if(!E)
		return false;
	if(const auto *L = dynamic_cast<const LiteralAST *>(E)) {
		try {
			Out = std::stoll(L->Value);
			return true;
		} catch(...) {
			return false;
		}
	}
	return false;
}

} // namespace

RecursiveCteOptimizer::RecursionKind RecursiveCteOptimizer::Classify(const CteClause &Cte) const {
	if(!Cte.IsRecursive())
		return RecursionKind::None;
	std::string IdCol;
	std::string ParentCol;
	if(DetectParentChildJoin(*Cte.RecursiveStep, IdCol, ParentCol))
		return RecursionKind::ParentChild;
	int64_t Step = 0;
	std::string KeyCol;
	if(DetectLinearAdjacency(*Cte.RecursiveStep, Step, KeyCol))
		return RecursionKind::LinearAdjacency;
	std::string EqCol;
	std::string PkCol;
	if(DetectCategoryExpansion(*Cte.RecursiveStep, EqCol, PkCol))
		return RecursionKind::CategoryExpansion;
	return RecursionKind::Generic;
}

bool RecursiveCteOptimizer::DetectParentChildJoin(const SelectAST &Recursive, std::string &IdCol,
                                                std::string &ParentCol) const {
	const ExpressionAST *Where = Recursive.WhereRoot();
	if(!Where)
		return false;
	const auto *B = dynamic_cast<const BinaryOpAST *>(Where);
	if(!B || B->Op != "=")
		return false;
	std::string A;
	std::string C;
	if(!ColumnNameFromExpr(B->LHS.get(), A) || !ColumnNameFromExpr(B->RHS.get(), C))
		return false;
	const auto IsParent = [](const std::string &N) {
		const std::string L = [&] {
			std::string S = N;
			for(char &Ch : S)
				Ch = static_cast<char>(std::tolower(static_cast<unsigned char>(Ch)));
			return S;
		}();
		return L == "parent_id" || L == "parent" || L == "manager_id";
	};
	const auto IsId = [](const std::string &N) {
		const std::string L = [&] {
			std::string S = N;
			for(char &Ch : S)
				Ch = static_cast<char>(std::tolower(static_cast<unsigned char>(Ch)));
			return S;
		}();
		return L == "id" || L.ends_with("_id");
	};
	if(IsParent(C) && IsId(A)) {
		IdCol = A;
		ParentCol = C;
		return true;
	}
	if(IsParent(A) && IsId(C)) {
		IdCol = C;
		ParentCol = A;
		return true;
	}
	return false;
}

bool RecursiveCteOptimizer::DetectLinearAdjacency(const SelectAST &Recursive, int64_t &StepOut,
                                                std::string &KeyCol) const {
	const ExpressionAST *Where = Recursive.WhereRoot();
	const auto *B = dynamic_cast<const BinaryOpAST *>(Where);
	if(!B || B->Op != "=")
		return false;
	const auto *Add = dynamic_cast<const BinaryOpAST *>(B->LHS.get());
	if(!Add || Add->Op != "+")
		return false;
	std::string LeftCol;
	if(!ColumnNameFromExpr(Add->LHS.get(), LeftCol))
		return false;
	int64_t Step = 0;
	if(!ParseInt64Literal(Add->RHS.get(), Step) && !ParseInt64Literal(Add->LHS.get(), Step))
		return false;
	std::string RightCol;
	if(!ColumnNameFromExpr(B->RHS.get(), RightCol))
		return false;
	if(LeftCol != RightCol)
		return false;
	KeyCol = LeftCol;
	StepOut = Step;
	return Step != 0;
}

bool RecursiveCteOptimizer::DetectCategoryExpansion(const SelectAST &Recursive, std::string &EqCol,
                                                  std::string &PkCol) const {
	const ExpressionAST *Where = Recursive.WhereRoot();
	const auto *And = dynamic_cast<const BinaryOpAST *>(Where);
	if(!And || And->Op != "AND")
		return false;
	const auto *Eq = dynamic_cast<const BinaryOpAST *>(And->LHS.get());
	const auto *Ne = dynamic_cast<const BinaryOpAST *>(And->RHS.get());
	if(!Eq || Eq->Op != "=" || !Ne || Ne->Op != "!=")
		return false;
	std::string A;
	std::string B;
	if(!ColumnNameFromExpr(Eq->LHS.get(), A) || !ColumnNameFromExpr(Eq->RHS.get(), B) || A != B)
		return false;
	EqCol = A;
	std::string P1;
	std::string P2;
	if(!ColumnNameFromExpr(Ne->LHS.get(), P1) || !ColumnNameFromExpr(Ne->RHS.get(), P2) || P1 != P2)
		return false;
	PkCol = P1;
	return true;
}

bool RecursiveCteOptimizer::TryOptimizeWithMaterializedPath(CteClause &Cte) {
	std::string IdCol;
	std::string ParentCol;
	if(!DetectParentChildJoin(*Cte.RecursiveStep, IdCol, ParentCol))
		return false;
	Cte.Pattern = RecursiveCtePattern::ParentChild;
	Cte.PreferHierarchyScan = true;
	Cte.HierarchyIdCol = std::move(IdCol);
	Cte.HierarchyParentCol = std::move(ParentCol);
	return true;
}

bool RecursiveCteOptimizer::TryClosedFormLinearAdjacency(CteClause &Cte) {
	int64_t Step = 0;
	std::string KeyCol;
	if(!DetectLinearAdjacency(*Cte.RecursiveStep, Step, KeyCol))
		return false;
	Cte.Pattern = RecursiveCtePattern::LinearAdjacency;
	Cte.LinearAdjacencyStep = Step;
	Cte.PreferBfs = true;
	return true;
}

bool RecursiveCteOptimizer::TryCategoryExpansionBfs(CteClause &Cte) {
	std::string EqCol;
	std::string PkCol;
	if(!DetectCategoryExpansion(*Cte.RecursiveStep, EqCol, PkCol))
		return false;
	Cte.Pattern = RecursiveCtePattern::CategoryExpansion;
	Cte.PreferBfs = true;
	(void)EqCol;
	(void)PkCol;
	return true;
}

bool RecursiveCteOptimizer::TryOptimize(WithSelectAST &With) {
	bool Changed = false;
	for(CteClause &Cte : With.Clauses) {
		if(!Cte.IsRecursive())
			continue;
		switch(Classify(Cte)) {
		case RecursionKind::ParentChild:
			Changed = TryOptimizeWithMaterializedPath(Cte) || Changed;
			break;
		case RecursionKind::LinearAdjacency:
			Changed = TryClosedFormLinearAdjacency(Cte) || Changed;
			break;
		case RecursionKind::CategoryExpansion:
			Changed = TryCategoryExpansionBfs(Cte) || Changed;
			break;
		case RecursionKind::Generic:
			Cte.PreferBfs = true;
			Changed = true;
			break;
		default:
			break;
		}
	}
	return Changed;
}

} // namespace SQL
} // namespace AstralDB
