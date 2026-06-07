#include <SQL/SemistructuredPlanner.hxx>

#include <Database/Storage/PredicateKind.hxx>
#include <Database/Storage/SimdTiling.hxx>

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

void ExtractFromFilterTriple(const FilterTriple &F, std::vector<JsonPredicate> &JsonPreds,
                             std::vector<XmlPredicate> &XmlPreds, std::vector<RegexPredicate> &RegexPreds,
                             FtsPredicate &FtsPred) {
	if(F.Op == "JSON_EXTRACT" || F.Op == "NOT JSON_EXTRACT") {
		const std::size_t Split = F.Literal.find('\x1E');
		if(Split == std::string::npos)
			return;
		JsonPreds.push_back({F.Column, F.Literal.substr(0, Split), F.Literal.substr(Split + 1)});
		return;
	}
	if(F.Op == "XML_VALID" || F.Op == "NOT XML_VALID") {
		XmlPreds.push_back({F.Column, {}, F.Literal, true});
		return;
	}
	if(F.Op == "REGEXP" || F.Op == "~" || F.Op == "NOT REGEXP" || F.Op == "!~") {
		RegexPreds.push_back({F.Column, F.Literal});
		return;
	}
	if(F.Op == "MATCH" || F.Op == "NOT MATCH") {
		FtsPred.Column = F.Column;
		FtsPred.Query = F.Literal;
	}
}

void AppendPredicateInstructions(const std::vector<JsonPredicate> &JsonPreds, const std::vector<XmlPredicate> &XmlPreds,
                                 const std::vector<RegexPredicate> &RegexPreds, const FtsPredicate &FtsPred,
                                 SSProgram &Prog) {
	for(const JsonPredicate &J : JsonPreds) {
		SSInstruction Inst;
		Inst.Op = SSOpcode::JSON_EXTRACT_BATCH;
		Inst.StrOperands = {J.Column, J.Path, J.Expected};
		Prog.Instructions.push_back(std::move(Inst));
	}
	for(const XmlPredicate &X : XmlPreds) {
		SSInstruction Inst;
		if(X.ValidOnly) {
			Inst.Op = SSOpcode::XML_EXTRACT_BATCH;
			Inst.IntOperands = {1};
			Inst.StrOperands = {X.Column};
		} else {
			Inst.Op = SSOpcode::XML_EXTRACT_BATCH;
			Inst.StrOperands = {X.Column, X.Path, X.Expected};
		}
		Prog.Instructions.push_back(std::move(Inst));
	}
	for(const RegexPredicate &R : RegexPreds) {
		SSInstruction Inst;
		Inst.Op = SSOpcode::REGEX_BATCH;
		Inst.StrOperands = {R.Column, R.Pattern};
		Prog.Instructions.push_back(std::move(Inst));
	}
	if(!FtsPred.Column.empty()) {
		SSInstruction Inst;
		Inst.Op = SSOpcode::FTS_MATCH_BATCH;
		Inst.StrOperands = {FtsPred.Column, FtsPred.Query};
		Prog.Instructions.push_back(std::move(Inst));
	}
}

} // namespace

bool SemistructuredPlanner::IsSemistructuredQuery(const SelectAST &Sel) {
	if(Sel.SourceTableName().empty() || !Sel.WhereRoot())
		return false;
	if(!Sel.JoinSpecs().empty() || !Sel.GroupKeys().empty() || Sel.DistinctSelected())
		return false;
	if(Sel.HavingRoot() != nullptr || !Sel.WindowSpecs().empty())
		return false;
	if(Sel.SelectLimitValue() < 0 || Sel.OrderBySpecs().size() != 1)
		return false;
	return true;
}

void SemistructuredPlanner::ExtractPredicates(const ExpressionAST *Where, std::vector<JsonPredicate> &JsonPreds,
                                              std::vector<XmlPredicate> &XmlPreds, std::vector<RegexPredicate> &RegexPreds,
                                              FtsPredicate &FtsPred) {
	(void)RegexPreds;
	std::vector<const ExpressionAST *> Atoms;
	FlattenAnd(Where, Atoms);
	for(const ExpressionAST *Atom : Atoms) {
		if(const auto *Bin = dynamic_cast<const BinaryOpAST *>(Atom)) {
			if(Bin->Op == "MATCH" || Bin->Op == "NOT MATCH") {
				std::string Col, Query;
				if(ColumnNameFromExpr(Bin->LHS.get(), Col) && LiteralFromExpr(Bin->RHS.get(), Query)) {
					FtsPred = {Col, Query};
					continue;
				}
			}
			const auto *Sf = dynamic_cast<const ScalarFuncExprAST *>(Bin->LHS.get());
			std::string Rhs;
			if(Sf && LiteralFromExpr(Bin->RHS.get(), Rhs)) {
				std::string Col;
				if(Sf->Fn == ScalarSqlFn::JsonExtract && Sf->Args.size() == 2 &&
				   ColumnNameFromExpr(Sf->Args[0].get(), Col)) {
					std::string Path;
					if(LiteralFromExpr(Sf->Args[1].get(), Path))
						JsonPreds.push_back({Col, Path, Rhs});
					continue;
				}
				if(Sf->Fn == ScalarSqlFn::XmlValid && Sf->Args.size() == 1 &&
				   ColumnNameFromExpr(Sf->Args[0].get(), Col)) {
					XmlPreds.push_back({Col, {}, Rhs, true});
					continue;
				}
			}
		}
	}
}

void SemistructuredPlanner::ExtractPredicatesFromFilters(const std::vector<FilterTriple> &Filters,
                                                         std::vector<JsonPredicate> &JsonPreds,
                                                         std::vector<XmlPredicate> &XmlPreds,
                                                         std::vector<RegexPredicate> &RegexPreds, FtsPredicate &FtsPred) {
	for(const FilterTriple &F : Filters)
		ExtractFromFilterTriple(F, JsonPreds, XmlPreds, RegexPreds, FtsPred);
}

std::uint32_t SemistructuredPlanner::DetermineBatchSize(const std::size_t EstimatedRows,
                                                        const std::size_t RowWidthBytes) {
	const TiledCachePlan Plan = SimdTiling::ActivePlan(WorkloadClass::OlapScan, RowWidthBytes > 0 ? RowWidthBytes : 64);
	if(EstimatedRows <= 512 || Plan.L1PanelElements <= 512)
		return 512;
	if(EstimatedRows <= 4'096 || Plan.L2BlockElements <= 4096)
		return 4096;
	if(EstimatedRows <= 65'536 || Plan.L3BlockElements <= 65536)
		return 65536;
	return 1024;
}

void SemistructuredPlanner::ConfigureExecution(SSProgram &Prog, const std::size_t EstimatedRows,
                                               const std::size_t RowWidthBytes) {
	Prog.BatchSize = DetermineBatchSize(EstimatedRows, RowWidthBytes);
	Prog.UseSimd = true;
	Prog.UseParallel = EstimatedRows >= 256'000;
}

SSProgram SemistructuredPlanner::PlanScan(const std::string &Table, const BulkWhereDnf &FilterDnf,
                                          const std::vector<SemistructuredProjectionSpec> &Projections,
                                          const std::string &OrderCol, const bool OrderAscending, const std::size_t Limit,
                                          const std::size_t EstimatedRows) {
	(void)Table;
	SSProgram Prog;
	std::vector<JsonPredicate> JsonPreds;
	std::vector<XmlPredicate> XmlPreds;
	std::vector<RegexPredicate> RegexPreds;
	FtsPredicate FtsPred;
	if(!FilterDnf.empty() && !FilterDnf.front().empty()) {
		std::vector<FilterTriple> Filters;
		for(const auto &Pred : FilterDnf.front()) {
			const auto &[Col, Op, Lit] = Pred;
			Filters.push_back({Col, Op, Lit});
		}
		ExtractPredicatesFromFilters(Filters, JsonPreds, XmlPreds, RegexPreds, FtsPred);
	}
	AppendPredicateInstructions(JsonPreds, XmlPreds, RegexPreds, FtsPred, Prog);

	{
		SSInstruction Rank;
		Rank.Op = SSOpcode::RANK_BATCH;
		Rank.StrOperands = {OrderCol};
		Prog.Instructions.push_back(std::move(Rank));
	}
	{
		SSInstruction TopKInit;
		TopKInit.Op = SSOpcode::TOPK_INIT;
		TopKInit.IntOperands = {static_cast<int32_t>(Limit), OrderAscending ? 1 : 0};
		Prog.Instructions.push_back(std::move(TopKInit));
	}
	{
		SSInstruction Mat;
		Mat.Op = SSOpcode::MATERIALIZE_BATCH;
		Mat.IntOperands = {static_cast<int32_t>(Projections.size())};
		for(const SemistructuredProjectionSpec &P : Projections)
			Mat.StrOperands.push_back(P.OutCol);
		Prog.Instructions.push_back(std::move(Mat));
	}
	{
		SSInstruction Fused;
		Fused.Op = SSOpcode::FUSED_EXECUTE;
		Prog.Instructions.push_back(std::move(Fused));
	}
	ConfigureExecution(Prog, EstimatedRows, 128);
	return Prog;
}

SSProgram SemistructuredPlanner::Plan(const FusionPlan &Plan) {
	std::vector<JsonPredicate> JsonPreds;
	std::vector<XmlPredicate> XmlPreds;
	std::vector<RegexPredicate> RegexPreds;
	FtsPredicate FtsPred;
	ExtractPredicatesFromFilters(Plan.Filters, JsonPreds, XmlPreds, RegexPreds, FtsPred);

	SSProgram Prog;
	AppendPredicateInstructions(JsonPreds, XmlPreds, RegexPreds, FtsPred, Prog);
	{
		SSInstruction Rank;
		Rank.Op = SSOpcode::RANK_BATCH;
		Rank.StrOperands = {Plan.OrderColumn};
		Prog.Instructions.push_back(std::move(Rank));
	}
	{
		SSInstruction TopKInit;
		TopKInit.Op = SSOpcode::TOPK_INIT;
		TopKInit.IntOperands = {static_cast<int32_t>(Plan.Limit), Plan.OrderAscending ? 1 : 0};
		Prog.Instructions.push_back(std::move(TopKInit));
	}
	{
		SSInstruction Mat;
		Mat.Op = SSOpcode::MATERIALIZE_BATCH;
		Mat.IntOperands = {static_cast<int32_t>(Plan.Projections.size())};
		for(const FusedProjectionSpec &P : Plan.Projections)
			Mat.StrOperands.push_back(P.OutColumn);
		Prog.Instructions.push_back(std::move(Mat));
	}
	Prog.Instructions.push_back({SSOpcode::FUSED_EXECUTE, {}, {}, {}});
	ConfigureExecution(Prog, 1'000'000, 128);
	return Prog;
}

SSProgram SemistructuredPlanner::Plan(const SelectAST &Sel) {
	if(!IsSemistructuredQuery(Sel))
		return {};
	std::vector<JsonPredicate> JsonPreds;
	std::vector<XmlPredicate> XmlPreds;
	std::vector<RegexPredicate> RegexPreds;
	FtsPredicate FtsPred;
	ExtractPredicates(Sel.WhereRoot(), JsonPreds, XmlPreds, RegexPreds, FtsPred);

	SSProgram Prog;
	AppendPredicateInstructions(JsonPreds, XmlPreds, RegexPreds, FtsPred, Prog);
	{
		SSInstruction Rank;
		Rank.Op = SSOpcode::RANK_BATCH;
		Rank.StrOperands = {Sel.OrderBySpecs()[0].Column};
		Prog.Instructions.push_back(std::move(Rank));
	}
	{
		SSInstruction TopKInit;
		TopKInit.Op = SSOpcode::TOPK_INIT;
		TopKInit.IntOperands = {static_cast<int32_t>(Sel.SelectLimitValue()),
		                        Sel.OrderBySpecs()[0].Ascending ? 1 : 0};
		Prog.Instructions.push_back(std::move(TopKInit));
	}
	Prog.Instructions.push_back({SSOpcode::FUSED_EXECUTE, {}, {}, {}});
	ConfigureExecution(Prog, 1'000'000, 128);
	return Prog;
}

} // namespace SQL
} // namespace AstralDB
