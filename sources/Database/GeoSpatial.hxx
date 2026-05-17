#pragma once



#include <optional>

#include <string>

#include <string_view>

#include <vector>



namespace AstralDB {

namespace GeoSpatial {



struct Point {

	double Lon = 0.0;

	double Lat = 0.0;

};



/** 3D point with terrain elevation in meters (WGS84 horizontal). */

struct TerrainPoint {

	double Lon = 0.0;

	double Lat = 0.0;

	double ElevM = 0.0;

};



constexpr double EarthRadiusMeters = 6'371'000.0;



std::optional<Point> ParsePointCell(std::string_view Cell);

std::optional<Point> ParseWktPoint(std::string_view Wkt);

std::optional<TerrainPoint> ParseTerrainCell(std::string_view Cell);

std::optional<TerrainPoint> ParseWktPointZ(std::string_view Wkt);

std::string FormatPointCell(double Lon, double Lat);

std::string FormatTerrainCell(double Lon, double Lat, double ElevM);

std::string FormatWktPoint(const Point &P);

std::string FormatWktPointZ(const TerrainPoint &P);



double EuclideanDistance(const Point &A, const Point &B);

double HaversineMeters(const Point &A, const Point &B);

bool WithinBbox(const Point &P, double MinLon, double MinLat, double MaxLon, double MaxLat);



/** Bilinear DEM sample: \p Dem is row-major elevations, \p Rows x \p Cols grid over the bbox. */

std::optional<double> SampleDemBilinear(const std::vector<double> &Dem, std::size_t Rows, std::size_t Cols,

                                        double MinLon, double MinLat, double MaxLon, double MaxLat, double Lon,

                                        double Lat);

/** Approximate terrain slope (degrees) from four neighboring DEM cells. */

std::optional<double> TerrainSlopeDegrees(const std::vector<double> &Dem, std::size_t Rows, std::size_t Cols,

                                          double MinLon, double MinLat, double MaxLon, double MaxLat, double Lon,

                                          double Lat);



} // namespace GeoSpatial

} // namespace AstralDB

