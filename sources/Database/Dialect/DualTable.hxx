#pragma once

#include <string_view>

namespace AstralDB {

/** Oracle \c DUAL and internal scratch name for a one-row, zero-column relation. */
inline constexpr std::string_view kDialectDualInternal = "__astral_dual__";

bool IsDialectDualTable(std::string_view Name);

} // namespace AstralDB
