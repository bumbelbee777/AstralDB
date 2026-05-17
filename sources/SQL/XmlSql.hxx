#pragma once

#include <DS/XML.hxx>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace AstralDB {
namespace SQL {
namespace XmlSql {

std::shared_ptr<DS::XmlNode> ParseCellXml(std::string_view Cell);
std::optional<std::string> ExtractPath(const DS::XmlNode &Root, std::string_view Path);
std::string SerializeCell(const DS::XmlNode &Root);
bool IsValidXml(std::string_view Cell);

} // namespace XmlSql
} // namespace SQL
} // namespace AstralDB
