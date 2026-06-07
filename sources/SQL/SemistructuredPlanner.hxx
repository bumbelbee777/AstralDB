#pragma once

#include <Database/Storage/BulkSyntheticSemistructured.hxx>
#include <SQL/Fusion/FusionPlanTypes.hxx>
#include <SQL/SemistructuredBytecode.hxx>
#include <SQL/SQL.hxx>

#include <cstddef>
#include <string>
#include <vector>

namespace AstralDB {
namespace SQL {

struct JsonPredicate {
	std::string Column;
	std::string Path;
	std::string Expected;
};

struct XmlPredicate {
	std::string Column;
	std::string Path;
	std::string Expected;
	bool ValidOnly = false;
};

struct RegexPredicate {
	std::string Column;
	std::string Pattern;
};

struct FtsPredicate {
	std::string Column;
	std::string Query;
};

class SemistructuredPlanner {
public:
	static SSProgram Plan(const SelectAST &Sel);
	static SSProgram Plan(const FusionPlan &Plan);
	static SSProgram PlanScan(const std::string &Table, const BulkWhereDnf &FilterDnf,
	                          const std::vector<SemistructuredProjectionSpec> &Projections, const std::string &OrderCol,
	                          bool OrderAscending, std::size_t Limit, std::size_t EstimatedRows);

private:
	static bool IsSemistructuredQuery(const SelectAST &Sel);
	static void ExtractPredicates(const ExpressionAST *Where, std::vector<JsonPredicate> &JsonPreds,
	                              std::vector<XmlPredicate> &XmlPreds, std::vector<RegexPredicate> &RegexPreds,
	                              FtsPredicate &FtsPred);
	static void ExtractPredicatesFromFilters(const std::vector<FilterTriple> &Filters,
	                                         std::vector<JsonPredicate> &JsonPreds, std::vector<XmlPredicate> &XmlPreds,
	                                         std::vector<RegexPredicate> &RegexPreds, FtsPredicate &FtsPred);
	static std::uint32_t DetermineBatchSize(std::size_t EstimatedRows, std::size_t RowWidthBytes);
	static void ConfigureExecution(SSProgram &Prog, std::size_t EstimatedRows, std::size_t RowWidthBytes);
};

} // namespace SQL
} // namespace AstralDB
