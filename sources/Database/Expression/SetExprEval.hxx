#pragma once

#include <Database/Database.hxx>
#include <optional>
#include <string>
#include <string_view>

namespace AstralDB {

/** Row bindings for evaluating assignment RHS expressions. */
struct RowEvalContext {
	const Database::Item *Target = nullptr;
	const Database::Item *Excluded = nullptr;
	const Database::Item *Source = nullptr;
};

/** Deserialize and evaluate a serialized assignment expression (SQL frontend encoding). */
std::optional<std::string> EvalSerializedSetValueExpr(std::string_view Blob, const RowEvalContext &Ctx);

} // namespace AstralDB
