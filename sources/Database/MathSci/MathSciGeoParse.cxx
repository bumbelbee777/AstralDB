#include <Database/MathSci/MathSciGeoParse.hxx>

#include <Database/Graph/GeoSpatial.hxx>
#include <Database/Types/AdvancedTypes.hxx>
#include <DS/Geometry2D.hxx>

namespace AstralDB {
namespace MathSci {

std::optional<DS::Geometry2D::Polygon> ParseGeomPoly(std::string_view Cell) {
	if(const auto P = DS::Geometry2D::ParsePolygonCell(Cell))
		return P;
	if(const auto W = DS::Geometry2D::ParseWktPolygon(Cell))
		return W;
	if(const auto G = DS::Geometry2D::ImportGeoJsonPolygon(Cell))
		return G;
	if(Cell.starts_with("POLYGON"))
		return DS::Geometry2D::ParseWktPolygon(Cell);
	return std::nullopt;
}

std::optional<DS::Geometry2D::Vec2> ParseGeomPoint(std::string_view Cell) {
	if(const auto P = GeoSpatial::ParsePointCell(Cell))
		return DS::Geometry2D::Vec2{P->Lon, P->Lat};
	if(const auto W = GeoSpatial::ParseWktPoint(Cell))
		return DS::Geometry2D::Vec2{W->Lon, W->Lat};
	if(const auto V = AdvancedTypes::ParseVectorCell(Cell)) {
		if(V->size() >= 2)
			return DS::Geometry2D::Vec2{(*V)[0], (*V)[1]};
	}
	return std::nullopt;
}

} // namespace MathSci
} // namespace AstralDB
