#include <Database/Storage/LazyStarJoinFilter.hxx>

#include <Database/Storage/BulkSynthetic.hxx>

namespace AstralDB {

namespace {

const Database::Column *FindCol(const std::vector<Database::Column> &Schema, std::string_view Name) {
	for(const Database::Column &Co : Schema)
		if(Co.Name == Name)
			return &Co;
	return nullptr;
}

} // namespace

bool LazyEvalCrossTableFilterPred(const std::tuple<std::string, std::string, std::string> &Pred, const int64_t FactRowId,
                                  const int64_t LinkedRowId, const LazyCrossTableFilterCtx &Ctx) {
	const auto &[ColName, Op, Val] = Pred;
	const Database::Column *ColDef = nullptr;
	if(Ctx.FactSchema && FindCol(*Ctx.FactSchema, ColName))
		ColDef = FindCol(*Ctx.FactSchema, ColName);
	else if(Ctx.LinkedSchemas) {
		for(const std::vector<Database::Column> &Sch : *Ctx.LinkedSchemas) {
			if((ColDef = FindCol(Sch, ColName)) != nullptr)
				break;
		}
	}
	const int64_t RowId = ColDef && Ctx.FactSchema && FindCol(*Ctx.FactSchema, ColName) ? FactRowId : LinkedRowId;
	if(Ctx.LazyMaterialization) {
		if(const auto Fast = BulkSyntheticTryMatchPredicate(RowId, ColName, Op, Val, ColDef))
			return *Fast;
	}
	if(!ColDef)
		return false;
	return BulkSyntheticCellString(*ColDef, BulkSyntheticContext{RowId, 1, 0, 1}) == Val;
}

bool LazyCrossTablePassesFilters(const BulkWhereDnf *Filters, const int64_t FactRowId, const int64_t LinkedRowId,
                                 const LazyCrossTableFilterCtx &Ctx) {
	if(!Filters || Filters->empty())
		return true;
	for(const BulkWhereDnfBranch &Branch : *Filters) {
		if(Branch.empty())
			return true;
		bool BranchOk = true;
		for(const auto &Pred : Branch) {
			if(!LazyEvalCrossTableFilterPred(Pred, FactRowId, LinkedRowId, Ctx)) {
				BranchOk = false;
				break;
			}
		}
		if(BranchOk)
			return true;
	}
	return false;
}

} // namespace AstralDB
