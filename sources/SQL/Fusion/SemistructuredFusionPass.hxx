#pragma once

#include <SQL/Fusion/FusionPlanTypes.hxx>
#include <SQL/SQL.hxx>

#include <optional>

namespace AstralDB {
namespace SQL {

class SemistructuredFusionPass {
public:
	static std::optional<FusionPlan> TryFuse(const SelectAST &Sel);
};

} // namespace SQL
} // namespace AstralDB
