#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {
namespace DS {
namespace Geometry2D {

/** Maximum vertices per ring (outer or hole). */
constexpr std::size_t kMaxRingVertices = 65'536;

struct Vec2 {
	double X = 0.0;
	double Y = 0.0;
};

struct Ring {
	std::vector<Vec2> Vertices;
};

struct Polygon {
	Ring Outer;
	std::vector<Ring> Holes;
};

struct Bbox2D {
	double MinX = 0.0, MinY = 0.0, MaxX = 0.0, MaxY = 0.0;
	bool Valid = false;
};

struct ValidationReport {
	bool Ok = false;
	std::string Reason;
};

bool IsFiniteCoord(double V);
bool RingWithinLimits(const Ring &R);
bool PolygonWithinLimits(const Polygon &P);

std::optional<Polygon> ParsePolygonCell(std::string_view Cell);
std::string FormatPolygonCell(const Polygon &P);

std::optional<Polygon> ParseWktPolygon(std::string_view Wkt);
std::string FormatWktPolygon(const Polygon &P);

std::optional<Polygon> ImportGeoJsonPolygon(std::string_view JsonText);
std::string ExportGeoJsonPolygon(const Polygon &P);

Polygon PolygonFromRing(const std::vector<double> &FlatCoords);
Bbox2D ComputeBbox(const Polygon &P);

/** Remove duplicate/collinear vertices; enforce ring orientation (CCW outer, CW holes). */
Ring RepairRing(Ring R, bool WantCcw);
Polygon RepairPolygon(Polygon P);
ValidationReport ValidatePolygon(const Polygon &P);
/** Parse-tolerant canonical form: repair + validate (returns nullopt if irreparable). */
std::optional<Polygon> NormalizePolygon(Polygon P);

double RingAreaSigned(const Ring &R);
double RingArea(const Ring &R);
double PolygonArea(const Polygon &P);
double RingPerimeter(const Ring &R);
double PolygonPerimeter(const Polygon &P);
Vec2 RingCentroid(const Ring &R);
Vec2 PolygonCentroid(const Polygon &P);

bool PointInRing(const Vec2 &P, const Ring &R);
bool PointInPolygon(const Vec2 &P, const Polygon &Poly);
bool RingsIntersect(const Ring &A, const Ring &B);
bool PolygonsIntersect(const Polygon &A, const Polygon &B);
bool PolygonContains(const Polygon &Outer, const Polygon &Inner);
bool PolygonWithin(const Polygon &Inner, const Polygon &Outer);
bool PolygonsOverlap(const Polygon &A, const Polygon &B);
bool PolygonsTouch(const Polygon &A, const Polygon &B);

Polygon Buffer(const Polygon &P, double Distance, int Segments = 8);
Polygon Simplify(const Polygon &P, double Tolerance);
Polygon ConvexHull(const Polygon &P);
Polygon ConvexHullPoints(const std::vector<Vec2> &Pts);

struct Tri2 {
	Vec2 A, B, C;
};

/** Ear-clip triangulation (outer CCW; holes emit reversed winding). */
std::vector<Tri2> Triangulate(const Polygon &P);

Polygon Union(const Polygon &A, const Polygon &B);
Polygon Intersection(const Polygon &A, const Polygon &B);
Polygon Difference(const Polygon &A, const Polygon &B);
Polygon SymDifference(const Polygon &A, const Polygon &B);

} // namespace Geometry2D
} // namespace DS
} // namespace AstralDB
