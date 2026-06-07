#pragma once



#include <SQL/Fusion/FusionPlanTypes.hxx>

#include <SQL/Optimizer/ConfidenceScoring.hxx>

#include <DS/Tree.hxx>

#include <SQL/SQL.hxx>

#include <SQL/Profiler/SqlSessionConfig.hxx>



#include <optional>

#include <string>

#include <vector>



namespace AstralDB {

namespace SQL {



/** AST-level operator fusion: merge SELECT/WHERE/AGG/LIMIT/JOIN patterns before codegen. */

class FusionPass {

public:

	bool Run(Tree<ASTNode> &Ast, const SqlSessionConfig *SessionCfg = nullptr);



	static bool CanFuse(const SelectAST &Sel, FusionKind Kind);

	static FusionPlan Fuse(const SelectAST &Sel, FusionKind Kind);

	static bool ExtractSimplePredicates(const ExpressionAST *Root, std::vector<FilterTriple> &Out);



private:

	bool FuseSubtree(Tree<ASTNode>::Node *Node, const SqlSessionConfig *SessionCfg, Confidence MinConfidence,

	                 ConfidenceScorer &Scorer);

	static std::optional<FusionKind> DetectKind(const SelectAST &Sel);

};



} // namespace SQL

} // namespace AstralDB


