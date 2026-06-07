#pragma once

#include <DS/Geometry2D.hxx>
#include <optional>
#include <string_view>

namespace AstralDB {
namespace MathSci {

std::optional<DS::Geometry2D::Polygon> ParseGeomPoly(std::string_view Cell);
std::optional<DS::Geometry2D::Vec2> ParseGeomPoint(std::string_view Cell);

} // namespace MathSci
} // namespace AstralDB
