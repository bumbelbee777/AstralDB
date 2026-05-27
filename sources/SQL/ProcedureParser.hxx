#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB::SQL {

enum class ProcedureDialectKind {
	Standard,
	PlSql,
	PlPgSql,
};

/** One \c WHEN … \c THEN handler inside an \c EXCEPTION block. */
struct ProcedureExceptionWhen {
	std::string Condition;
	std::string HandlerSql;
};

/** Lowered try body plus optional exception handlers for bytecode stitching. */
struct LoweredProcedureBody {
	std::string TryBodySql;
	std::vector<ProcedureExceptionWhen> ExceptionHandlers;
};

struct ProcedureParseResult {
	ProcedureDialectKind Dialect = ProcedureDialectKind::Standard;
	std::string ProcedureName;
	/** Lowered statement list for the main SQL parser / bytecode compiler. */
	std::string LoweredBodySql;
	LoweredProcedureBody Body_;
	/** Human-readable dialect tag stored in procedure metadata (\c plsql, \c plpgsql). */
	std::string DialectTag;
	bool OrReplace = false;
	bool IfNotExists = false;
};

/**
 * Dedicated lexer/parser for Oracle PL/SQL and PostgreSQL PL/pgSQL \c CREATE PROCEDURE|FUNCTION forms.
 * Dialect bodies are identified, then lowered to sequential AstralDB SQL for \c .abc compilation.
 */
class ProcedureParser {
public:
	explicit ProcedureParser(std::string_view Source);

	/** True for \c CREATE OR REPLACE, \c LANGUAGE plpgsql, dollar-quoted bodies, or \c IS … \c BEGIN forms. */
	static bool IsDialectProcedureStatement(std::string_view Source);

	/** Parse a full dialect \c CREATE … statement; throws \c std::runtime_error on failure. */
	ProcedureParseResult ParseDialectCreate() const;

	/** Lower an extracted dialect procedure body (after header parse) to executable SQL text. */
	static std::string LowerDialectBody(std::string_view BodyText, ProcedureDialectKind Dialect);

	/** Lower body and split \c EXCEPTION … \c WHEN handlers when present. */
	static LoweredProcedureBody LowerDialectBodyStructured(std::string_view BodyText, ProcedureDialectKind Dialect);

	static const char *DialectTag(ProcedureDialectKind Dialect);

private:
	std::string_view Source_;
};

} // namespace AstralDB::SQL
