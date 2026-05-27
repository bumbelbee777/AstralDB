#include <DS/glTF.hxx>

#include <DS/JSON.hxx>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace AstralDB {
namespace DS {
namespace glTF {

double SurfaceArea(const Mesh &M);
Mesh SewMesh(const Mesh &M, double Epsilon);

namespace {

constexpr double kEps = 1e-12;
constexpr double kDegenerateArea = 1e-14;

bool MeshShapeValid(const Mesh &M) {
	return !M.Vertices.empty() && M.Vertices.size() % 3 == 0 && !M.Indices.empty() && M.Indices.size() % 3 == 0;
}

bool CoordOk(double V) { return std::isfinite(V); }

bool IndicesInRange(const Mesh &M) {
	const std::uint32_t VertCount = static_cast<std::uint32_t>(M.Vertices.size() / 3);
	for(auto Idx : M.Indices) {
		if(Idx >= VertCount)
			return false;
	}
	return true;
}

MeshBounds ComputeBbox(const Mesh &M) {
	MeshBounds B;
	if(!MeshShapeValid(M))
		return B;
	B.MinX = B.MaxX = M.Vertices[0];
	B.MinY = B.MaxY = M.Vertices[1];
	B.MinZ = B.MaxZ = M.Vertices[2];
	for(size_t I = 3; I < M.Vertices.size(); I += 3) {
		B.MinX = std::min(B.MinX, M.Vertices[I]);
		B.MinY = std::min(B.MinY, M.Vertices[I + 1]);
		B.MinZ = std::min(B.MinZ, M.Vertices[I + 2]);
		B.MaxX = std::max(B.MaxX, M.Vertices[I]);
		B.MaxY = std::max(B.MaxY, M.Vertices[I + 1]);
		B.MaxZ = std::max(B.MaxZ, M.Vertices[I + 2]);
	}
	B.Valid = true;
	return B;
}

bool BboxOverlaps(const MeshBounds &A, const MeshBounds &B) {
	if(!A.Valid || !B.Valid)
		return false;
	return !(A.MaxX < B.MinX || A.MinX > B.MaxX || A.MaxY < B.MinY || A.MinY > B.MaxY || A.MaxZ < B.MinZ
	         || A.MinZ > B.MaxZ);
}

bool BboxContains(const MeshBounds &Outer, const MeshBounds &Inner) {
	if(!Outer.Valid || !Inner.Valid)
		return false;
	return Outer.MinX <= Inner.MinX && Outer.MinY <= Inner.MinY && Outer.MinZ <= Inner.MinZ
	    && Outer.MaxX >= Inner.MaxX && Outer.MaxY >= Inner.MaxY && Outer.MaxZ >= Inner.MaxZ;
}

void TriVertex(const Mesh &M, std::uint32_t Idx, double &X, double &Y, double &Z) {
	const size_t B = static_cast<size_t>(Idx) * 3;
	if(B + 2 < M.Vertices.size()) {
		X = M.Vertices[B];
		Y = M.Vertices[B + 1];
		Z = M.Vertices[B + 2];
	}
}

double TriArea3(const double X0, const double Y0, const double Z0, const double X1, const double Y1, const double Z1,
                const double X2, const double Y2, const double Z2) {
	const double Ax = X1 - X0, Ay = Y1 - Y0, Az = Z1 - Z0;
	const double Bx = X2 - X0, By = Y2 - Y0, Bz = Z2 - Z0;
	const double Cx = Ay * Bz - Az * By;
	const double Cy = Az * Bx - Ax * Bz;
	const double Cz = Ax * By - Ay * Bx;
	return 0.5 * std::sqrt(Cx * Cx + Cy * Cy + Cz * Cz);
}

bool RayTriHit(const double Ox, const double Oy, const double Oz, const double Dx, const double Dy, const double Dz,
               const double X0, const double Y0, const double Z0, const double X1, const double Y1, const double Z1,
               const double X2, const double Y2, const double Z2) {
	const double E1x = X1 - X0, E1y = Y1 - Y0, E1z = Z1 - Z0;
	const double E2x = X2 - X0, E2y = Y2 - Y0, E2z = Z2 - Z0;
	const double Px = Dy * E2z - Dz * E2y;
	const double Py = Dz * E2x - Dx * E2z;
	const double Pz = Dx * E2y - Dy * E2x;
	const double Det = E1x * Px + E1y * Py + E1z * Pz;
	if(std::abs(Det) < kEps)
		return false;
	const double Inv = 1.0 / Det;
	const double Tx = Ox - X0, Ty = Oy - Y0, Tz = Oz - Z0;
	const double U = (Tx * Px + Ty * Py + Tz * Pz) * Inv;
	if(U < 0.0 || U > 1.0)
		return false;
	const double Qx = Ty * E1z - Tz * E1y;
	const double Qy = Tz * E1x - Tx * E1z;
	const double Qz = Tx * E1y - Ty * E1x;
	const double V = (Dx * Qx + Dy * Qy + Dz * Qz) * Inv;
	if(V < 0.0 || U + V > 1.0)
		return false;
	const double T = (E2x * Qx + E2y * Qy + E2z * Qz) * Inv;
	return T > kEps;
}

int RayHits(const Mesh &M, double Px, double Py, double Pz, double Dx, double Dy, double Dz) {
	int Hits = 0;
	for(size_t I = 0; I + 2 < M.Indices.size(); I += 3) {
		double X0, Y0, Z0, X1, Y1, Z1, X2, Y2, Z2;
		TriVertex(M, M.Indices[I], X0, Y0, Z0);
		TriVertex(M, M.Indices[I + 1], X1, Y1, Z1);
		TriVertex(M, M.Indices[I + 2], X2, Y2, Z2);
		if(RayTriHit(Px, Py, Pz, Dx, Dy, Dz, X0, Y0, Z0, X1, Y1, Z1, X2, Y2, Z2))
			++Hits;
	}
	return Hits;
}

double SolidAngleTri(double Px, double Py, double Pz, double X0, double Y0, double Z0, double X1, double Y1,
                     double Z1, double X2, double Y2, double Z2) {
	const double A0 = X0 - Px, A1 = Y0 - Py, A2 = Z0 - Pz;
	const double B0 = X1 - Px, B1 = Y1 - Py, B2 = Z1 - Pz;
	const double C0 = X2 - Px, C1 = Y2 - Py, C2 = Z2 - Pz;
	const double La = std::sqrt(A0 * A0 + A1 * A1 + A2 * A2);
	const double Lb = std::sqrt(B0 * B0 + B1 * B1 + B2 * B2);
	const double Lc = std::sqrt(C0 * C0 + C1 * C1 + C2 * C2);
	if(La < kEps || Lb < kEps || Lc < kEps)
		return 0.0;
	const double Num = A0 * (B1 * C2 - B2 * C1) + A1 * (B2 * C0 - B0 * C2) + A2 * (B0 * C1 - B1 * C0);
	const double Den = La * Lb * Lc + A0 * B0 + A1 * B1 + A2 * B2 + A0 * C0 + A1 * C1 + A2 * C2
	                   + B0 * C0 + B1 * C1 + B2 * C2;
	return 2.0 * std::atan2(Num, Den);
}

double WindingNumber(const Mesh &M, double Px, double Py, double Pz) {
	if(!MeshShapeValid(M) || !IndicesInRange(M))
		return 0.0;
	double W = 0.0;
	for(size_t I = 0; I + 2 < M.Indices.size(); I += 3) {
		double X0, Y0, Z0, X1, Y1, Z1, X2, Y2, Z2;
		TriVertex(M, M.Indices[I], X0, Y0, Z0);
		TriVertex(M, M.Indices[I + 1], X1, Y1, Z1);
		TriVertex(M, M.Indices[I + 2], X2, Y2, Z2);
		W += SolidAngleTri(Px, Py, Pz, X0, Y0, Z0, X1, Y1, Z1, X2, Y2, Z2);
	}
	return W / (4.0 * M_PI);
}

bool PointInsideMesh(const Mesh &M, double Px, double Py, double Pz) {
	if(!MeshShapeValid(M) || !IndicesInRange(M))
		return false;
	const double W = WindingNumber(M, Px, Py, Pz);
	if(std::abs(W) >= 0.5)
		return true;
	const int Hx = RayHits(M, Px, Py, Pz, 1.0, 0.0, 0.0);
	const int Hy = RayHits(M, Px, Py, Pz, 0.0, 1.0, 0.0);
	const int Hz = RayHits(M, Px, Py, Pz, 0.0, 0.0, 1.0);
	int InsideVotes = 0;
	if((Hx % 2) == 1)
		++InsideVotes;
	if((Hy % 2) == 1)
		++InsideVotes;
	if((Hz % 2) == 1)
		++InsideVotes;
	return InsideVotes >= 2;
}

enum class TriInside : uint8_t { Outside, Inside, Straddle };

TriInside ClassifyTriangle(const Mesh &Ref, double X0, double Y0, double Z0, double X1, double Y1, double Z1,
                           double X2, double Y2, double Z2) {
	const bool I0 = PointInsideMesh(Ref, X0, Y0, Z0);
	const bool I1 = PointInsideMesh(Ref, X1, Y1, Z1);
	const bool I2 = PointInsideMesh(Ref, X2, Y2, Z2);
	if(I0 && I1 && I2)
		return TriInside::Inside;
	if(!I0 && !I1 && !I2)
		return TriInside::Outside;
	return TriInside::Straddle;
}

Mesh FilterTrianglesByInside(const Mesh &Src, const Mesh &Ref, bool KeepInside) {
	Mesh Out;
	for(size_t I = 0; I + 2 < Src.Indices.size(); I += 3) {
		double X0, Y0, Z0, X1, Y1, Z1, X2, Y2, Z2;
		TriVertex(Src, Src.Indices[I], X0, Y0, Z0);
		TriVertex(Src, Src.Indices[I + 1], X1, Y1, Z1);
		TriVertex(Src, Src.Indices[I + 2], X2, Y2, Z2);
		const double Cx = (X0 + X1 + X2) / 3.0;
		const double Cy = (Y0 + Y1 + Y2) / 3.0;
		const double Cz = (Z0 + Z1 + Z2) / 3.0;
		const auto Kind = ClassifyTriangle(Ref, X0, Y0, Z0, X1, Y1, Z1, X2, Y2, Z2);
		bool In = false;
		if(Kind == TriInside::Inside)
			In = true;
		else if(Kind == TriInside::Outside)
			In = false;
		else
			In = PointInsideMesh(Ref, Cx, Cy, Cz);
		if(In != KeepInside)
			continue;
		const std::uint32_t Base = static_cast<std::uint32_t>(Out.Vertices.size() / 3);
		Out.Vertices.push_back(X0);
		Out.Vertices.push_back(Y0);
		Out.Vertices.push_back(Z0);
		Out.Vertices.push_back(X1);
		Out.Vertices.push_back(Y1);
		Out.Vertices.push_back(Z1);
		Out.Vertices.push_back(X2);
		Out.Vertices.push_back(Y2);
		Out.Vertices.push_back(Z2);
		Out.Indices.push_back(Base);
		Out.Indices.push_back(Base + 1);
		Out.Indices.push_back(Base + 2);
	}
	return Out;
}

void RotatePoint(double &X, double &Y, double &Z, double DegX, double DegY, double DegZ) {
	const double Rx = DegX * M_PI / 180.0;
	const double Ry = DegY * M_PI / 180.0;
	const double Rz = DegZ * M_PI / 180.0;
	if(std::abs(Rx) > kEps) {
		const double C = std::cos(Rx), S = std::sin(Rx);
		const double Ny = C * Y - S * Z;
		const double Nz = S * Y + C * Z;
		Y = Ny;
		Z = Nz;
	}
	if(std::abs(Ry) > kEps) {
		const double C = std::cos(Ry), S = std::sin(Ry);
		const double Nx = C * X + S * Z;
		const double Nz = -S * X + C * Z;
		X = Nx;
		Z = Nz;
	}
	if(std::abs(Rz) > kEps) {
		const double C = std::cos(Rz), S = std::sin(Rz);
		const double Nx = C * X - S * Y;
		const double Ny = S * X + C * Y;
		X = Nx;
		Y = Ny;
	}
}

} // namespace

bool IsFiniteCoord(double V) { return std::isfinite(V); }

bool MeshWithinLimits(const Mesh &M) {
	return M.Vertices.size() / 3 <= kMaxMeshVertexCount && M.Indices.size() <= kMaxMeshIndexCount;
}

MeshValidationReport ValidateMesh(const Mesh &M) {
	MeshValidationReport R;
	if(!MeshShapeValid(M)) {
		R.Reason = "mesh must have xyz vertices and triangle indices";
		return R;
	}
	if(!MeshWithinLimits(M)) {
		R.Reason = "mesh exceeds size limits";
		return R;
	}
	for(double V : M.Vertices) {
		if(!CoordOk(V)) {
			R.Reason = "non-finite vertex coordinate";
			return R;
		}
	}
	if(!IndicesInRange(M)) {
		R.Reason = "triangle index out of range";
		return R;
	}
	if(SurfaceArea(M) < kDegenerateArea) {
		R.Reason = "mesh surface area is zero";
		return R;
	}
	R.Ok = true;
	return R;
}

Mesh RemoveDegenerateTriangles(const Mesh &M) {
	Mesh Out;
	if(!MeshShapeValid(M) || !IndicesInRange(M))
		return Out;
	for(size_t I = 0; I + 2 < M.Indices.size(); I += 3) {
		double X0, Y0, Z0, X1, Y1, Z1, X2, Y2, Z2;
		TriVertex(M, M.Indices[I], X0, Y0, Z0);
		TriVertex(M, M.Indices[I + 1], X1, Y1, Z1);
		TriVertex(M, M.Indices[I + 2], X2, Y2, Z2);
		if(TriArea3(X0, Y0, Z0, X1, Y1, Z1, X2, Y2, Z2) < kDegenerateArea)
			continue;
		const std::uint32_t Base = static_cast<std::uint32_t>(Out.Vertices.size() / 3);
		Out.Vertices.push_back(X0);
		Out.Vertices.push_back(Y0);
		Out.Vertices.push_back(Z0);
		Out.Vertices.push_back(X1);
		Out.Vertices.push_back(Y1);
		Out.Vertices.push_back(Z1);
		Out.Vertices.push_back(X2);
		Out.Vertices.push_back(Y2);
		Out.Vertices.push_back(Z2);
		Out.Indices.push_back(Base);
		Out.Indices.push_back(Base + 1);
		Out.Indices.push_back(Base + 2);
	}
	return Out;
}

Mesh RepairMesh(const Mesh &M) {
	Mesh Out = RemoveDegenerateTriangles(M);
	return SewMesh(Out, 1e-6);
}

std::optional<Mesh> NormalizeMesh(Mesh M) {
	if(!MeshWithinLimits(M))
		return std::nullopt;
	const auto Report = ValidateMesh(M);
	if(!Report.Ok)
		M = RepairMesh(M);
	const auto Report2 = ValidateMesh(M);
	if(!Report2.Ok)
		return std::nullopt;
	return RemoveDegenerateTriangles(SewMesh(M, 1e-6));
}

std::optional<Mesh> ParseMeshCell(std::string_view Cell) {
	if(Cell.size() < 6 || !Cell.starts_with("M3{") || Cell.back() != '}')
		return std::nullopt;
	const auto Root = TryDecodeJSON(Cell.substr(2));
	if(!Root || !Root->IsObject())
		return std::nullopt;
	const auto &Obj = Root->AsObject();
	auto ItV = Obj.find("v");
	auto ItI = Obj.find("i");
	if(ItV == Obj.end() || ItI == Obj.end() || !ItV->second.IsArray() || !ItI->second.IsArray())
		return std::nullopt;
	Mesh M;
	for(const auto &Val : ItV->second.AsArray()) {
		if(!Val.IsNumber())
			return std::nullopt;
		const double V = Val.AsNumber();
		if(!CoordOk(V))
			return std::nullopt;
		M.Vertices.push_back(V);
	}
	if(M.Vertices.size() / 3 > kMaxMeshVertexCount)
		return std::nullopt;
	for(const auto &Val : ItI->second.AsArray()) {
		if(!Val.IsNumber())
			return std::nullopt;
		const double N = Val.AsNumber();
		if(N < 0.0 || N > static_cast<double>(std::numeric_limits<std::uint32_t>::max()))
			return std::nullopt;
		M.Indices.push_back(static_cast<std::uint32_t>(N));
	}
	if(M.Indices.size() > kMaxMeshIndexCount)
		return std::nullopt;
	return NormalizeMesh(std::move(M));
}

std::string FormatMeshCell(const Mesh &M) {
	JSONObject O;
	JSONArray V;
	JSONArray I;
	for(double X : M.Vertices)
		V.emplace_back(X);
	for(auto X : M.Indices)
		I.emplace_back(static_cast<double>(X));
	O.emplace("v", JSON(std::move(V)));
	O.emplace("i", JSON(std::move(I)));
	return "M3" + SerializeJSON(JSON(std::move(O)));
}

std::optional<Mesh> ImportMeshFromGltfJson(std::string_view JsonText) {
	const auto Doc = TryDecodeJSON(JsonText);
	if(!Doc || !Doc->IsObject())
		return std::nullopt;
	const auto &Obj = Doc->AsObject();
	auto ItMeshes = Obj.find("meshes");
	if(ItMeshes == Obj.end() || !ItMeshes->second.IsArray() || ItMeshes->second.AsArray().empty())
		return std::nullopt;
	const auto &Mesh0 = ItMeshes->second.AsArray().front();
	if(!Mesh0.IsObject())
		return std::nullopt;
	const auto &MeshObj = Mesh0.AsObject();
	auto ItPrims = MeshObj.find("primitives");
	if(ItPrims == MeshObj.end() || !ItPrims->second.IsArray() || ItPrims->second.AsArray().empty())
		return std::nullopt;
	const auto &Prim0 = ItPrims->second.AsArray().front();
	if(!Prim0.IsObject())
		return std::nullopt;
	const auto &PrimObj = Prim0.AsObject();
	auto ItPos = PrimObj.find("POSITION");
	auto ItIdx = PrimObj.find("indices");
	if(ItPos == PrimObj.end() || ItIdx == PrimObj.end() || !ItPos->second.IsArray() || !ItIdx->second.IsArray())
		return std::nullopt;

	Mesh M;
	for(const auto &V3 : ItPos->second.AsArray()) {
		if(!V3.IsArray() || V3.AsArray().size() != 3)
			return std::nullopt;
		for(const auto &Comp : V3.AsArray()) {
			if(!Comp.IsNumber())
				return std::nullopt;
			M.Vertices.push_back(Comp.AsNumber());
		}
	}
	for(const auto &Idx : ItIdx->second.AsArray()) {
		if(!Idx.IsNumber())
			return std::nullopt;
		const double N = Idx.AsNumber();
		if(N < 0.0 || N > static_cast<double>(std::numeric_limits<std::uint32_t>::max()))
			return std::nullopt;
		M.Indices.push_back(static_cast<std::uint32_t>(N));
	}
	return NormalizeMesh(std::move(M));
}

std::string ExportMeshToGltfJson(const Mesh &M) {
	JSONObject Prim;
	JSONArray Pos;
	Pos.reserve(M.Vertices.size() / 3);
	for(size_t I = 0; I + 2 < M.Vertices.size(); I += 3) {
		Pos.emplace_back(JSONArray{JSON(M.Vertices[I]), JSON(M.Vertices[I + 1]), JSON(M.Vertices[I + 2])});
	}
	JSONArray Idx;
	Idx.reserve(M.Indices.size());
	for(auto I : M.Indices)
		Idx.emplace_back(static_cast<double>(I));
	Prim.emplace("POSITION", JSON(std::move(Pos)));
	Prim.emplace("indices", JSON(std::move(Idx)));
	Prim.emplace("mode", JSON(4.0));

	JSONObject Mesh0;
	Mesh0.emplace("primitives", JSON(JSONArray{JSON(std::move(Prim))}));

	JSONObject Root;
	Root.emplace("asset", JSON(JSONObject{{"version", JSON("2.0")}}));
	Root.emplace("meshes", JSON(JSONArray{JSON(std::move(Mesh0))}));
	return SerializeJSON(JSON(std::move(Root)));
}

MeshBounds ComputeBounds(const Mesh &M) { return ComputeBbox(M); }

double SurfaceArea(const Mesh &M) {
	if(!MeshShapeValid(M))
		return 0.0;
	double A = 0.0;
	for(size_t I = 0; I + 2 < M.Indices.size(); I += 3) {
		double X0, Y0, Z0, X1, Y1, Z1, X2, Y2, Z2;
		TriVertex(M, M.Indices[I], X0, Y0, Z0);
		TriVertex(M, M.Indices[I + 1], X1, Y1, Z1);
		TriVertex(M, M.Indices[I + 2], X2, Y2, Z2);
		A += TriArea3(X0, Y0, Z0, X1, Y1, Z1, X2, Y2, Z2);
	}
	return A;
}

double Volume(const Mesh &M) {
	if(!MeshShapeValid(M))
		return 0.0;
	double Vol = 0.0;
	for(size_t I = 0; I + 2 < M.Indices.size(); I += 3) {
		double X0, Y0, Z0, X1, Y1, Z1, X2, Y2, Z2;
		TriVertex(M, M.Indices[I], X0, Y0, Z0);
		TriVertex(M, M.Indices[I + 1], X1, Y1, Z1);
		TriVertex(M, M.Indices[I + 2], X2, Y2, Z2);
		Vol += X0 * (Y1 * Z2 - Z1 * Y2) + X1 * (Y2 * Z0 - Z2 * Y0) + X2 * (Y0 * Z1 - Z0 * Y1);
	}
	return std::abs(Vol) / 6.0;
}

void Centroid(const Mesh &M, double &OutX, double &OutY, double &OutZ) {
	OutX = OutY = OutZ = 0.0;
	if(!MeshShapeValid(M))
		return;
	double Total = 0.0;
	for(size_t I = 0; I + 2 < M.Indices.size(); I += 3) {
		double X0, Y0, Z0, X1, Y1, Z1, X2, Y2, Z2;
		TriVertex(M, M.Indices[I], X0, Y0, Z0);
		TriVertex(M, M.Indices[I + 1], X1, Y1, Z1);
		TriVertex(M, M.Indices[I + 2], X2, Y2, Z2);
		const double A = TriArea3(X0, Y0, Z0, X1, Y1, Z1, X2, Y2, Z2);
		OutX += A * (X0 + X1 + X2) / 3.0;
		OutY += A * (Y0 + Y1 + Y2) / 3.0;
		OutZ += A * (Z0 + Z1 + Z2) / 3.0;
		Total += A;
	}
	if(Total > kEps) {
		OutX /= Total;
		OutY /= Total;
		OutZ /= Total;
	}
}

Mesh Translate(const Mesh &M, double Dx, double Dy, double Dz) {
	Mesh Out = M;
	for(size_t I = 0; I < Out.Vertices.size(); I += 3) {
		Out.Vertices[I] += Dx;
		Out.Vertices[I + 1] += Dy;
		Out.Vertices[I + 2] += Dz;
	}
	return Out;
}

Mesh Scale(const Mesh &M, double Sx, double Sy, double Sz) {
	Mesh Out = M;
	for(size_t I = 0; I < Out.Vertices.size(); I += 3) {
		Out.Vertices[I] *= Sx;
		Out.Vertices[I + 1] *= Sy;
		Out.Vertices[I + 2] *= Sz;
	}
	return Out;
}

Mesh Rotate(const Mesh &M, double DegX, double DegY, double DegZ) {
	Mesh Out = M;
	for(size_t I = 0; I < Out.Vertices.size(); I += 3)
		RotatePoint(Out.Vertices[I], Out.Vertices[I + 1], Out.Vertices[I + 2], DegX, DegY, DegZ);
	return Out;
}

Mesh SewMesh(const Mesh &M, double Epsilon) {
	if(!MeshShapeValid(M))
		return M;
	if(Epsilon <= 0.0)
		Epsilon = 1e-6;
	Mesh Out;
	Out.Indices.reserve(M.Indices.size());
	std::unordered_map<std::string, std::uint32_t> Dedup;
	const auto Q = [Epsilon](double V) {
		return static_cast<long long>(std::llround(V / Epsilon));
	};
	for(auto Idx : M.Indices) {
		const size_t B = static_cast<size_t>(Idx) * 3;
		if(B + 2 >= M.Vertices.size())
			continue;
		const double X = M.Vertices[B], Y = M.Vertices[B + 1], Z = M.Vertices[B + 2];
		const std::string Key = std::to_string(Q(X)) + "|" + std::to_string(Q(Y)) + "|" + std::to_string(Q(Z));
		auto It = Dedup.find(Key);
		if(It == Dedup.end()) {
			const std::uint32_t NewIdx = static_cast<std::uint32_t>(Out.Vertices.size() / 3);
			Out.Vertices.push_back(X);
			Out.Vertices.push_back(Y);
			Out.Vertices.push_back(Z);
			Dedup.emplace(Key, NewIdx);
			Out.Indices.push_back(NewIdx);
		} else
			Out.Indices.push_back(It->second);
	}
	return Out;
}

Mesh MergeMeshes(const Mesh &A, const Mesh &B, double SewEpsilon) {
	return SewMesh(CsgUnion(A, B), SewEpsilon);
}

Mesh CsgUnion(const Mesh &A, const Mesh &B) {
	auto Na = NormalizeMesh(A);
	auto Nb = NormalizeMesh(B);
	if(!Na && !Nb)
		return Mesh{};
	if(!Na)
		return *Nb;
	if(!Nb)
		return *Na;
	Mesh Out = *Na;
	const std::uint32_t Base = static_cast<std::uint32_t>(Out.Vertices.size() / 3);
	Out.Vertices.insert(Out.Vertices.end(), Nb->Vertices.begin(), Nb->Vertices.end());
	Out.Indices.reserve(Out.Indices.size() + Nb->Indices.size());
	for(auto I : Nb->Indices)
		Out.Indices.push_back(Base + I);
	if(auto N = NormalizeMesh(std::move(Out)))
		return *N;
	return Mesh{};
}

Mesh CsgIntersection(const Mesh &A, const Mesh &B) {
	auto Na = NormalizeMesh(A);
	auto Nb = NormalizeMesh(B);
	if(!Na || !Nb)
		return Mesh{};
	const auto BA = ComputeBbox(*Na);
	const auto BB = ComputeBbox(*Nb);
	if(!BboxOverlaps(BA, BB))
		return Mesh{};
	Mesh Out = FilterTrianglesByInside(*Na, *Nb, true);
	if(Out.Indices.empty())
		Out = FilterTrianglesByInside(*Nb, *Na, true);
	if(auto N = NormalizeMesh(std::move(Out)))
		return *N;
	return Mesh{};
}

Mesh CsgDifference(const Mesh &A, const Mesh &B) {
	auto Na = NormalizeMesh(A);
	auto Nb = NormalizeMesh(B);
	if(!Na)
		return Mesh{};
	if(!Nb)
		return *Na;
	const auto BA = ComputeBbox(*Na);
	const auto BB = ComputeBbox(*Nb);
	if(BboxContains(BB, BA))
		return Mesh{};
	Mesh Out = FilterTrianglesByInside(*Na, *Nb, false);
	if(auto N = NormalizeMesh(std::move(Out)))
		return *N;
	return Mesh{};
}

} // namespace glTF
} // namespace DS
} // namespace AstralDB
