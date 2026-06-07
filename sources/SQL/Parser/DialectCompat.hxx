#pragma once

#include <Database/Dialect/DualTable.hxx>
#include <Database/Text/PatternMatch.hxx>
#include <optional>
#include <string>
#include <string_view>

namespace AstralDB::SQL {

using AstralDB::IsDialectDualTable;
using AstralDB::kDialectDualInternal;

/** SELECT-list placeholder prefix for \c COLUMNS(...) expansion (\c __astral_columns__:N). */
inline constexpr std::string_view kColumnsExpandPrefix = "__astral_columns__:";

/** Fold ASCII letters to upper case (in place). */
void FoldAsciiUpper(std::string &S);

using AstralDB::SqlLikeAscii;
using AstralDB::SqlLikeAsciiCaseInsensitive;
using AstralDB::SqlGlobMatch;
using AstralDB::ColumnNameGlobMatch;
using AstralDB::SqlRegexpMatch;
using AstralDB::SqlRegexpExtract;

} // namespace AstralDB::SQL
