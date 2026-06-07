#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace AstralDB {

bool SqlLikeAscii(const std::string &Str, const std::string &Pat);
bool SqlLikeAsciiCaseInsensitive(const std::string &Str, const std::string &Pat);

/** SQLite \c GLOB: \c * and \c ? wildcards (not SQL \c LIKE \c % / \c _ ). */
bool SqlGlobMatch(const std::string &Str, const std::string &Pat);

/** Column-name filter for \c COLUMNS('pat') (glob on identifiers). */
bool ColumnNameGlobMatch(std::string_view Name, std::string_view Pat);

/** POSIX-style regex search (\c REGEXP / PostgreSQL \c ~ ). */
bool SqlRegexpMatch(const std::string &Str, const std::string &Pat, bool CaseInsensitive = false);

/** ISO SQL \c REGEXP_EXTRACT — first match, optional capturing group (default 1). */
std::optional<std::string> SqlRegexpExtract(const std::string &Str, const std::string &Pat, int GroupIndex = 1,
                                            bool CaseInsensitive = false);

} // namespace AstralDB
