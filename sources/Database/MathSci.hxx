#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace AstralDB {
class Database;

namespace SQL {
enum class ScalarSqlFn : std::int16_t;
}

namespace MathSci {

struct BuiltinArity {
	int Min = 0;
	int Max = 0;
};

struct BuiltinSpec {
	AstralDB::SQL::ScalarSqlFn Fn = static_cast<AstralDB::SQL::ScalarSqlFn>(0);
	BuiltinArity Arity{};
};

bool IsMathSciScalarFn(AstralDB::SQL::ScalarSqlFn Fn);
BuiltinArity ArityFor(AstralDB::SQL::ScalarSqlFn Fn);
const char *SqlNameFor(AstralDB::SQL::ScalarSqlFn Fn);

std::optional<BuiltinSpec> LookupBuiltin(std::string_view Name);

std::optional<std::string> EvalScalar(AstralDB::SQL::ScalarSqlFn Fn, const std::vector<std::string> &Cells,
                                     Database *Db = nullptr);

} // namespace MathSci
} // namespace AstralDB
