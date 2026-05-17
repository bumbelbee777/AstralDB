#pragma once

#include <DS/JSON.hxx>
#include <optional>
#include <string>
#include <string_view>

namespace AstralDB {
namespace SQL {
namespace JsonSql {

std::optional<DS::JSON> ParseCellJson(std::string_view Cell);
std::optional<DS::JSON> ExtractPath(const DS::JSON &Root, std::string_view Path);
std::string JsonCellToText(const DS::JSON &J);

} // namespace JsonSql
} // namespace SQL
} // namespace AstralDB
