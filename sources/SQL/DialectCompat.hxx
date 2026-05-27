#pragma once

#include <string>
#include <string_view>

namespace AstralDB::SQL {

/** Oracle \c DUAL and internal scratch name for a one-row, zero-column relation. */
inline constexpr std::string_view kDialectDualInternal = "__astral_dual__";

/** SELECT-list placeholder prefix for \c COLUMNS(...) expansion (\c __astral_columns__:N). */
inline constexpr std::string_view kColumnsExpandPrefix = "__astral_columns__:";

bool IsDialectDualTable(std::string_view Name);

/** Fold ASCII letters to upper case (in place). */
void FoldAsciiUpper(std::string &S);

bool SqlLikeAscii(const std::string &Str, const std::string &Pat);
bool SqlLikeAsciiCaseInsensitive(const std::string &Str, const std::string &Pat);

/** SQLite \c GLOB: \c * and \c ? wildcards (not SQL \c LIKE \c % / \c _). */
bool SqlGlobMatch(const std::string &Str, const std::string &Pat);

/** Column-name filter for \c COLUMNS('pat') (glob on identifiers). */
bool ColumnNameGlobMatch(std::string_view Name, std::string_view Pat);

/** POSIX-style regex search (\c REGEXP / PostgreSQL \c ~ ). */
bool SqlRegexpMatch(const std::string &Str, const std::string &Pat, bool CaseInsensitive = false);

} // namespace AstralDB::SQL
