#include <Database/Types/AdvancedTypes.hxx>

#include <Database/Graph/GeoSpatial.hxx>
#include <DS/glTF.hxx>
#include <DS/Geometry2D.hxx>
#include <IO/SIMD.hxx>

#include <cctype>
#include <cmath>
#include <sstream>
#include <vector>

namespace AstralDB {
namespace AdvancedTypes {
namespace {

std::string Upper(std::string_view V) {
	std::string O;
	O.reserve(V.size());
	for(unsigned char C : V)
		O.push_back(static_cast<char>(std::toupper(C)));
	return O;
}

std::string Trim(std::string_view V) {
	size_t L = 0;
	size_t R = V.size();
	while(L < R && std::isspace(static_cast<unsigned char>(V[L])))
		++L;
	while(R > L && std::isspace(static_cast<unsigned char>(V[R - 1])))
		--R;
	return std::string(V.substr(L, R - L));
}

bool StartsWith(std::string_view S, std::string_view Prefix) {
	return S.size() >= Prefix.size() && S.substr(0, Prefix.size()) == Prefix;
}

std::optional<double> ParseFloat(std::string_view V) {
	try {
		return std::stod(std::string(V));
	} catch(...) {
		return std::nullopt;
	}
}

std::vector<std::string> SplitTopLevelCommas(std::string_view Inner) {
	std::vector<std::string> Parts;
	std::string Cur;
	int Depth = 0;
	for(char C : Inner) {
		if(C == '(')
			++Depth;
		else if(C == ')')
			--Depth;
		if(C == ',' && Depth == 0) {
			Parts.push_back(Trim(Cur));
			Cur.clear();
		} else
			Cur.push_back(C);
	}
	if(!Cur.empty())
		Parts.push_back(Trim(Cur));
	return Parts;
}

std::string ParseSimpleTypeToken(std::string_view Tok) {
	const std::string U = Upper(Tok);
	if(U == "INT" || U == "INTEGER" || U == "BIGINT" || U == "SMALLINT")
		return "INTEGER";
	if(U == "BOOL")
		return "BOOLEAN";
	if(U == "DEC" || U == "NUMBER")
		return "NUMERIC";
	if(U == "DATETIME")
		return "TIMESTAMP";
	return U;
}

} // namespace

bool IsAdvancedTypeSpelling(std::string_view SqlType) {
	const std::string U = Upper(Trim(SqlType));
	if(U == "COMPLEX")
		return true;
	if(StartsWith(U, "STRUCT("))
		return true;
	if(StartsWith(U, "MAP("))
		return true;
	if(StartsWith(U, "VECTOR("))
		return true;
	if(StartsWith(U, "MATRIX("))
		return true;
	if(StartsWith(U, "LIST("))
		return true;
	if(U == "POINT" || U == "GEOMETRY")
		return true;
	if(StartsWith(U, "GEOMETRY(POINT"))
		return true;
	if(U == "VARIANT" || U == "ANY")
		return true;
	if(U == "TERRAIN" || U == "TERRAIN_POINT")
		return true;
	if(U == "MESH" || StartsWith(U, "GEOMETRY(MESH"))
		return true;
	if(U == "POLYGON" || StartsWith(U, "GEOMETRY(POLYGON"))
		return true;
	return false;
}

std::optional<TypeDescriptor> ParseTypeSpelling(std::string_view SqlType) {
	const std::string Raw = Trim(SqlType);
	const std::string U = Upper(Raw);
	TypeDescriptor D;
	D.SqlSpelling = Raw;
	if(U == "COMPLEX") {
		D.Family = TypeFamily::Complex;
		return D;
	}
	if(StartsWith(U, "STRUCT(") && U.back() == ')') {
		D.Family = TypeFamily::Struct;
		const std::string Inner = Raw.substr(7, Raw.size() - 8);
		for(const auto &Part : SplitTopLevelCommas(Inner)) {
			const size_t Sp = Part.find_last_of(" \t");
			if(Sp == std::string::npos || Sp == 0)
				return std::nullopt;
			std::string FName = Trim(Part.substr(0, Sp));
			std::string FType = ParseSimpleTypeToken(Trim(Part.substr(Sp + 1)));
			if(FName.empty() || FType.empty())
				return std::nullopt;
			D.StructFields.emplace_back(std::move(FName), std::move(FType));
		}
		if(D.StructFields.empty())
			return std::nullopt;
		return D;
	}
	if(StartsWith(U, "MAP(") && U.back() == ')') {
		D.Family = TypeFamily::Map;
		const auto Parts = SplitTopLevelCommas(Raw.substr(4, Raw.size() - 5));
		if(Parts.size() != 2)
			return std::nullopt;
		D.MapKeyType = ParseSimpleTypeToken(Parts[0]);
		D.MapValueType = ParseSimpleTypeToken(Parts[1]);
		return D;
	}
	if(StartsWith(U, "VECTOR(")) {
		D.Family = TypeFamily::Vector;
		const size_t Close = Raw.find(')');
		if(Close == std::string::npos)
			return std::nullopt;
		const auto Len = ParseFloat(Trim(Raw.substr(7, Close - 7)));
		if(!Len || *Len < 1 || *Len > 65535)
			return std::nullopt;
		D.VectorLength = static_cast<std::size_t>(*Len);
		std::string Rest = Trim(Raw.substr(Close + 1));
		if(!Rest.empty())
			D.ElementType = ParseSimpleTypeToken(Rest);
		return D;
	}
	if(StartsWith(U, "LIST(") && U.back() == ')') {
		D.Family = TypeFamily::List;
		const std::string Inner = Trim(Raw.substr(5, Raw.size() - 6));
		if(Inner.empty())
			return std::nullopt;
		D.ListElementType = ParseSimpleTypeToken(Inner);
		return D;
	}
	if(StartsWith(U, "MATRIX(")) {
		D.Family = TypeFamily::Matrix;
		const size_t Close = Raw.find(')');
		if(Close == std::string::npos)
			return std::nullopt;
		const auto Parts = SplitTopLevelCommas(Raw.substr(7, Close - 7));
		if(Parts.size() != 2)
			return std::nullopt;
		const auto R = ParseFloat(Parts[0]);
		const auto C = ParseFloat(Parts[1]);
		if(!R || !C || *R < 1 || *C < 1 || *R > 4096 || *C > 4096)
			return std::nullopt;
		D.MatrixRows = static_cast<std::size_t>(*R);
		D.MatrixCols = static_cast<std::size_t>(*C);
		std::string Rest = Trim(Raw.substr(Close + 1));
		if(!Rest.empty())
			D.ElementType = ParseSimpleTypeToken(Rest);
		return D;
	}
	if(U == "POINT" || StartsWith(U, "GEOMETRY(POINT")) {
		D.Family = TypeFamily::Point;
		return D;
	}
	if(U == "VARIANT" || U == "ANY") {
		D.Family = TypeFamily::Variant;
		return D;
	}
	if(U == "TERRAIN" || U == "TERRAIN_POINT") {
		D.Family = TypeFamily::Terrain;
		return D;
	}
	if(U == "MESH" || StartsWith(U, "GEOMETRY(MESH")) {
		D.Family = TypeFamily::Mesh;
		return D;
	}
	if(U == "POLYGON" || StartsWith(U, "GEOMETRY(POLYGON")) {
		D.Family = TypeFamily::Polygon;
		return D;
	}
	return std::nullopt;
}

std::optional<std::pair<std::string, std::string>> ParseVariantCell(std::string_view Cell) {
	if(!StartsWith(Cell, "V{") || Cell.size() < 4 || Cell.back() != '}')
		return std::nullopt;
	const std::string Inner = std::string(Cell.substr(2, Cell.size() - 3));
	const size_t Colon = Inner.find(':');
	if(Colon == std::string::npos || Colon == 0)
		return std::nullopt;
	std::string Tag = Trim(Inner.substr(0, Colon));
	std::string Payload = Trim(Inner.substr(Colon + 1));
	if(Tag.empty())
		return std::nullopt;
	return std::pair{std::move(Tag), std::move(Payload)};
}

std::string FormatVariantCell(std::string_view Tag, std::string_view Payload) {
	return "V{" + std::string(Tag) + ":" + std::string(Payload) + "}";
}

std::optional<std::unordered_map<std::string, std::string>> ParseStructCell(std::string_view Cell) {
	if(!StartsWith(Cell, "S{") || Cell.size() < 3 || Cell.back() != '}')
		return std::nullopt;
	std::unordered_map<std::string, std::string> Out;
	const std::string Inner = std::string(Cell.substr(2, Cell.size() - 3));
	for(const auto &Part : SplitTopLevelCommas(Inner)) {
		const size_t Eq = Part.find('=');
		if(Eq == std::string::npos)
			return std::nullopt;
		std::string Key = Trim(Part.substr(0, Eq));
		std::string Val = Trim(Part.substr(Eq + 1));
		if(Key.empty())
			return std::nullopt;
		Out[Key] = Val;
	}
	return Out;
}

std::optional<std::unordered_map<std::string, std::string>> ParseMapCell(std::string_view Cell) {
	if(!StartsWith(Cell, "M{") || Cell.size() < 3 || Cell.back() != '}')
		return std::nullopt;
	std::unordered_map<std::string, std::string> Out;
	const std::string Inner = std::string(Cell.substr(2, Cell.size() - 3));
	if(Inner.empty())
		return Out;
	for(const auto &Part : SplitTopLevelCommas(Inner)) {
		const size_t Col = Part.find(':');
		if(Col == std::string::npos)
			return std::nullopt;
		std::string Key = Trim(Part.substr(0, Col));
		std::string Val = Trim(Part.substr(Col + 1));
		if(Key.empty())
			return std::nullopt;
		Out[Key] = Val;
	}
	return Out;
}

std::optional<std::pair<double, double>> ParseComplexCell(std::string_view Cell) {
	if(!StartsWith(Cell, "C(") || Cell.back() != ')')
		return std::nullopt;
	const auto Parts = SplitTopLevelCommas(Cell.substr(2, Cell.size() - 3));
	if(Parts.size() != 2)
		return std::nullopt;
	const auto Re = ParseFloat(Parts[0]);
	const auto Im = ParseFloat(Parts[1]);
	if(!Re || !Im)
		return std::nullopt;
	return std::make_pair(*Re, *Im);
}

std::optional<std::vector<std::string>> ParseListCell(std::string_view Cell) {
	if(!StartsWith(Cell, "L[") || Cell.find("]:") == std::string::npos)
		return std::nullopt;
	const size_t Br = Cell.find("]:");
	const auto Decl = ParseFloat(Cell.substr(2, Br - 2));
	if(!Decl)
		return std::nullopt;
	const std::size_t N = static_cast<std::size_t>(*Decl);
	std::vector<std::string> Out;
	Out.reserve(N);
	const std::string Rest = std::string(Cell.substr(Br + 2));
	if(Rest.empty()) {
		if(N != 0)
			return std::nullopt;
		return Out;
	}
	for(const auto &P : SplitTopLevelCommas(Rest)) {
		if(P.empty())
			return std::nullopt;
		Out.push_back(std::string(P));
	}
	if(Out.size() != N)
		return std::nullopt;
	return Out;
}

std::optional<std::vector<double>> ParseVectorCell(std::string_view Cell, std::size_t ExpectedLen) {
	if(!StartsWith(Cell, "V[") || Cell.find("]:") == std::string::npos)
		return std::nullopt;
	const size_t Br = Cell.find("]:");
	const auto Decl = ParseFloat(Cell.substr(2, Br - 2));
	if(!Decl)
		return std::nullopt;
	const std::size_t N = static_cast<std::size_t>(*Decl);
	if(ExpectedLen != 0 && N != ExpectedLen)
		return std::nullopt;
	std::vector<double> Out;
	Out.reserve(N);
	const std::string Rest = std::string(Cell.substr(Br + 2));
	if(Rest.empty()) {
		if(N != 0)
			return std::nullopt;
		return Out;
	}
	for(const auto &P : SplitTopLevelCommas(Rest)) {
		const auto V = ParseFloat(P);
		if(!V)
			return std::nullopt;
		Out.push_back(*V);
	}
	if(Out.size() != N)
		return std::nullopt;
	return Out;
}

std::optional<MatrixPayload> DecodeMatrixCell(std::string_view Cell) {
	if(!StartsWith(Cell, "T[") || Cell.find("]:") == std::string::npos)
		return std::nullopt;
	const size_t Br = Cell.find(']');
	const auto Head = SplitTopLevelCommas(Cell.substr(2, Br - 2));
	if(Head.size() != 2)
		return std::nullopt;
	const auto R = ParseFloat(Head[0]);
	const auto C = ParseFloat(Head[1]);
	if(!R || !C)
		return std::nullopt;
	MatrixPayload P;
	P.Rows = static_cast<std::size_t>(*R);
	P.Cols = static_cast<std::size_t>(*C);
	const auto Flat = ParseMatrixCell(Cell, P.Rows, P.Cols);
	if(!Flat)
		return std::nullopt;
	P.Flat = std::move(*Flat);
	return P;
}

std::optional<std::vector<double>> ParseMatrixCell(std::string_view Cell, std::size_t ExpectedRows,
                                                   std::size_t ExpectedCols) {
	if(!StartsWith(Cell, "T[") || Cell.find("]:") == std::string::npos)
		return std::nullopt;
	const size_t Br = Cell.find(']');
	const auto Head = SplitTopLevelCommas(Cell.substr(2, Br - 2));
	if(Head.size() != 2)
		return std::nullopt;
	const auto R = ParseFloat(Head[0]);
	const auto C = ParseFloat(Head[1]);
	if(!R || !C)
		return std::nullopt;
	const std::size_t Rows = static_cast<std::size_t>(*R);
	const std::size_t Cols = static_cast<std::size_t>(*C);
	if(ExpectedRows != 0 && Rows != ExpectedRows)
		return std::nullopt;
	if(ExpectedCols != 0 && Cols != ExpectedCols)
		return std::nullopt;
	std::vector<double> Out;
	Out.reserve(Rows * Cols);
	const std::string Rest = std::string(Cell.substr(Br + 2));
	if(Rest.empty()) {
		if(Rows * Cols != 0)
			return std::nullopt;
		return Out;
	}
	for(const auto &P : SplitTopLevelCommas(Rest)) {
		const auto V = ParseFloat(P);
		if(!V)
			return std::nullopt;
		Out.push_back(*V);
	}
	if(Out.size() != Rows * Cols)
		return std::nullopt;
	return Out;
}

std::string FormatStructCell(const std::unordered_map<std::string, std::string> &Fields) {
	std::ostringstream O;
	O << "S{";
	bool First = true;
	for(const auto &[K, V] : Fields) {
		if(!First)
			O << ',';
		First = false;
		O << K << '=' << V;
	}
	O << '}';
	return std::move(O).str();
}

std::string FormatMapCell(const std::unordered_map<std::string, std::string> &Entries) {
	std::ostringstream O;
	O << "M{";
	bool First = true;
	for(const auto &[K, V] : Entries) {
		if(!First)
			O << ',';
		First = false;
		O << K << ':' << V;
	}
	O << '}';
	return std::move(O).str();
}

std::string FormatComplexCell(double Re, double Im) {
	std::ostringstream O;
	O << "C(" << Re << ',' << Im << ')';
	return std::move(O).str();
}

std::string FormatListCell(const std::vector<std::string> &Elements) {
	std::ostringstream O;
	O << "L[" << Elements.size() << "]:";
	for(size_t I = 0; I < Elements.size(); ++I) {
		if(I)
			O << ',';
		O << Elements[I];
	}
	return std::move(O).str();
}

std::string FormatVectorCell(const std::vector<double> &Values) {
	std::ostringstream O;
	O << "V[" << Values.size() << "]:";
	for(size_t I = 0; I < Values.size(); ++I) {
		if(I)
			O << ',';
		O << Values[I];
	}
	return std::move(O).str();
}

std::string FormatMatrixCell(const std::vector<double> &Flat, std::size_t Rows, std::size_t Cols) {
	std::ostringstream O;
	O << "T[" << Rows << ',' << Cols << "]:";
	for(size_t I = 0; I < Flat.size(); ++I) {
		if(I)
			O << ',';
		O << Flat[I];
	}
	return std::move(O).str();
}

bool ValidateCell(const TypeDescriptor &Type, std::string_view Cell) {
	if(Cell.empty())
		return false;
	switch(Type.Family) {
	case TypeFamily::Struct:
		return ParseStructCell(Cell).has_value();
	case TypeFamily::Map:
		return ParseMapCell(Cell).has_value();
	case TypeFamily::Complex:
		return ParseComplexCell(Cell).has_value();
	case TypeFamily::Vector:
		return ParseVectorCell(Cell, Type.VectorLength).has_value();
	case TypeFamily::Matrix:
		return ParseMatrixCell(Cell, Type.MatrixRows, Type.MatrixCols).has_value();
	case TypeFamily::List:
		return ParseListCell(Cell).has_value();
	case TypeFamily::Point:
		return GeoSpatial::ParsePointCell(Cell).has_value() || GeoSpatial::ParseWktPoint(Cell).has_value();
	case TypeFamily::Variant:
		return ParseVariantCell(Cell).has_value();
	case TypeFamily::Terrain:
		return GeoSpatial::ParseTerrainCell(Cell).has_value() || GeoSpatial::ParseWktPointZ(Cell).has_value();
	case TypeFamily::Mesh:
		return DS::glTF::ParseMeshCell(Cell).has_value();
	case TypeFamily::Polygon: {
		if(const auto P = DS::Geometry2D::ParsePolygonCell(Cell))
			return DS::Geometry2D::ValidatePolygon(*P).Ok;
		if(const auto W = DS::Geometry2D::ParseWktPolygon(Cell))
			return DS::Geometry2D::ValidatePolygon(*W).Ok;
		return false;
	}
	default:
		return true;
	}
}

std::optional<std::string> NormalizeCell(const TypeDescriptor &Type, std::string_view Cell) {
	switch(Type.Family) {
	case TypeFamily::Struct: {
		const auto P = ParseStructCell(Cell);
		return P ? std::optional<std::string>(FormatStructCell(*P)) : std::nullopt;
	}
	case TypeFamily::Map: {
		const auto P = ParseMapCell(Cell);
		return P ? std::optional<std::string>(FormatMapCell(*P)) : std::nullopt;
	}
	case TypeFamily::Complex: {
		const auto P = ParseComplexCell(Cell);
		return P ? std::optional<std::string>(FormatComplexCell(P->first, P->second)) : std::nullopt;
	}
	case TypeFamily::Vector: {
		const auto P = ParseVectorCell(Cell, Type.VectorLength);
		return P ? std::optional<std::string>(FormatVectorCell(*P)) : std::nullopt;
	}
	case TypeFamily::Matrix: {
		const auto P = ParseMatrixCell(Cell, Type.MatrixRows, Type.MatrixCols);
		return P ? std::optional<std::string>(FormatMatrixCell(*P, Type.MatrixRows, Type.MatrixCols)) : std::nullopt;
	}
	case TypeFamily::List: {
		const auto P = ParseListCell(Cell);
		return P ? std::optional<std::string>(FormatListCell(*P)) : std::nullopt;
	}
	case TypeFamily::Point: {
		if(const auto P = GeoSpatial::ParsePointCell(Cell))
			return GeoSpatial::FormatPointCell(P->Lon, P->Lat);
		if(const auto W = GeoSpatial::ParseWktPoint(Cell))
			return GeoSpatial::FormatPointCell(W->Lon, W->Lat);
		return std::nullopt;
	}
	case TypeFamily::Variant: {
		if(const auto P = ParseVariantCell(Cell))
			return FormatVariantCell(P->first, P->second);
		return std::nullopt;
	}
	case TypeFamily::Terrain: {
		if(const auto T = GeoSpatial::ParseTerrainCell(Cell))
			return GeoSpatial::FormatTerrainCell(T->Lon, T->Lat, T->ElevM);
		if(const auto W = GeoSpatial::ParseWktPointZ(Cell))
			return GeoSpatial::FormatTerrainCell(W->Lon, W->Lat, W->ElevM);
		return std::nullopt;
	}
	case TypeFamily::Mesh: {
		if(const auto M = DS::glTF::ParseMeshCell(Cell))
			return DS::glTF::FormatMeshCell(*M);
		return std::nullopt;
	}
	case TypeFamily::Polygon: {
		if(const auto P = DS::Geometry2D::ParsePolygonCell(Cell))
			return DS::Geometry2D::FormatPolygonCell(*P);
		if(const auto W = DS::Geometry2D::ParseWktPolygon(Cell))
			return DS::Geometry2D::FormatPolygonCell(*W);
		return std::nullopt;
	}
	default:
		return std::string(Cell);
	}
}

} // namespace AdvancedTypes
} // namespace AstralDB
