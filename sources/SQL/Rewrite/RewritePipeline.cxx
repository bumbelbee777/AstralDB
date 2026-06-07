#include <SQL/Rewrite/RewritePipeline.hxx>
#include <SQL/Rewrite/RecursiveCteOptimizer.hxx>

namespace AstralDB {
namespace SQL {

bool RewritePipeline::Run(StatementAST &Root) {
	bool Changed = false;
	// Subquery pull-up requires derived-table materialization; disabled until CTE lowering exists.
	Changed = RunSemiJoinRewriter(Root) || Changed;
	Changed = RunMagicSetRewriter(Root) || Changed;
	if(auto *With = dynamic_cast<WithSelectAST *>(&Root)) {
		RecursiveCteOptimizer Opt;
		Changed = Opt.TryOptimize(*With) || Changed;
	}
	return Changed;
}

bool RewritePipeline::RunTree(Tree<ASTNode> &Statements) {
	bool Changed = false;
	for(auto &NodePtr : Statements.Nodes_) {
		if(NodePtr && NodePtr->Value)
			Changed = Run(*NodePtr->Value) || Changed;
	}
	return Changed;
}

} // namespace SQL
} // namespace AstralDB
