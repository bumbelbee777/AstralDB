#pragma once

#include <SQL/SQL.hxx>

namespace AstralDB {
namespace SQL {

/** Pattern detection and compile-time rewrite for recursive CTEs. */
class RecursiveCteOptimizer {
public:
	enum class RecursionKind { None, ParentChild, LinearAdjacency, CategoryExpansion, Generic };

	[[nodiscard]] RecursionKind Classify(const CteClause &Cte) const;
	bool TryOptimize(WithSelectAST &With);
	bool TryOptimizeWithMaterializedPath(CteClause &Cte);
	bool TryClosedFormLinearAdjacency(CteClause &Cte);
	bool TryCategoryExpansionBfs(CteClause &Cte);

private:
	[[nodiscard]] bool DetectParentChildJoin(const SelectAST &Recursive, std::string &IdCol, std::string &ParentCol) const;
	[[nodiscard]] bool DetectLinearAdjacency(const SelectAST &Recursive, int64_t &StepOut, std::string &KeyCol) const;
	[[nodiscard]] bool DetectCategoryExpansion(const SelectAST &Recursive, std::string &EqCol, std::string &PkCol) const;
};

} // namespace SQL
} // namespace AstralDB
