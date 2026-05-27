#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {
namespace DS {
namespace glTF {

constexpr std::size_t kMaxMeshVertexCount = 2'000'000;
constexpr std::size_t kMaxMeshIndexCount = 6'000'000;

struct Mesh {
	std::vector<double> Vertices;
	std::vector<std::uint32_t> Indices;
};

struct MeshBounds {
	double MinX = 0.0, MinY = 0.0, MinZ = 0.0;
	double MaxX = 0.0, MaxY = 0.0, MaxZ = 0.0;
	bool Valid = false;
};

struct MeshValidationReport {
	bool Ok = false;
	std::string Reason;
};

bool IsFiniteCoord(double V);
bool MeshWithinLimits(const Mesh &M);

std::optional<Mesh> ParseMeshCell(std::string_view Cell);
std::string FormatMeshCell(const Mesh &M);

std::optional<Mesh> ImportMeshFromGltfJson(std::string_view JsonText);
std::string ExportMeshToGltfJson(const Mesh &M);

MeshValidationReport ValidateMesh(const Mesh &M);
Mesh RemoveDegenerateTriangles(const Mesh &M);
Mesh RepairMesh(const Mesh &M);
std::optional<Mesh> NormalizeMesh(Mesh M);

MeshBounds ComputeBounds(const Mesh &M);
double SurfaceArea(const Mesh &M);
double Volume(const Mesh &M);
void Centroid(const Mesh &M, double &OutX, double &OutY, double &OutZ);

Mesh Translate(const Mesh &M, double Dx, double Dy, double Dz);
Mesh Scale(const Mesh &M, double Sx, double Sy, double Sz);
Mesh Rotate(const Mesh &M, double DegX, double DegY, double DegZ);

Mesh SewMesh(const Mesh &M, double Epsilon = 1e-6);
Mesh MergeMeshes(const Mesh &A, const Mesh &B, double SewEpsilon = 1e-6);
Mesh CsgUnion(const Mesh &A, const Mesh &B);
Mesh CsgIntersection(const Mesh &A, const Mesh &B);
Mesh CsgDifference(const Mesh &A, const Mesh &B);

} // namespace glTF
} // namespace DS
} // namespace AstralDB
