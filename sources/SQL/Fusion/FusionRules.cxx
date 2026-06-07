#include <SQL/Fusion/FusionPass.hxx>
#include <SQL/Fusion/SemistructuredFusionPass.hxx>
#include <SQL/Shape/ShapeComposition.hxx>
#include <SQL/Optimizer/AdaptiveOptimizer.hxx>
#include <SQL/Optimizer/ConfidenceScoring.hxx>
#include <SQL/Profiler/SqlSessionConfig.hxx>

#include <optional>

namespace AstralDB {
namespace SQL {

namespace {

bool IsComparisonOp(const std::string &Op) {
	return Op == "=" || Op == "==" || Op == "!=" || Op == "<>" || Op == ">" || Op == ">=" || Op == "<" || Op == "<=";
}

SelectAST *AsSelectMut(ASTNode &Node) {
	return dynamic_cast<SelectAST *>(Node.get());
}

bool HasOnlyTrivialProjections(const SelectAST &Sel) {
	for(const auto &Ptr : Sel.ProjectionExprs()) {
		if(!Ptr)
			continue;
		if(dynamic_cast<const ColumnsExprAST *>(Ptr.get()))
			continue;
		if(dynamic_cast<const ColumnRefAST *>(Ptr.get()))
			continue;
		return false;
	}
	return true;
}

bool HasUnsupportedPostScanFeatures(const SelectAST &Sel) {
	return !Sel.OrderBySpecs().empty() || Sel.DistinctSelected() || Sel.HavingRoot() != nullptr ||
	       !Sel.WindowSpecs().empty() || Sel.SelectLimitValue() >= 0 || Sel.SelectOffsetValue() > 0 ||
	       !HasOnlyTrivialProjections(Sel);
}

} // namespace

bool FusionPass::ExtractSimplePredicates(const ExpressionAST *Root, std::vector<FilterTriple> &Out) {
	if(!Root)
		return true;
	if(const auto *Bin = dynamic_cast<const BinaryOpAST *>(Root)) {
		if(Bin->Op == "AND") {
			return ExtractSimplePredicates(Bin->LHS.get(), Out) && ExtractSimplePredicates(Bin->RHS.get(), Out);
		}
		if(IsComparisonOp(Bin->Op)) {
			const auto *Col = dynamic_cast<const ColumnRefAST *>(Bin->LHS.get());
			const auto *Lit = dynamic_cast<const LiteralAST *>(Bin->RHS.get());
			if(!Col || !Lit)
				return false;
			Out.push_back({Col->Name, Bin->Op, Lit->Value});
			return true;
		}
		return false;
	}
	return false;
}

bool FusionPass::CanFuse(const SelectAST &Sel, FusionKind Kind) {
	if(HasUnsupportedPostScanFeatures(Sel))
		return false;
	switch(Kind) {
	case FusionKind::ScanFilter:
		return !Sel.SourceTableName().empty() && Sel.WhereRoot() && Sel.GroupKeys().empty() &&
		       Sel.AggKind() == GroupAggMode::None && Sel.JoinSpecs().empty();
	case FusionKind::ScanFilterAgg:
		return !Sel.SourceTableName().empty() && Sel.WhereRoot() && !Sel.GroupKeys().empty() &&
		       Sel.AggKind() == GroupAggMode::None && Sel.JoinSpecs().empty() &&
		       Sel.ComboAggs().size() == 1 && Sel.ComboAggs()[0].Kind == GroupCombAggKind::Sum;
	case FusionKind::ScanFilterAggLimit:
		return CanFuse(Sel, FusionKind::ScanFilterAgg) && Sel.SelectLimitValue() >= 0;
	case FusionKind::JoinFilter: {
		if(Sel.JoinSpecs().size() != 1 || Sel.JoinSpecs()[0].Kind != SqlJoinKind::Inner)
			return false;
		const auto &J = Sel.JoinSpecs()[0];
		return J.OnPairs.size() == 1 && Sel.WhereRoot();
	}
	case FusionKind::FilterMerge:
		return Sel.WhereRoot() != nullptr;
	default:
		return false;
	}
}

FusionPlan FusionPass::Fuse(const SelectAST &Sel, FusionKind Kind) {
	FusionPlan Plan;
	Plan.Kind = Kind;
	Plan.Table = Sel.SourceTableName();
	Plan.Limit = Sel.SelectLimitValue();
	Plan.Offset = Sel.SelectOffsetValue();
	ExtractSimplePredicates(Sel.WhereRoot(), Plan.Filters);
	if(Kind == FusionKind::ScanFilterAgg || Kind == FusionKind::ScanFilterAggLimit) {
		Plan.GroupKeys = Sel.GroupKeys();
		if(!Sel.ComboAggs().empty()) {
			Plan.SumColumn = Sel.ComboAggs()[0].SourceColumn;
			Plan.SumOutputColumn = Sel.ComboAggs()[0].OutputColumn;
		}
	}
	if(Kind == FusionKind::JoinFilter && !Sel.JoinSpecs().empty()) {
		const auto &J = Sel.JoinSpecs()[0];
		Plan.JoinRightTable = J.RightTable;
		Plan.JoinLeftCol = J.OnPairs[0].first;
		Plan.JoinRightCol = J.OnPairs[0].second;
	}
	return Plan;
}

std::optional<FusionKind> FusionPass::DetectKind(const SelectAST &Sel) {
	if(CanFuse(Sel, FusionKind::ScanFilterAggLimit))
		return FusionKind::ScanFilterAggLimit;
	if(CanFuse(Sel, FusionKind::ScanFilterAgg))
		return FusionKind::ScanFilterAgg;
	if(CanFuse(Sel, FusionKind::JoinFilter))
		return FusionKind::JoinFilter;
	if(CanFuse(Sel, FusionKind::ScanFilter))
		return FusionKind::ScanFilter;
	if(CanFuse(Sel, FusionKind::FilterMerge))
		return FusionKind::FilterMerge;
	return std::nullopt;
}

bool FusionPass::FuseSubtree(Tree<ASTNode>::Node *Node, const SqlSessionConfig *SessionCfg, Confidence MinConfidence,
                             ConfidenceScorer &Scorer) {
	if(!Node)
		return false;
	bool Changed = false;
	for(auto &Child : Node->Children)
		Changed = FuseSubtree(Child.get(), SessionCfg, MinConfidence, Scorer) || Changed;

	SelectAST *Sel = AsSelectMut(Node->Value);
	if(!Sel)
		return Changed;

	const auto AcceptFusion = [&](const FusionPlan &Plan, FusionKind Kind) -> bool {
		const bool HasJoin = Kind == FusionKind::JoinFilter;
		const Confidence Score = Scorer.ScoreFusionPlan(true, Plan.Filters.size(), HasJoin);
		return Score >= MinConfidence;
	};

	if(!Sel->HasFusedPlan()) {
		if(const auto Sp = SemistructuredFusionPass::TryFuse(*Sel)) {
			if(AcceptFusion(*Sp, Sp->Kind)) {
				Sel->SetFusedPlan(*Sp);
				Sel->SetStorageHint(StorageLayout::Columnar);
				return true;
			}
			return Changed;
		}
	}

	const auto Kind = DetectKind(*Sel);
	if(!Kind)
		return Changed;

	const FusionPlan Plan = Fuse(*Sel, *Kind);
	if(Plan.Filters.empty() && *Kind != FusionKind::JoinFilter && *Kind != FusionKind::ScanFilterProjectLimit)
		return Changed;
	if(!AcceptFusion(Plan, *Kind))
		return Changed;

	Sel->SetFusedPlan(Plan);
	Sel->SetStorageHint(StorageLayout::Columnar);
	if(*Kind == FusionKind::FilterMerge && Plan.Filters.size() >= 2)
		Changed = true;
	else if(*Kind != FusionKind::None)
		Changed = true;
	return Changed;
}

bool FusionPass::Run(Tree<ASTNode> &Ast, const SqlSessionConfig *SessionCfg) {
	Confidence MinConfidence = Confidence::Medium;
	if(SessionCfg)
		MinConfidence = ConfidenceFromOptFloor(SessionCfg->OptConfidenceFloor);
	ConfidenceScorer Scorer;
	bool Any = false;
	for(auto &Root : Ast.Nodes_)
		Any = FuseSubtree(Root.get(), SessionCfg, MinConfidence, Scorer) || Any;
	ShapeCompositionPass Composition;
	Composition.Run(Ast);
	return Any;
}

} // namespace SQL
} // namespace AstralDB
