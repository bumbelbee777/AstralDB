#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AstralDB {
namespace AdvancedTypes {

enum class TypeFamily : int8_t {
	Scalar = 0,
	Struct = 1,
	Map = 2,
	Vector = 3,
	Matrix = 4,
	Complex = 5,
	List = 6,
	Point = 7,
	Variant = 8,
	Terrain = 9,
	Mesh = 10,
	Polygon = 11
};

struct TypeDescriptor {
	TypeFamily Family = TypeFamily::Scalar;
	std::string SqlSpelling;
	std::vector<std::pair<std::string, std::string>> StructFields;
	std::string MapKeyType;
	std::string MapValueType;
	std::size_t VectorLength = 0;
	std::size_t MatrixRows = 0;
	std::size_t MatrixCols = 0;
	std::string ElementType = "FLOAT";
	std::string ListElementType;
};

bool IsAdvancedTypeSpelling(std::string_view SqlType);
std::optional<TypeDescriptor> ParseTypeSpelling(std::string_view SqlType);
bool ValidateCell(const TypeDescriptor &Type, std::string_view Cell);
std::optional<std::string> NormalizeCell(const TypeDescriptor &Type, std::string_view Cell);

std::optional<std::unordered_map<std::string, std::string>> ParseStructCell(std::string_view Cell);
std::optional<std::unordered_map<std::string, std::string>> ParseMapCell(std::string_view Cell);
std::optional<std::pair<double, double>> ParseComplexCell(std::string_view Cell);
std::optional<std::vector<double>> ParseVectorCell(std::string_view Cell, std::size_t ExpectedLen = 0);
std::optional<std::vector<std::string>> ParseListCell(std::string_view Cell);
std::optional<std::vector<double>> ParseMatrixCell(std::string_view Cell, std::size_t ExpectedRows = 0,
                                                   std::size_t ExpectedCols = 0);
std::optional<std::pair<std::string, std::string>> ParseVariantCell(std::string_view Cell);

std::string FormatVariantCell(std::string_view Tag, std::string_view Payload);

struct MatrixPayload {
	std::size_t Rows = 0;
	std::size_t Cols = 0;
	std::vector<double> Flat;
};
std::optional<MatrixPayload> DecodeMatrixCell(std::string_view Cell);

std::string FormatStructCell(const std::unordered_map<std::string, std::string> &Fields);
std::string FormatMapCell(const std::unordered_map<std::string, std::string> &Entries);
std::string FormatComplexCell(double Re, double Im);
std::string FormatVectorCell(const std::vector<double> &Values);
std::string FormatListCell(const std::vector<std::string> &Elements);
std::string FormatMatrixCell(const std::vector<double> &Flat, std::size_t Rows, std::size_t Cols);

} // namespace AdvancedTypes
} // namespace AstralDB
