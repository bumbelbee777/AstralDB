#include <SQL/Rewrite/RewritePipeline.hxx>

#include <memory>

namespace AstralDB {
namespace SQL {

namespace {

bool IsMaxSubqueryCorrelated(const BinaryOpAST *Eq, std::string &OuterCol, std::string & /*InnerTable*/,
                             std::string &PartitionCol, std::string &AggCol) {
	if(!Eq || Eq->Op != "=")
		return false;
	const auto *Lhs = dynamic_cast<const ColumnRefAST *>(Eq->LHS.get());
	if(!Lhs)
		return false;
	const auto *Rhs = dynamic_cast<const FuncCallExprAST *>(Eq->RHS.get());
	if(!Rhs || Rhs->BuiltinKind != FuncCallExprAST::Kind::Max)
		return false;
	const auto *Sub = dynamic_cast<const InSubqueryPredAST *>(Eq->RHS.get());
	(void)Sub;
	OuterCol = Lhs->Name;
	AggCol = Rhs->ArgColumn;
	PartitionCol = AggCol;
	return !AggCol.empty();
}

bool TryMagicSetSelect(SelectAST &Sel, bool &Changed) {
	const ExpressionAST *Where = Sel.WhereRoot();
	const auto *Eq = dynamic_cast<const BinaryOpAST *>(Where);
	if(!Eq)
		return false;
	std::string OuterCol;
	std::string InnerTable;
	std::string PartitionCol;
	std::string AggCol;
	if(!IsMaxSubqueryCorrelated(Eq, OuterCol, InnerTable, PartitionCol, AggCol))
		return false;
	WindowSpec W;
	W.Kind = WindowFnKind::Max;
	W.SourceColumn = AggCol;
	W.PartitionBy.push_back(PartitionCol);
	W.OutputColumn = std::string("__magic_max_") + AggCol;
	W.OrderColumn = AggCol;
	const std::string OutCol = W.OutputColumn;
	Sel.RewriteWindows().push_back(std::move(W));
	Sel.RewriteProjectionColumns().push_back(OutCol);
	Changed = true;
	return true;
}

void WalkStatement(StatementAST &Root, bool &Changed) {
	if(auto *Sel = dynamic_cast<SelectAST *>(&Root)) {
		TryMagicSetSelect(*Sel, Changed);
		return;
	}
	if(auto *With = dynamic_cast<WithSelectAST *>(&Root)) {
		if(With->Main)
			WalkStatement(*With->Main, Changed);
	}
}

} // namespace

bool RunMagicSetRewriter(StatementAST &Root) {
	bool Changed = false;
	WalkStatement(Root, Changed);
	return Changed;
}

} // namespace SQL
} // namespace AstralDB
