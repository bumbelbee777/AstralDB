#pragma once

#include <Database/Database.hxx>
#include <Database/Storage/ColumnarStorage.hxx>

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace AstralDB {

struct LazyCrossTableFilterCtx {
	const std::vector<Database::Column> *FactSchema = nullptr;
	const std::vector<std::vector<Database::Column>> *LinkedSchemas = nullptr;
	bool LazyMaterialization = true;
};

[[nodiscard]] bool LazyEvalCrossTableFilterPred(const std::tuple<std::string, std::string, std::string> &Pred,
                                                int64_t FactRowId, int64_t LinkedRowId,
                                                const LazyCrossTableFilterCtx &Ctx);

[[nodiscard]] bool LazyCrossTablePassesFilters(const BulkWhereDnf *Filters, int64_t FactRowId, int64_t LinkedRowId,
                                               const LazyCrossTableFilterCtx &Ctx);

} // namespace AstralDB
