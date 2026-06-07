#pragma once

#include <SQL/SQL.hxx>

namespace AstralDB {
namespace SQL {

/** Compile-time AST rewrite orchestrator (Phase 10). */
class RewritePipeline {
public:
	bool Run(StatementAST &Root);
	bool RunTree(Tree<ASTNode> &Statements);
};

bool RunSubqueryFlattener(StatementAST &Root);
bool RunSemiJoinRewriter(StatementAST &Root);
bool RunMagicSetRewriter(StatementAST &Root);

} // namespace SQL
} // namespace AstralDB
