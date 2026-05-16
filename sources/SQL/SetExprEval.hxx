#pragma once

#include <Database/Database.hxx>
#include <SQL/SQL.hxx>
#include <optional>
#include <string>

namespace AstralDB::SQL {

/** Row bindings for evaluating assignment RHS expressions. */
struct RowEvalContext {
	const Database::Item *Target = nullptr;
	const Database::Item *Excluded = nullptr;
	const Database::Item *Source = nullptr;
};

/** Evaluate a restricted expression tree for SET / MERGE / UPSERT assignments. */
std::optional<std::string> EvalSetValueExpr(const ExpressionAST *Expr, const RowEvalContext &Ctx);

/** Serialize for bytecode operands (UPDATE_MATCHING / MERGE / UPSERT). */
std::string SerializeSetValueExpr(const ExpressionAST *Expr);

/** Deserialize and evaluate a serialized assignment expression. */
std::optional<std::string> EvalSerializedSetValueExpr(std::string_view Blob, const RowEvalContext &Ctx);

} // namespace AstralDB::SQL
