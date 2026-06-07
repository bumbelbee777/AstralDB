#pragma once

#include <SQL/Shape/ShapeCompositionTypes.hxx>
#include <SQL/Fusion/FusionPlanTypes.hxx>
#include <SQL/SQL.hxx>

namespace AstralDB {
namespace SQL {

class ShapeCompositionPass {
public:
	bool Run(Tree<ASTNode> &Ast) noexcept;
};

[[nodiscard]] bool BuildShapeCompositionFromSelect(const SelectAST &Sel, QueryShapeComposition &Out) noexcept;

[[nodiscard]] bool BuildShapeCompositionFromCompound(const CompoundSelectAST &Comp, QueryShapeComposition &Out) noexcept;

[[nodiscard]] bool InferShapeCompositionFromBytecode(const Bytecode &Code, QueryShapeComposition &Out) noexcept;

} // namespace SQL
} // namespace AstralDB
