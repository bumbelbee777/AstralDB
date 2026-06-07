#pragma once

#include <Database/Expression/SetExprEval.hxx>
#include <SQL/SQL.hxx>
#include <optional>
#include <string>

namespace AstralDB::SQL {

using RowEvalContext = AstralDB::RowEvalContext;

/** Evaluate a restricted expression tree for SET / MERGE / UPSERT assignments. */
std::optional<std::string> EvalSetValueExpr(const ExpressionAST *Expr, const RowEvalContext &Ctx);

/** Serialize for bytecode operands (UPDATE_MATCHING / MERGE / UPSERT). */
std::string SerializeSetValueExpr(const ExpressionAST *Expr);

/** Deep-copy \a Root replacing \c ColumnRefAST nodes named \a Param with \a BindingCol . */
std::unique_ptr<ExpressionAST> BindLambdaParameter(const ExpressionAST *Root, std::string_view Param,
                                                   std::string_view BindingCol);

/** True when \a Expr evaluates to a non-empty, non-zero value under \a Ctx . */
bool EvalExpressionTruthy(const ExpressionAST *Expr, const RowEvalContext &Ctx);

/** Serialize a lambda for \c COLUMNS_EXPAND / \c LIST_TRANSFORM operands. */
std::string SerializeLambdaExpr(const LambdaExprAST &Lambda);

} // namespace AstralDB::SQL
