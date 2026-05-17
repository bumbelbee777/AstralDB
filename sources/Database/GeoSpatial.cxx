#include <Database/GeoSpatial.hxx>

#include <cmath>
#include <cctype>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace AstralDB {
namespace GeoSpatial {
namespace {

std::string Trim(std::string_view V) {
	size_t L = 0;
	size_t R = V.size();
	while(L < R && std::isspace(static_cast<unsigned char>(V[L])))
		++L;
	while(R > L && std::isspace(static_cast<unsigned char>(V[R - 1])))
		--R;
	return std::string(V.substr(L, R - L));
}

std::optional<double> ParseDouble(std::string_view V) {
	try {
		return std::stod(std::string(V));
	} catch(...) {
		return std::nullopt;
	}
}

bool StartsWith(std::string_view S, std::string_view P) {
	return S.size() >= P.size() && S.substr(0, P.size()) == P;
}

} // namespace

std::optional<Point> ParsePointCell(std::string_view Cell) {
	const std::string T = Trim(Cell);
	if(T.size() < 5 || T[0] != 'G' || T[1] != '(')
		return std::nullopt;
	const size_t Close = T.rfind(')');
	if(Close == std::string::npos || Close <= 2)
		return std::nullopt;
	const std::string Inner = T.substr(2, Close - 2);
	const size_t Comma = Inner.find(',');
	if(Comma == std::string::npos)
		return std::nullopt;
	const auto Lon = ParseDouble(Trim(Inner.substr(0, Comma)));
	const auto Lat = ParseDouble(Trim(Inner.substr(Comma + 1)));
	if(!Lon || !Lat)
		return std::nullopt;
	return Point{*Lon, *Lat};
}

std::optional<Point> ParseWktPoint(std::string_view Wkt) {
	std::string U = Trim(Wkt);
	for(char &C : U)
		C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
	if(!StartsWith(U, "POINT(") && !StartsWith(U, "POINT ("))
		return std::nullopt;
	const size_t L = U.find('(');
	const size_t R = U.rfind(')');
	if(L == std::string::npos || R == std::string::npos || R <= L + 1)
		return std::nullopt;
	std::string Inner = U.substr(L + 1, R - L - 1);
	for(char &C : Inner) {
		if(C == ' ' || C == '\t')
			C = ',';
	}
	const size_t Comma = Inner.find(',');
	if(Comma == std::string::npos)
		return std::nullopt;
	const auto Lon = ParseDouble(Trim(Inner.substr(0, Comma)));
	const auto Lat = ParseDouble(Trim(Inner.substr(Comma + 1)));
	if(!Lon || !Lat)
		return std::nullopt;
	return Point{*Lon, *Lat};
}

std::string FormatPointCell(double Lon, double Lat) {
	return "G(" + std::to_string(Lon) + "," + std::to_string(Lat) + ")";
}

std::string FormatWktPoint(const Point &P) {
	return "POINT(" + std::to_string(P.Lon) + " " + std::to_string(P.Lat) + ")";
}

double EuclideanDistance(const Point &A, const Point &B) {
	const double Dx = A.Lon - B.Lon;
	const double Dy = A.Lat - B.Lat;
	return std::sqrt(Dx * Dx + Dy * Dy);
}

double HaversineMeters(const Point &A, const Point &B) {
	const double Phi1 = A.Lat * (M_PI / 180.0);
	const double Phi2 = B.Lat * (M_PI / 180.0);
	const double DPhi = (B.Lat - A.Lat) * (M_PI / 180.0);
	const double DLambda = (B.Lon - A.Lon) * (M_PI / 180.0);
	const double SinDPhi = std::sin(DPhi * 0.5);
	const double SinDLambda = std::sin(DLambda * 0.5);
	const double H = SinDPhi * SinDPhi + std::cos(Phi1) * std::cos(Phi2) * SinDLambda * SinDLambda;
	return 2.0 * EarthRadiusMeters * std::asin(std::sqrt(H));
}

bool WithinBbox(const Point &P, double MinLon, double MinLat, double MaxLon, double MaxLat) {
	return P.Lon >= MinLon && P.Lon <= MaxLon && P.Lat >= MinLat && P.Lat <= MaxLat;
}

std::optional<TerrainPoint> ParseTerrainCell(std::string_view Cell) {
	const std::string T = Trim(Cell);
	if(T.size() < 7 || T[0] != 'Z' || T[1] != '(')
		return std::nullopt;
	const size_t Close = T.rfind(')');
	if(Close == std::string::npos || Close <= 2)
		return std::nullopt;
	const auto Parts = [&]() {
		std::vector<std::string> Out;
		std::string Cur;
		int Depth = 0;
		for(char C : T.substr(2, Close - 2)) {
			if(C == '(')
				++Depth;
			else if(C == ')')
				--Depth;
			if(C == ',' && Depth == 0) {
				Out.push_back(Trim(Cur));
				Cur.clear();
			} else
				Cur.push_back(C);
		}
		if(!Cur.empty())
			Out.push_back(Trim(Cur));
		return Out;
	}();
	if(Parts.size() != 3)
		return std::nullopt;
	const auto Lon = ParseDouble(Parts[0]);
	const auto Lat = ParseDouble(Parts[1]);
	const auto Elev = ParseDouble(Parts[2]);
	if(!Lon || !Lat || !Elev)
		return std::nullopt;
	return TerrainPoint{*Lon, *Lat, *Elev};
}

std::optional<TerrainPoint> ParseWktPointZ(std::string_view Wkt) {
	std::string U = Trim(Wkt);
	for(char &C : U)
		C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
	if(!StartsWith(U, "POINT Z(") && !StartsWith(U, "POINTZ("))
		return std::nullopt;
	const size_t L = U.find('(');
	const size_t R = U.rfind(')');
	if(L == std::string::npos || R == std::string::npos || R <= L + 1)
		return std::nullopt;
	std::string Inner = U.substr(L + 1, R - L - 1);
	for(char &C : Inner) {
		if(C == ' ' || C == '\t')
			C = ',';
	}
	std::vector<std::string> Parts;
	std::string Cur;
	for(char C : Inner) {
		if(C == ',') {
			Parts.push_back(Trim(Cur));
			Cur.clear();
		} else
			Cur.push_back(C);
	}
	if(!Cur.empty())
		Parts.push_back(Trim(Cur));
	if(Parts.size() != 3)
		return std::nullopt;
	const auto Lon = ParseDouble(Parts[0]);
	const auto Lat = ParseDouble(Parts[1]);
	const auto Elev = ParseDouble(Parts[2]);
	if(!Lon || !Lat || !Elev)
		return std::nullopt;
	return TerrainPoint{*Lon, *Lat, *Elev};
}

std::string FormatTerrainCell(double Lon, double Lat, double ElevM) {
	return "Z(" + std::to_string(Lon) + "," + std::to_string(Lat) + "," + std::to_string(ElevM) + ")";
}

std::string FormatWktPointZ(const TerrainPoint &P) {
	return "POINT Z(" + std::to_string(P.Lon) + " " + std::to_string(P.Lat) + " " + std::to_string(P.ElevM) + ")";
}

namespace {

double Lerp1d(double A, double B, double T) { return A + (B - A) * T; }

std::optional<std::pair<std::size_t, std::size_t>> DemGridIndex(std::size_t Rows, std::size_t Cols, double MinLon,
                                                                double MinLat, double MaxLon, double MaxLat,
                                                                double Lon, double Lat) {
	if(Rows < 2 || Cols < 2 || Lon < MinLon || Lon > MaxLon || Lat < MinLat || Lat > MaxLat)
		return std::nullopt;
	const double U = (Lon - MinLon) / (MaxLon - MinLon);
	const double V = (Lat - MinLat) / (MaxLat - MinLat);
	const auto Col = static_cast<std::size_t>(U * static_cast<double>(Cols - 1));
	const auto Row = static_cast<std::size_t>(V * static_cast<double>(Rows - 1));
	if(Col >= Cols - 1 || Row >= Rows - 1)
		return std::nullopt;
	return std::pair{Row, Col};
}

} // namespace

std::optional<double> SampleDemBilinear(const std::vector<double> &Dem, std::size_t Rows, std::size_t Cols,
                                        double MinLon, double MinLat, double MaxLon, double MaxLat, double Lon,
                                        double Lat) {
	if(Dem.size() != Rows * Cols || Rows < 2 || Cols < 2)
		return std::nullopt;
	if(Lon < MinLon || Lon > MaxLon || Lat < MinLat || Lat > MaxLat)
		return std::nullopt;
	const double U = (Lon - MinLon) / (MaxLon - MinLon);
	const double V = (Lat - MinLat) / (MaxLat - MinLat);
	const auto C0 = static_cast<std::size_t>(U * static_cast<double>(Cols - 1));
	const auto R0 = static_cast<std::size_t>(V * static_cast<double>(Rows - 1));
	const auto C1 = std::min(C0 + 1, Cols - 1);
	const auto R1 = std::min(R0 + 1, Rows - 1);
	const double Fu = U * static_cast<double>(Cols - 1) - static_cast<double>(C0);
	const double Fv = V * static_cast<double>(Rows - 1) - static_cast<double>(R0);
	const double Z00 = Dem[R0 * Cols + C0];
	const double Z01 = Dem[R0 * Cols + C1];
	const double Z10 = Dem[R1 * Cols + C0];
	const double Z11 = Dem[R1 * Cols + C1];
	const double Z0 = Lerp1d(Z00, Z01, Fu);
	const double Z1 = Lerp1d(Z10, Z11, Fu);
	return Lerp1d(Z0, Z1, Fv);
}

std::optional<double> TerrainSlopeDegrees(const std::vector<double> &Dem, std::size_t Rows, std::size_t Cols,
                                          double MinLon, double MinLat, double MaxLon, double MaxLat, double Lon,
                                          double Lat) {
	const auto Idx = DemGridIndex(Rows, Cols, MinLon, MinLat, MaxLon, MaxLat, Lon, Lat);
	if(!Idx)
		return std::nullopt;
	const auto [R, C] = *Idx;
	const double Zc = Dem[R * Cols + C];
	const double Zr = Dem[std::min(R + 1, Rows - 1) * Cols + C];
	const double Zl = Dem[R * Cols + std::min(C + 1, Cols - 1)];
	const double Dx = (MaxLon - MinLon) / static_cast<double>(Cols - 1) * 111'320.0 * std::cos(Lat * M_PI / 180.0);
	const double Dy = (MaxLat - MinLat) / static_cast<double>(Rows - 1) * 110'540.0;
	if(Dx <= 0.0 || Dy <= 0.0)
		return std::nullopt;
	const double Slope = std::sqrt(std::pow((Zr - Zc) / Dy, 2) + std::pow((Zl - Zc) / Dx, 2));
	return std::atan(Slope) * (180.0 / M_PI);
}

} // namespace GeoSpatial
} // namespace AstralDB
