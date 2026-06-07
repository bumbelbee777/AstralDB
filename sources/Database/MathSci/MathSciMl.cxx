#include <Database/MathSci/MathSciMl.hxx>

#include <Database/MathSci/MathSciComplex.hxx>
#include <Database/MathSci/MathSciModel.hxx>
#include <Database/MathSci/MathSciPrimitiveMicrokernels.hxx>
#include <Database/MathSci/MathSciSimdUtil.hxx>
#include <Database/Types/AdvancedTypes.hxx>
#include <IO/SIMD.hxx>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string>

namespace AstralDB {
namespace MathSciMl {
namespace {

constexpr std::uint32_t QuantModelMagic = 0x0038514Du; // 'Q8M\0'
constexpr std::uint16_t QuantModelVersion = 1;
constexpr std::uint32_t Fp16ModelMagic = 0x3631464Du; // 'MF16'
constexpr std::uint16_t Fp16ModelVersion = 1;

std::optional<std::vector<double>> AsDoublesFromList(const std::vector<std::string> &Cells) {
	std::vector<double> Out;
	Out.reserve(Cells.size());
	for(const auto &S : Cells) {
		try {
			Out.push_back(std::stod(S));
		} catch(...) {
			return std::nullopt;
		}
	}
	return Out;
}

std::optional<std::vector<double>> ParseCommaDoubles(std::string_view Body) {
	if(Body.empty())
		return std::nullopt;
	std::vector<double> Out;
	std::size_t Pos = 0;
	while(Pos < Body.size()) {
		std::size_t End = Pos;
		while(End < Body.size() && Body[End] != ',')
			++End;
		try {
			Out.push_back(std::stod(std::string(Body.substr(Pos, End - Pos))));
		} catch(...) {
			return std::nullopt;
		}
		Pos = End + (End < Body.size() ? 1 : 0);
	}
	return Out;
}

std::optional<std::vector<double>> ParseRealSeq(std::string_view Cell) {
	if(const auto L = AdvancedTypes::ParseListCell(Cell))
		return AsDoublesFromList(*L);
	if(const auto V = AdvancedTypes::ParseVectorCell(Cell))
		return *V;
	return std::nullopt;
}

std::vector<float> ToF32(const std::vector<double> &V) {
	return MathSciSimdUtil::SeqToF32(V);
}

std::optional<double> ParseNum(std::string_view S) {
	if(S.empty())
		return std::nullopt;
	try {
		return std::stod(std::string(S));
	} catch(...) {
		return std::nullopt;
	}
}

std::optional<std::vector<double>> ParsePrefixedBody(std::string_view Cell, std::string_view Prefix) {
	if(Cell.size() < Prefix.size() + 3 || Cell.substr(0, Prefix.size()) != Prefix)
		return std::nullopt;
	const auto Close = Cell.find("]:");
	if(Close == std::string_view::npos || Close + 2 >= Cell.size())
		return std::nullopt;
	return ParseCommaDoubles(Cell.substr(Close + 2));
}

std::optional<std::pair<std::size_t, std::size_t>> ParseMatrixDecl(std::string_view Cell, std::string_view Prefix) {
	if(Cell.size() < Prefix.size() + 3 || Cell.substr(0, Prefix.size()) != Prefix)
		return std::nullopt;
	const auto Close = Cell.find(']');
	if(Close == std::string_view::npos || Close + 2 >= Cell.size() || Cell[Close + 1] != ':')
		return std::nullopt;
	const auto Comma = Cell.find(',', Prefix.size());
	if(Comma == std::string_view::npos || Comma > Close)
		return std::nullopt;
	const auto R = ParseNum(Cell.substr(Prefix.size(), Comma - Prefix.size()));
	const auto C = ParseNum(Cell.substr(Comma + 1, Close - Comma - 1));
	if(!R || !C || *R <= 0 || *C <= 0)
		return std::nullopt;
	return std::make_pair(static_cast<std::size_t>(*R), static_cast<std::size_t>(*C));
}

MlTensor TensorFromNumericVec(const MathSciComplex::NumericVec &V, MlDtype Dtype) {
	MlTensor T;
	T.Dtype = Dtype;
	T.F32 = V.Values;
	return T;
}

std::vector<float> InterleavedF64ToF32(const std::vector<double> &V) {
	std::vector<float> Out(V.size());
	for(std::size_t I = 0; I < V.size(); ++I)
		Out[I] = static_cast<float>(V[I]);
	return Out;
}

std::vector<double> InterleavedF32ToF64(const std::vector<float> &V) {
	std::vector<double> Out(V.size());
	for(std::size_t I = 0; I < V.size(); ++I)
		Out[I] = static_cast<double>(V[I]);
	return Out;
}

std::string FormatCommaDoubles(const std::vector<double> &V) {
	std::ostringstream O;
	for(std::size_t I = 0; I < V.size(); ++I) {
		if(I)
			O << ',';
		O << V[I];
	}
	return std::move(O).str();
}

static char HexDigit(unsigned V) { return static_cast<char>(V < 10 ? '0' + V : 'a' + (V - 10)); }

std::string HexEncode(const std::uint8_t *Data, std::size_t Len) {
	std::string Out;
	Out.reserve(Len * 2);
	for(std::size_t I = 0; I < Len; ++I) {
		Out.push_back(HexDigit((Data[I] >> 4) & 0xF));
		Out.push_back(HexDigit(Data[I] & 0xF));
	}
	return Out;
}

std::optional<int> HexVal(char C) {
	if(C >= '0' && C <= '9')
		return C - '0';
	if(C >= 'a' && C <= 'f')
		return 10 + (C - 'a');
	if(C >= 'A' && C <= 'F')
		return 10 + (C - 'A');
	return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> HexDecode(std::string_view Hex) {
	if((Hex.size() % 2) != 0)
		return std::nullopt;
	std::vector<std::uint8_t> Out;
	Out.reserve(Hex.size() / 2);
	for(std::size_t I = 0; I < Hex.size(); I += 2) {
		const auto Hi = HexVal(Hex[I]);
		const auto Lo = HexVal(Hex[I + 1]);
		if(!Hi || !Lo)
			return std::nullopt;
		Out.push_back(static_cast<std::uint8_t>((*Hi << 4) | *Lo));
	}
	return Out;
}

void AppendU32(std::vector<std::uint8_t> &Buf, std::uint32_t V) {
	Buf.push_back(static_cast<std::uint8_t>(V & 0xFF));
	Buf.push_back(static_cast<std::uint8_t>((V >> 8) & 0xFF));
	Buf.push_back(static_cast<std::uint8_t>((V >> 16) & 0xFF));
	Buf.push_back(static_cast<std::uint8_t>((V >> 24) & 0xFF));
}

void AppendU16(std::vector<std::uint8_t> &Buf, std::uint16_t V) {
	Buf.push_back(static_cast<std::uint8_t>(V & 0xFF));
	Buf.push_back(static_cast<std::uint8_t>((V >> 8) & 0xFF));
}

std::optional<std::uint32_t> ReadU32(const std::uint8_t *&P, const std::uint8_t *End) {
	if(static_cast<std::size_t>(End - P) < 4)
		return std::nullopt;
	const std::uint32_t V = static_cast<std::uint32_t>(P[0]) | (static_cast<std::uint32_t>(P[1]) << 8) |
	                        (static_cast<std::uint32_t>(P[2]) << 16) | (static_cast<std::uint32_t>(P[3]) << 24);
	P += 4;
	return V;
}

std::optional<std::uint16_t> ReadU16(const std::uint8_t *&P, const std::uint8_t *End) {
	if(static_cast<std::size_t>(End - P) < 2)
		return std::nullopt;
	const std::uint16_t V = static_cast<std::uint16_t>(P[0]) | (static_cast<std::uint16_t>(P[1]) << 8);
	P += 2;
	return V;
}

void ApplyActivation(MathSciModel::Activation Act, float *V, std::size_t N) {
	switch(Act) {
	case MathSciModel::Activation::Linear:
		break;
	case MathSciModel::Activation::Sigmoid:
		for(std::size_t I = 0; I < N; ++I)
			V[I] = 1.f / (1.f + std::exp(-V[I]));
		break;
	case MathSciModel::Activation::Tanh:
		for(std::size_t I = 0; I < N; ++I)
			V[I] = std::tanh(V[I]);
		break;
	case MathSciModel::Activation::Relu:
		Simd::ReluF32(V, V, N);
		break;
	}
}

} // namespace

std::int8_t QuantizeSymF32(float Value, float Scale) {
	if(Scale <= 0.f)
		return 0;
	const float Scaled = Value / Scale;
	const int Rounded = static_cast<int>(std::lround(Scaled));
	return static_cast<std::int8_t>(std::clamp(Rounded, -127, 127));
}

float DequantizeSymF32(std::int8_t Code, float Scale) { return static_cast<float>(Code) * Scale; }

float QuantizeScaleSymF32(const float *Values, std::size_t N) {
	return MathSciPrimitiveMicrokernels::QuantizeScaleSymF32(Values, N);
}

QuantizedSeq QuantizeInt8F32(const float *Values, std::size_t N) {
	QuantizedSeq Q;
	Q.Scale = QuantizeScaleSymF32(Values, N);
	Q.Codes.resize(N);
	MathSciPrimitiveMicrokernels::QuantizeSymF32ToI8(Values, Q.Codes.data(), N, Q.Scale);
	return Q;
}

std::vector<float> DequantizeInt8F32(const QuantizedSeq &Q) {
	std::vector<float> Out(Q.Codes.size());
	MathSciPrimitiveMicrokernels::DequantizeI8ToF32(Q.Codes.data(), Out.data(), Q.Codes.size(), Q.Scale);
	return Out;
}

struct QuantizedInt4Pack {
	float Scale = 1.f;
	std::vector<std::uint8_t> Bytes;
};

QuantizedInt4Pack QuantizeInt4F32(const float *Values, std::size_t N) {
	QuantizedInt4Pack Q;
	Q.Scale = MathSciPrimitiveMicrokernels::QuantizeScaleSymI4F32(Values, N);
	const std::size_t Bytes = (N + 1) / 2;
	Q.Bytes.resize(Bytes);
	MathSciPrimitiveMicrokernels::QuantizeSymF32ToI4Packed(Values, Q.Bytes.data(), N, Q.Scale);
	return Q;
}

std::vector<float> DequantizeInt4F32(const QuantizedInt4Pack &Q, std::size_t FloatCount) {
	std::vector<float> Out(FloatCount);
	MathSciPrimitiveMicrokernels::DequantizeI4PackedToF32(Q.Bytes.data(), Out.data(), FloatCount, Q.Scale);
	return Out;
}

std::uint16_t Fp32ToFp16Bits(float Value) {
	const std::uint32_t Bits = std::bit_cast<std::uint32_t>(Value);
	const std::uint32_t Sign = (Bits >> 16) & 0x8000u;
	std::uint32_t Exp = (Bits >> 23) & 0xFFu;
	std::uint32_t Mant = Bits & 0x7FFFFFu;
	if(Exp == 0xFFu)
		return static_cast<std::uint16_t>(Sign | 0x7C00u | (Mant ? 0x0200u : 0u));
	if(Exp == 0u)
		return static_cast<std::uint16_t>(Sign);
	Exp = Exp + 112;
	if(Exp >= 0x1Fu)
		return static_cast<std::uint16_t>(Sign | 0x7C00u);
	if(Exp <= 0u)
		return static_cast<std::uint16_t>(Sign);
	Mant >>= 13;
	return static_cast<std::uint16_t>(Sign | (Exp << 10) | Mant);
}

float Fp16BitsToFp32(std::uint16_t Bits) {
	const std::uint32_t Sign = static_cast<std::uint32_t>(Bits & 0x8000u) << 16;
	const std::uint32_t Exp = (Bits >> 10) & 0x1Fu;
	const std::uint32_t Mant = Bits & 0x3FFu;
	if(Exp == 0) {
		if(Mant == 0)
			return std::bit_cast<float>(Sign);
		std::uint32_t Mantissa = Mant;
		std::uint32_t Exponent = 127 - 14;
		while((Mantissa & 0x400u) == 0) {
			Mantissa <<= 1;
			--Exponent;
		}
		Mantissa &= 0x3FFu;
		const std::uint32_t Out = Sign | (Exponent << 23) | (Mantissa << 13);
		return std::bit_cast<float>(Out);
	}
	if(Exp == 31u) {
		const std::uint32_t Out = Sign | 0x7F800000u | (Mant << 13);
		return std::bit_cast<float>(Out);
	}
	const std::uint32_t Out = Sign | ((Exp + 112) << 23) | (Mant << 13);
	return std::bit_cast<float>(Out);
}

std::vector<std::uint16_t> Fp32ToFp16Bits(const float *Values, std::size_t N) {
	std::vector<std::uint16_t> Out(N);
	for(std::size_t I = 0; I < N; ++I)
		Out[I] = Fp32ToFp16Bits(Values[I]);
	return Out;
}

std::vector<float> Fp16BitsToFp32(const std::uint16_t *Bits, std::size_t N) {
	std::vector<float> Out(N);
	for(std::size_t I = 0; I < N; ++I)
		Out[I] = Fp16BitsToFp32(Bits[I]);
	return Out;
}

std::vector<float> PruneMagnitudesF32(const float *Values, std::size_t N, float Threshold) {
	std::vector<float> Out(N);
	for(std::size_t I = 0; I < N; ++I)
		Out[I] = std::abs(Values[I]) < Threshold ? 0.f : Values[I];
	return Out;
}

std::vector<float> PruneComplexMagnitudesF32(const float *Interleaved, std::size_t Slots, float Threshold) {
	std::vector<float> Out(Slots * 2);
	for(std::size_t I = 0; I < Slots; ++I) {
		const float Re = Interleaved[I * 2];
		const float Im = Interleaved[I * 2 + 1];
		if(std::hypot(Re, Im) < Threshold) {
			Out[I * 2] = 0.f;
			Out[I * 2 + 1] = 0.f;
		} else {
			Out[I * 2] = Re;
			Out[I * 2 + 1] = Im;
		}
	}
	return Out;
}

std::vector<float> SinusoidalComplexPosEncF32(std::size_t Pos, std::size_t Slots, float Base) {
	if(Slots == 0 || Slots > MaxPosEncDim)
		return {};
	std::vector<float> Out(Slots * 2);
	const float Denom = static_cast<float>(Slots);
	for(std::size_t I = 0; I < Slots; ++I) {
		const float Exponent = static_cast<float>(2 * I) / Denom;
		const float Freq = std::pow(Base, Exponent);
		const float Angle = static_cast<float>(Pos) / Freq;
		Out[I * 2] = std::cos(Angle);
		Out[I * 2 + 1] = std::sin(Angle);
	}
	return Out;
}

std::vector<float> AddComplexPosEncSequenceF32(const float *Seq, std::size_t Len, std::size_t Slots, float Base) {
	if(Len == 0 || Slots == 0 || Len * Slots > MaxMlLen)
		return {};
	std::vector<float> Out(Len * Slots * 2);
	for(std::size_t P = 0; P < Len; ++P) {
		const auto Pe = SinusoidalComplexPosEncF32(P, Slots, Base);
		const std::size_t Off = P * Slots * 2;
		Simd::ComplexAddInterleavedF32(Out.data() + Off, Seq + Off, Pe.data(), Slots);
	}
	return Out;
}

const char *DtypeName(MlDtype D) {
	switch(D) {
	case MlDtype::Float32:
		return "float32";
	case MlDtype::Float64:
		return "float64";
	case MlDtype::Float16:
		return "float16";
	case MlDtype::Int8:
		return "int8";
	case MlDtype::Complex64:
		return "complex64";
	case MlDtype::Complex128:
		return "complex128";
	case MlDtype::Complex16:
		return "complex16";
	case MlDtype::ComplexInt8:
		return "complexint8";
	case MlDtype::Int4:
		return "int4";
	case MlDtype::ComplexInt4:
		return "complexint4";
	}
	return "unknown";
}

std::optional<MlDtype> ParseDtypeName(std::string_view Name) {
	std::string K(Name);
	for(char &C : K)
		C = static_cast<char>(std::tolower(static_cast<unsigned char>(C)));
	if(K == "float32" || K == "f32" || K == "fp32")
		return MlDtype::Float32;
	if(K == "float64" || K == "f64" || K == "fp64")
		return MlDtype::Float64;
	if(K == "float16" || K == "f16" || K == "fp16" || K == "half")
		return MlDtype::Float16;
	if(K == "int8" || K == "i8")
		return MlDtype::Int8;
	if(K == "complex64" || K == "c64" || K == "cf32" || K == "complexfloat")
		return MlDtype::Complex64;
	if(K == "complex128" || K == "c128" || K == "cf64" || K == "complexdouble")
		return MlDtype::Complex128;
	if(K == "complex16" || K == "c16" || K == "cf16" || K == "complexhalf")
		return MlDtype::Complex16;
	if(K == "complexint8" || K == "ci8" || K == "complexi8")
		return MlDtype::ComplexInt8;
	if(K == "int4" || K == "i4")
		return MlDtype::Int4;
	if(K == "complexint4" || K == "ci4" || K == "complexi4")
		return MlDtype::ComplexInt4;
	return std::nullopt;
}

std::size_t CountPrunedZerosF32(const float *Values, std::size_t N) {
	std::size_t Z = 0;
	for(std::size_t I = 0; I < N; ++I)
		if(Values[I] == 0.f)
			++Z;
	return Z;
}

std::vector<float> SinusoidalPosEncF32(std::size_t Pos, std::size_t Dim, float Base) {
	if(Dim == 0 || Dim > MaxPosEncDim)
		return {};
	std::vector<float> Out(Dim);
	const float Denom = static_cast<float>(Dim);
	for(std::size_t I = 0; I < Dim; I += 2) {
		const float Exponent = static_cast<float>(I) / Denom;
		const float Freq = std::pow(Base, Exponent);
		const float Angle = static_cast<float>(Pos) / Freq;
		Out[I] = std::sin(Angle);
		if(I + 1 < Dim)
			Out[I + 1] = std::cos(Angle);
	}
	return Out;
}

std::vector<float> SinusoidalPosEncSequenceF32(std::size_t Len, std::size_t Dim, float Base) {
	if(Len == 0 || Dim == 0 || Len * Dim > MaxMlLen)
		return {};
	std::vector<float> Out(Len * Dim);
	for(std::size_t P = 0; P < Len; ++P) {
		const auto Row = SinusoidalPosEncF32(P, Dim, Base);
		Simd::Memcpy(Out.data() + P * Dim, Row.data(), Dim * sizeof(float));
	}
	return Out;
}

std::vector<float> AddPosEncSequenceF32(const float *Seq, std::size_t Len, std::size_t Dim, float Base) {
	if(Len == 0 || Dim == 0 || Len * Dim > MaxMlLen)
		return {};
	std::vector<float> Out(Len * Dim);
	for(std::size_t P = 0; P < Len; ++P) {
		const auto Pe = SinusoidalPosEncF32(P, Dim, Base);
		const std::size_t Off = P * Dim;
		Simd::AddF32(Out.data() + Off, Seq + Off, Pe.data(), Dim);
	}
	return Out;
}

std::optional<QuantizedSeq> ParseQuantizedCell(std::string_view Cell) {
	if(Cell.size() < 4 || Cell.substr(0, 3) != "Q8[")
		return std::nullopt;
	const auto Colon = Cell.find("]:");
	if(Colon == std::string_view::npos || Colon + 2 >= Cell.size())
		return std::nullopt;
	const auto R = ParseCommaDoubles(Cell.substr(Colon + 2));
	if(!R || R->size() < 2)
		return std::nullopt;
	QuantizedSeq Q;
	Q.Scale = static_cast<float>((*R)[0]);
	Q.Codes.reserve(R->size() - 1);
	for(std::size_t I = 1; I < R->size(); ++I)
		Q.Codes.push_back(static_cast<std::int8_t>(std::lround((*R)[I])));
	return Q;
}

std::string FormatQuantizedCell(const QuantizedSeq &Q) {
	std::ostringstream O;
	O << "Q8[" << (Q.Codes.size() + 1) << "]:" << Q.Scale;
	for(std::int8_t C : Q.Codes)
		O << ',' << static_cast<int>(C);
	return std::move(O).str();
}

std::optional<QuantizedSeq> ParseQuantizedComplexCell(std::string_view Cell) {
	if(Cell.rfind("Q8TC[", 0) == 0) {
		const auto Dims = ParseMatrixDecl(Cell, "Q8TC[");
		const auto Close = Cell.find("]:");
		if(!Dims || Close == std::string_view::npos)
			return std::nullopt;
		const auto R = ParseCommaDoubles(Cell.substr(Close + 2));
		if(!R || R->size() < 2)
			return std::nullopt;
		const std::size_t Need = Dims->first * Dims->second * 2 + 1;
		if(R->size() != Need)
			return std::nullopt;
		QuantizedSeq Q;
		Q.Scale = static_cast<float>((*R)[0]);
		Q.Codes.reserve(R->size() - 1);
		for(std::size_t I = 1; I < R->size(); ++I)
			Q.Codes.push_back(static_cast<std::int8_t>(std::lround((*R)[I])));
		return Q;
	}
	if(Cell.rfind("Q8C[", 0) != 0)
		return std::nullopt;
	const auto Colon = Cell.find("]:");
	if(Colon == std::string_view::npos || Colon + 2 >= Cell.size())
		return std::nullopt;
	const auto R = ParseCommaDoubles(Cell.substr(Colon + 2));
	if(!R || R->size() < 2)
		return std::nullopt;
	QuantizedSeq Q;
	Q.Scale = static_cast<float>((*R)[0]);
	Q.Codes.reserve(R->size() - 1);
	for(std::size_t I = 1; I < R->size(); ++I)
		Q.Codes.push_back(static_cast<std::int8_t>(std::lround((*R)[I])));
	return Q;
}

std::string FormatQuantizedComplexCell(const QuantizedSeq &Q) {
	std::ostringstream O;
	O << "Q8C[" << (Q.Codes.size() + 1) << "]:" << Q.Scale;
	for(std::int8_t C : Q.Codes)
		O << ',' << static_cast<int>(C);
	return std::move(O).str();
}

std::optional<QuantizedInt4Pack> ParseQuantizedInt4Cell(std::string_view Cell) {
	if(Cell.rfind("Q4TC[", 0) == 0) {
		const auto Dims = ParseMatrixDecl(Cell, "Q4TC[");
		const auto Close = Cell.find("]:");
		if(!Dims || Close == std::string_view::npos)
			return std::nullopt;
		const auto R = ParseCommaDoubles(Cell.substr(Close + 2));
		if(!R || R->size() < 2)
			return std::nullopt;
		const std::size_t NeedSlots = Dims->first * Dims->second;
		const std::size_t Need = NeedSlots + 1;
		if(R->size() != Need)
			return std::nullopt;
		QuantizedInt4Pack Q;
		Q.Scale = static_cast<float>((*R)[0]);
		Q.Bytes.reserve(R->size() - 1);
		for(std::size_t I = 1; I < R->size(); ++I)
			Q.Bytes.push_back(static_cast<std::uint8_t>(std::lround((*R)[I])));
		return Q;
	}
	if(Cell.rfind("Q4C[", 0) == 0 || Cell.rfind("Q4[", 0) == 0) {
		const auto Colon = Cell.find("]:");
		if(Colon == std::string_view::npos || Colon + 2 >= Cell.size())
			return std::nullopt;
		const auto R = ParseCommaDoubles(Cell.substr(Colon + 2));
		if(!R || R->size() < 2)
			return std::nullopt;
		QuantizedInt4Pack Q;
		Q.Scale = static_cast<float>((*R)[0]);
		Q.Bytes.reserve(R->size() - 1);
		for(std::size_t I = 1; I < R->size(); ++I)
			Q.Bytes.push_back(static_cast<std::uint8_t>(std::lround((*R)[I])));
		return Q;
	}
	return std::nullopt;
}

std::string FormatQuantizedInt4Cell(const QuantizedInt4Pack &Q, std::size_t DeclCount, bool Complex) {
	std::ostringstream O;
	O << (Complex ? "Q4C[" : "Q4[") << DeclCount << "]:" << Q.Scale;
	for(std::uint8_t B : Q.Bytes)
		O << ',' << static_cast<int>(B);
	return std::move(O).str();
}

std::optional<MlTensor> ParseMlCell(std::string_view Cell) {
	if(Cell.rfind("Q4TC[", 0) == 0) {
		const auto Dims = ParseMatrixDecl(Cell, "Q4TC[");
		const auto Q = ParseQuantizedInt4Cell(Cell);
		if(!Dims || !Q)
			return std::nullopt;
		MlTensor T;
		T.Dtype = MlDtype::ComplexInt4;
		T.I8Scale = Q->Scale;
		T.IsMatrix = true;
		T.MatRows = Dims->first;
		T.MatCols = Dims->second;
		T.I4 = Q->Bytes;
		return T;
	}
	if(Cell.rfind("Q4C[", 0) == 0) {
		const auto Q = ParseQuantizedInt4Cell(Cell);
		if(!Q)
			return std::nullopt;
		MlTensor T;
		T.Dtype = MlDtype::ComplexInt4;
		T.I8Scale = Q->Scale;
		T.I4 = Q->Bytes;
		return T;
	}
	if(Cell.rfind("Q4[", 0) == 0) {
		const auto Q = ParseQuantizedInt4Cell(Cell);
		if(!Q)
			return std::nullopt;
		MlTensor T;
		T.Dtype = MlDtype::Int4;
		T.I8Scale = Q->Scale;
		T.I4 = Q->Bytes;
		return T;
	}
	if(Cell.rfind("Q8TC[", 0) == 0) {
		const auto Dims = ParseMatrixDecl(Cell, "Q8TC[");
		const auto Q = ParseQuantizedComplexCell(Cell);
		if(!Dims || !Q)
			return std::nullopt;
		MlTensor T;
		T.Dtype = MlDtype::ComplexInt8;
		T.I8Scale = Q->Scale;
		T.IsMatrix = true;
		T.MatRows = Dims->first;
		T.MatCols = Dims->second;
		T.I8 = Q->Codes;
		return T;
	}
	if(Cell.rfind("Q8C[", 0) == 0) {
		const auto Q = ParseQuantizedComplexCell(Cell);
		if(!Q)
			return std::nullopt;
		MlTensor T;
		T.Dtype = MlDtype::ComplexInt8;
		T.I8Scale = Q->Scale;
		T.I8 = Q->Codes;
		return T;
	}
	if(Cell.rfind("Q8[", 0) == 0) {
		const auto Q = ParseQuantizedCell(Cell);
		if(!Q)
			return std::nullopt;
		MlTensor T;
		T.Dtype = MlDtype::Int8;
		T.I8Scale = Q->Scale;
		T.I8 = Q->Codes;
		return T;
	}
	if(Cell.rfind("CH[", 0) == 0) {
		const auto R = ParsePrefixedBody(Cell, "CH[");
		if(!R || (R->size() % 2) != 0)
			return std::nullopt;
		MlTensor T;
		T.Dtype = MlDtype::Complex16;
		T.F16.reserve(R->size());
		for(double V : *R)
			T.F16.push_back(static_cast<std::uint16_t>(static_cast<std::uint64_t>(V)));
		return T;
	}
	if(Cell.rfind("CD[", 0) == 0) {
		const auto R = ParsePrefixedBody(Cell, "CD[");
		if(!R || (R->size() % 2) != 0)
			return std::nullopt;
		MlTensor T;
		T.Dtype = MlDtype::Complex128;
		T.F64 = *R;
		return T;
	}
	if(Cell.rfind("F16[", 0) == 0) {
		const auto R = ParsePrefixedBody(Cell, "F16[");
		if(!R)
			return std::nullopt;
		MlTensor T;
		T.Dtype = MlDtype::Float16;
		T.F16.reserve(R->size());
		for(double V : *R)
			T.F16.push_back(static_cast<std::uint16_t>(static_cast<std::uint64_t>(V)));
		return T;
	}
	if(Cell.rfind("D[", 0) == 0) {
		const auto R = ParsePrefixedBody(Cell, "D[");
		if(!R)
			return std::nullopt;
		MlTensor T;
		T.Dtype = MlDtype::Float64;
		T.F64 = *R;
		return T;
	}
	if(const auto M = MathSciComplex::ParseNumericMat(Cell)) {
		MlTensor T;
		T.IsMatrix = true;
		T.MatRows = M->Rows;
		T.MatCols = M->Cols;
		if(M->Kind == MathSciComplex::NumericKind::Complex) {
			T.Dtype = MlDtype::Complex64;
			T.F32 = M->Flat;
		} else {
			T.Dtype = MlDtype::Float32;
			T.F32 = M->Flat;
		}
		return T;
	}
	if(const auto V = MathSciComplex::ParseNumericVec(Cell)) {
		if(V->IsComplex())
			return TensorFromNumericVec(*V, MlDtype::Complex64);
		MlTensor T;
		T.Dtype = MlDtype::Float32;
		T.F32 = V->Values;
		return T;
	}
	return std::nullopt;
}

std::optional<std::string> FormatMlCell(const MlTensor &T) {
	switch(T.Dtype) {
	case MlDtype::Int8: {
		QuantizedSeq Q{T.I8Scale, T.I8};
		return FormatQuantizedCell(Q);
	}
	case MlDtype::ComplexInt8: {
		QuantizedSeq Q{T.I8Scale, T.I8};
		if(T.IsMatrix) {
			std::ostringstream O;
			O << "Q8TC[" << T.MatRows << ',' << T.MatCols << "]:" << Q.Scale;
			for(std::int8_t C : Q.Codes)
				O << ',' << static_cast<int>(C);
			return std::move(O).str();
		}
		return FormatQuantizedComplexCell(Q);
	}
	case MlDtype::Int4: {
		QuantizedInt4Pack Q{T.I8Scale, T.I4};
		const std::size_t Decl = T.I4.size() + 1;
		return FormatQuantizedInt4Cell(Q, Decl, false);
	}
	case MlDtype::ComplexInt4: {
		QuantizedInt4Pack Q{T.I8Scale, T.I4};
		if(T.IsMatrix) {
			std::ostringstream O;
			O << "Q4TC[" << T.MatRows << ',' << T.MatCols << "]:" << Q.Scale;
			for(std::uint8_t B : Q.Bytes)
				O << ',' << static_cast<int>(B);
			return std::move(O).str();
		}
		const std::size_t Decl = T.I4.size() + 1;
		return FormatQuantizedInt4Cell(Q, Decl, true);
	}
	case MlDtype::Float16: {
		std::ostringstream O;
		O << "F16[" << T.F16.size() << "]:";
		for(std::size_t I = 0; I < T.F16.size(); ++I) {
			if(I)
				O << ',';
			O << static_cast<int>(T.F16[I]);
		}
		return std::move(O).str();
	}
	case MlDtype::Float64: {
		std::ostringstream O;
		O << "D[" << T.F64.size() << "]:" << FormatCommaDoubles(T.F64);
		return std::move(O).str();
	}
	case MlDtype::Complex16: {
		std::ostringstream O;
		O << "CH[" << T.ComplexSlots() << "]:";
		for(std::size_t I = 0; I < T.F16.size(); ++I) {
			if(I)
				O << ',';
			O << static_cast<int>(T.F16[I]);
		}
		return std::move(O).str();
	}
	case MlDtype::Complex128: {
		std::ostringstream O;
		O << "CD[" << T.ComplexSlots() << "]:" << FormatCommaDoubles(T.F64);
		return std::move(O).str();
	}
	case MlDtype::Complex64: {
		if(T.IsMatrix) {
			std::ostringstream O;
			O << "TC[" << T.MatRows << ',' << T.MatCols << "]:" << FormatCommaDoubles(InterleavedF32ToF64(T.F32));
			return std::move(O).str();
		}
		MathSciComplex::NumericVec V;
		V.Kind = MathSciComplex::NumericKind::Complex;
		V.Values = T.F32;
		return MathSciComplex::FormatNumericVec(V);
	}
	case MlDtype::Float32:
		if(T.IsMatrix) {
			std::ostringstream O;
			O << "T[" << T.MatRows << ',' << T.MatCols << "]:" << FormatCommaDoubles(InterleavedF32ToF64(T.F32));
			return std::move(O).str();
		}
		return MathSciSimdUtil::FormatListCellFromDoubles(InterleavedF32ToF64(T.F32));
	}
	return std::nullopt;
}

std::optional<MlTensor> CastMlTensor(const MlTensor &Src, MlDtype Dst) {
	const bool DstComplex = Dst == MlDtype::Complex64 || Dst == MlDtype::Complex128 || Dst == MlDtype::Complex16 ||
	                        Dst == MlDtype::ComplexInt8 || Dst == MlDtype::ComplexInt4;
	if(!Src.IsComplex() && DstComplex) {
		MlTensor Base = Src;
		if(Src.Dtype != MlDtype::Float32) {
			const auto Up = CastMlTensor(Src, MlDtype::Float32);
			if(!Up)
				return std::nullopt;
			Base = *Up;
		}
		std::vector<float> C;
		C.reserve(Base.F32.size() * 2);
		for(float X : Base.F32) {
			C.push_back(X);
			C.push_back(0.f);
		}
		Base.F32 = std::move(C);
		Base.Dtype = MlDtype::Complex64;
		if(Dst == MlDtype::Complex64)
			return Base;
		return CastMlTensor(Base, Dst);
	}
	if(Src.IsComplex() &&
	   (Dst == MlDtype::Float32 || Dst == MlDtype::Float64 || Dst == MlDtype::Float16 || Dst == MlDtype::Int8))
		return std::nullopt;
	MlTensor Out = Src;
	Out.Dtype = Dst;
	Out.I8Scale = 1.f;
	Out.IsMatrix = Src.IsMatrix;
	Out.MatRows = Src.MatRows;
	Out.MatCols = Src.MatCols;
	switch(Src.Dtype) {
	case MlDtype::Float32:
		break;
	case MlDtype::Float64:
		Out.F32 = InterleavedF64ToF32(Src.F64);
		Out.F64.clear();
		break;
	case MlDtype::Float16:
		Out.F32 = Fp16BitsToFp32(Src.F16.data(), Src.F16.size());
		Out.F16.clear();
		break;
	case MlDtype::Int8: {
		const auto Dq = DequantizeInt8F32({Src.I8Scale, Src.I8});
		Out.F32 = std::move(Dq);
		Out.I8.clear();
		break;
	}
	case MlDtype::Complex64:
		break;
	case MlDtype::Complex128:
		Out.F32 = InterleavedF64ToF32(Src.F64);
		Out.F64.clear();
		break;
	case MlDtype::Complex16:
		Out.F32 = Fp16BitsToFp32(Src.F16.data(), Src.F16.size());
		Out.F16.clear();
		break;
	case MlDtype::ComplexInt8: {
		const auto Dq = DequantizeInt8F32({Src.I8Scale, Src.I8});
		Out.F32 = std::move(Dq);
		Out.I8.clear();
		break;
	}
	case MlDtype::Int4: {
		const std::size_t FloatCount = Src.I4.size() * 2;
		const auto Dq = DequantizeInt4F32({Src.I8Scale, Src.I4}, FloatCount);
		Out.F32 = std::move(Dq);
		Out.I4.clear();
		break;
	}
	case MlDtype::ComplexInt4: {
		const std::size_t FloatCount = Src.ComplexSlots() * 2;
		const auto Dq = DequantizeInt4F32({Src.I8Scale, Src.I4}, FloatCount);
		Out.F32 = std::move(Dq);
		Out.I4.clear();
		break;
	}
	}
	switch(Dst) {
	case MlDtype::Float32:
		return Out;
	case MlDtype::Float64:
		Out.F64 = InterleavedF32ToF64(Out.F32);
		Out.F32.clear();
		Out.Dtype = MlDtype::Float64;
		return Out;
	case MlDtype::Float16:
		Out.F16 = Fp32ToFp16Bits(Out.F32.data(), Out.F32.size());
		Out.F32.clear();
		Out.Dtype = MlDtype::Float16;
		return Out;
	case MlDtype::Int8: {
		const auto Q = QuantizeInt8F32(Out.F32.data(), Out.F32.size());
		Out.I8 = std::move(Q.Codes);
		Out.I8Scale = Q.Scale;
		Out.F32.clear();
		Out.Dtype = MlDtype::Int8;
		return Out;
	}
	case MlDtype::Complex64:
		Out.Dtype = MlDtype::Complex64;
		return Out;
	case MlDtype::Complex128:
		Out.F64 = InterleavedF32ToF64(Out.F32);
		Out.F32.clear();
		Out.Dtype = MlDtype::Complex128;
		return Out;
	case MlDtype::Complex16:
		Out.F16 = Fp32ToFp16Bits(Out.F32.data(), Out.F32.size());
		Out.F32.clear();
		Out.Dtype = MlDtype::Complex16;
		return Out;
	case MlDtype::ComplexInt8: {
		const auto Q = QuantizeInt8F32(Out.F32.data(), Out.F32.size());
		Out.I8 = std::move(Q.Codes);
		Out.I8Scale = Q.Scale;
		Out.F32.clear();
		Out.Dtype = MlDtype::ComplexInt8;
		return Out;
	}
	case MlDtype::Int4: {
		const auto Q = QuantizeInt4F32(Out.F32.data(), Out.F32.size());
		Out.I4 = std::move(Q.Bytes);
		Out.I8Scale = Q.Scale;
		Out.F32.clear();
		Out.Dtype = MlDtype::Int4;
		return Out;
	}
	case MlDtype::ComplexInt4: {
		const auto Q = QuantizeInt4F32(Out.F32.data(), Out.F32.size());
		Out.I4 = std::move(Q.Bytes);
		Out.I8Scale = Q.Scale;
		Out.F32.clear();
		Out.Dtype = MlDtype::ComplexInt4;
		return Out;
	}
	}
	return std::nullopt;
}

std::optional<MlDtype> ParseMixedPrecMode(std::string_view ModeCell, bool SrcComplex) {
	const auto D = ParseDtypeName(ModeCell);
	if(D)
		return D;
	const std::string Mode(ModeCell);
	if(!SrcComplex) {
		if(Mode == "fp16" || Mode == "FP16" || Mode == "f16" || Mode == "1")
			return MlDtype::Float16;
		if(Mode == "fp32" || Mode == "FP32" || Mode == "f32" || Mode == "0")
			return MlDtype::Float32;
		if(Mode == "fp64" || Mode == "FP64" || Mode == "f64")
			return MlDtype::Float64;
	} else {
		if(Mode == "cf16" || Mode == "CF16" || Mode == "complex16" || Mode == "complexhalf")
			return MlDtype::Complex16;
		if(Mode == "cf32" || Mode == "CF32" || Mode == "complex64" || Mode == "complexfloat")
			return MlDtype::Complex64;
		if(Mode == "cf64" || Mode == "CF64" || Mode == "complex128" || Mode == "complexdouble")
			return MlDtype::Complex128;
	}
	return std::nullopt;
}

std::optional<QuantizedMlp> DeserializeQuantizedModel(std::string_view Cell) {
	if(Cell.size() < 5 || Cell.substr(0, 4) != "MQ8:")
		return std::nullopt;
	const auto Bytes = HexDecode(Cell.substr(4));
	if(!Bytes || Bytes->size() < 8)
		return std::nullopt;
	const std::uint8_t *P = Bytes->data();
	const std::uint8_t *End = Bytes->data() + Bytes->size();
	const auto Magic = ReadU32(P, End);
	const auto Ver = ReadU16(P, End);
	const auto NumLayers = ReadU16(P, End);
	if(!Magic || !Ver || !NumLayers || *Magic != QuantModelMagic || *Ver != QuantModelVersion ||
	   *NumLayers > MathSciModel::MaxModelLayers)
		return std::nullopt;
	QuantizedMlp Model;
	Model.Layers.reserve(*NumLayers);
	for(std::uint16_t L = 0; L < *NumLayers; ++L) {
		const auto Rows = ReadU32(P, End);
		const auto Cols = ReadU32(P, End);
		if(P >= End)
			return std::nullopt;
		const std::uint8_t Act = *P++;
		if(P + sizeof(float) > End)
			return std::nullopt;
		float Scale = 0.f;
		Simd::Memcpy(&Scale, P, sizeof(float));
		P += sizeof(float);
		if(!Rows || !Cols)
			return std::nullopt;
		const std::size_t Count = static_cast<std::size_t>(*Rows) * static_cast<std::size_t>(*Cols);
		if(static_cast<std::size_t>(End - P) < Count)
			return std::nullopt;
		QuantizedLayer LayerRow;
		LayerRow.Rows = static_cast<std::size_t>(*Rows);
		LayerRow.Cols = static_cast<std::size_t>(*Cols);
		LayerRow.Act = Act;
		LayerRow.Scale = Scale;
		LayerRow.Weights.resize(Count);
		for(std::size_t I = 0; I < Count; ++I)
			LayerRow.Weights[I] = static_cast<std::int8_t>(*P++);
		Model.Layers.push_back(std::move(LayerRow));
	}
	return Model;
}

std::string SerializeQuantizedModel(const QuantizedMlp &Model) {
	std::vector<std::uint8_t> Buf;
	Buf.reserve(64 + Model.Layers.size() * 16);
	AppendU32(Buf, QuantModelMagic);
	AppendU16(Buf, QuantModelVersion);
	AppendU16(Buf, static_cast<std::uint16_t>(Model.Layers.size()));
	for(const QuantizedLayer &L : Model.Layers) {
		AppendU32(Buf, static_cast<std::uint32_t>(L.Rows));
		AppendU32(Buf, static_cast<std::uint32_t>(L.Cols));
		Buf.push_back(L.Act);
		const auto *ScaleBytes = reinterpret_cast<const std::uint8_t *>(&L.Scale);
		Buf.insert(Buf.end(), ScaleBytes, ScaleBytes + sizeof(float));
		for(std::int8_t W : L.Weights)
			Buf.push_back(static_cast<std::uint8_t>(W));
	}
	std::ostringstream O;
	O << "MQ8:" << HexEncode(Buf.data(), Buf.size());
	return std::move(O).str();
}

std::optional<std::vector<float>> ForwardQuantizedF32(const QuantizedMlp &Model, const std::vector<float> &Input) {
	if(Model.Layers.empty() || Input.empty())
		return std::nullopt;
	std::vector<float> Cur = Input;
	for(const QuantizedLayer &L : Model.Layers) {
		if(Cur.size() != L.Cols)
			return std::nullopt;
		std::vector<float> Wf(L.Weights.size());
		for(std::size_t I = 0; I < L.Weights.size(); ++I)
			Wf[I] = DequantizeSymF32(L.Weights[I], L.Scale);
		std::vector<float> Out(L.Rows, 0.f);
		for(std::size_t O = 0; O < L.Rows; ++O) {
			float Acc = 0.f;
			for(std::size_t I = 0; I < L.Cols; ++I)
				Acc += Wf[O * L.Cols + I] * Cur[I];
			Out[O] = Acc;
		}
		ApplyActivation(static_cast<MathSciModel::Activation>(L.Act), Out.data(), Out.size());
		Cur = std::move(Out);
	}
	return Cur;
}

std::optional<std::string> QuantizeInt8CellFromReal(std::string_view SeqCell) {
	const auto T = ParseMlCell(SeqCell);
	if(!T || T->Dtype == MlDtype::Int8 || T->Dtype == MlDtype::ComplexInt8 || T->Dtype == MlDtype::Int4 ||
	   T->Dtype == MlDtype::ComplexInt4)
		return std::nullopt;
	if(const auto Q = CastMlTensor(*T, T->IsComplex() ? MlDtype::ComplexInt8 : MlDtype::Int8))
		return FormatMlCell(*Q);
	return std::nullopt;
}

std::optional<std::string> DequantizeInt8CellFromReal(std::string_view QuantCell) {
	const auto T = ParseMlCell(QuantCell);
	if(!T || (T->Dtype != MlDtype::Int8 && T->Dtype != MlDtype::ComplexInt8 && T->Dtype != MlDtype::Int4 &&
	          T->Dtype != MlDtype::ComplexInt4))
		return std::nullopt;
	const auto Dst = T->IsComplex() ? MlDtype::Complex64 : MlDtype::Float32;
	if(const auto Out = CastMlTensor(*T, Dst))
		return FormatMlCell(*Out);
	return std::nullopt;
}

std::optional<std::string> QuantizeModelCellFromReal(std::string_view ModelCell) {
	const auto Model = MathSciModel::Deserialize(ModelCell);
	if(!Model)
		return std::nullopt;
	QuantizedMlp Qm;
	Qm.Layers.reserve(Model->Layers.size());
	for(const auto &L : Model->Layers) {
		QuantizedLayer Ql;
		Ql.Rows = L.Rows;
		Ql.Cols = L.Cols;
		Ql.Act = static_cast<std::uint8_t>(L.Act);
		const auto Qs = QuantizeInt8F32(L.Weights.data(), L.Weights.size());
		Ql.Scale = Qs.Scale;
		Ql.Weights = std::move(Qs.Codes);
		Qm.Layers.push_back(std::move(Ql));
	}
	return SerializeQuantizedModel(Qm);
}

std::optional<std::string> DequantizeModelCellFromReal(std::string_view QuantCell) {
	const auto Qm = DeserializeQuantizedModel(QuantCell);
	if(!Qm)
		return std::nullopt;
	MathSciModel::MlpModel Model;
	Model.Layers.reserve(Qm->Layers.size());
	for(const auto &Ql : Qm->Layers) {
		MathSciModel::Layer L;
		L.Rows = Ql.Rows;
		L.Cols = Ql.Cols;
		L.Act = static_cast<MathSciModel::Activation>(Ql.Act);
		L.Weights = DequantizeInt8F32({Ql.Scale, Ql.Weights});
		Model.Layers.push_back(std::move(L));
	}
	return MathSciModel::SerializeBinary(Model);
}

std::optional<std::string> MixedPrecCellFromReal(std::string_view SeqCell, std::string_view ModeCell) {
	const auto T = ParseMlCell(SeqCell);
	if(!T)
		return std::nullopt;
	const auto Dst = ParseMixedPrecMode(ModeCell, T->IsComplex());
	if(!Dst)
		return std::nullopt;
	if(const auto Out = CastMlTensor(*T, *Dst))
		return FormatMlCell(*Out);
	return std::nullopt;
}

std::optional<std::string> MixedPrecDequantCellFromReal(std::string_view F16Cell) {
	const auto T = ParseMlCell(F16Cell);
	if(!T)
		return std::nullopt;
	const auto Dst = T->IsComplex() ? MlDtype::Complex64 : MlDtype::Float32;
	if(const auto Out = CastMlTensor(*T, Dst))
		return FormatMlCell(*Out);
	return std::nullopt;
}

std::optional<std::string> MixedPrecModelCellFromReal(std::string_view ModelCell, std::string_view ModeCell) {
	const std::string Mode(ModeCell);
	const bool ToF16 = Mode == "fp16" || Mode == "FP16" || Mode == "f16" || Mode == "1";
	if(!ToF16 && Mode != "fp32" && Mode != "FP32" && Mode != "f32" && Mode != "0")
		return std::nullopt;
	if(ModelCell.rfind("MF16:", 0) == 0 && !ToF16) {
		const auto Bytes = HexDecode(ModelCell.substr(5));
		if(!Bytes || Bytes->size() < 8)
			return std::nullopt;
		const std::uint8_t *P = Bytes->data();
		const std::uint8_t *End = Bytes->data() + Bytes->size();
		const auto Magic = ReadU32(P, End);
		const auto Ver = ReadU16(P, End);
		const auto NumLayers = ReadU16(P, End);
		if(!Magic || !Ver || !NumLayers || *Magic != Fp16ModelMagic || *Ver != Fp16ModelVersion ||
		   *NumLayers > MathSciModel::MaxModelLayers)
			return std::nullopt;
		MathSciModel::MlpModel Model;
		Model.Layers.reserve(*NumLayers);
		for(std::uint16_t L = 0; L < *NumLayers; ++L) {
			const auto Rows = ReadU32(P, End);
			const auto Cols = ReadU32(P, End);
			if(P >= End)
				return std::nullopt;
			const std::uint8_t Act = *P++;
			if(!Rows || !Cols)
				return std::nullopt;
			const std::size_t Count = static_cast<std::size_t>(*Rows) * static_cast<std::size_t>(*Cols);
			if(static_cast<std::size_t>(End - P) < Count * sizeof(std::uint16_t))
				return std::nullopt;
			std::vector<std::uint16_t> Bits(Count);
			Simd::Memcpy(Bits.data(), P, Count * sizeof(std::uint16_t));
			P += Count * sizeof(std::uint16_t);
			MathSciModel::Layer LayerRow;
			LayerRow.Rows = static_cast<std::size_t>(*Rows);
			LayerRow.Cols = static_cast<std::size_t>(*Cols);
			LayerRow.Act = static_cast<MathSciModel::Activation>(Act);
			LayerRow.Weights = Fp16BitsToFp32(Bits.data(), Bits.size());
			Model.Layers.push_back(std::move(LayerRow));
		}
		return MathSciModel::SerializeBinary(Model);
	}
	const auto Model = MathSciModel::Deserialize(ModelCell);
	if(!Model || !ToF16)
		return std::nullopt;
	std::vector<std::uint8_t> Buf;
	Buf.reserve(64 + Model->Layers.size() * 16);
	AppendU32(Buf, Fp16ModelMagic);
	AppendU16(Buf, Fp16ModelVersion);
	AppendU16(Buf, static_cast<std::uint16_t>(Model->Layers.size()));
	for(const MathSciModel::Layer &L : Model->Layers) {
		AppendU32(Buf, static_cast<std::uint32_t>(L.Rows));
		AppendU32(Buf, static_cast<std::uint32_t>(L.Cols));
		Buf.push_back(static_cast<std::uint8_t>(L.Act));
		const auto Bits = Fp32ToFp16Bits(L.Weights.data(), L.Weights.size());
		const auto *Raw = reinterpret_cast<const std::uint8_t *>(Bits.data());
		Buf.insert(Buf.end(), Raw, Raw + Bits.size() * sizeof(std::uint16_t));
	}
	std::ostringstream O;
	O << "MF16:" << HexEncode(Buf.data(), Buf.size());
	return std::move(O).str();
}

std::optional<std::string> PredictQuantModelCellFromReal(std::string_view QuantCell, std::string_view InputCell) {
	const auto Qm = DeserializeQuantizedModel(QuantCell);
	const auto R = ParseRealSeq(InputCell);
	if(!Qm || !R || R->empty())
		return std::nullopt;
	const auto F = ToF32(*R);
	const auto Out = ForwardQuantizedF32(*Qm, F);
	if(!Out)
		return std::nullopt;
	return MathSciSimdUtil::FormatListCellFromDoubles(MathSciSimdUtil::ToF64(*Out));
}

std::optional<std::string> PruneCellFromReal(std::string_view SeqCell, std::string_view ThresholdCell) {
	const auto T = ParseNum(ThresholdCell);
	const auto In = ParseMlCell(SeqCell);
	if(!T || !In)
		return std::nullopt;
	MlTensor Out = *In;
	const float Th = static_cast<float>(*T);
	if(In->IsComplex()) {
		std::vector<float> F32;
		if(In->Dtype == MlDtype::Complex64)
			F32 = In->F32;
		else if(const auto Up = CastMlTensor(*In, MlDtype::Complex64))
			F32 = Up->F32;
		else
			return std::nullopt;
		Out.Dtype = MlDtype::Complex64;
		Out.F32 = PruneComplexMagnitudesF32(F32.data(), F32.size() / 2, Th);
	} else {
		std::vector<float> F32;
		if(In->Dtype == MlDtype::Float32)
			F32 = In->F32;
		else if(const auto Up = CastMlTensor(*In, MlDtype::Float32))
			F32 = Up->F32;
		else
			return std::nullopt;
		Out.Dtype = MlDtype::Float32;
		Out.F32 = PruneMagnitudesF32(F32.data(), F32.size(), Th);
	}
	return FormatMlCell(Out);
}

std::optional<std::string> PruneModelCellFromReal(std::string_view ModelCell, std::string_view ThresholdCell) {
	auto Model = MathSciModel::Deserialize(ModelCell);
	const auto T = ParseNum(ThresholdCell);
	if(!Model || !T)
		return std::nullopt;
	for(auto &L : Model->Layers) {
		const auto P = PruneMagnitudesF32(L.Weights.data(), L.Weights.size(), static_cast<float>(*T));
		L.Weights = std::move(P);
	}
	return MathSciModel::SerializeBinary(*Model);
}

std::optional<std::string> PosEncCellFromReal(std::string_view PosCell, std::string_view DimCell) {
	const auto P = ParseNum(PosCell);
	const auto D = ParseNum(DimCell);
	if(!P || !D || *P < 0 || *D <= 0 || static_cast<std::size_t>(*D) > MaxPosEncDim)
		return std::nullopt;
	const auto Out = SinusoidalPosEncF32(static_cast<std::size_t>(*P), static_cast<std::size_t>(*D));
	return MathSciSimdUtil::FormatListCellFromDoubles(MathSciSimdUtil::ToF64(Out));
}

std::optional<std::string> PosEncSequenceCellFromReal(std::string_view LenCell, std::string_view DimCell) {
	const auto L = ParseNum(LenCell);
	const auto D = ParseNum(DimCell);
	if(!L || !D || *L <= 0 || *D <= 0 || static_cast<std::size_t>(*L) * static_cast<std::size_t>(*D) > MaxMlLen)
		return std::nullopt;
	const auto Out =
	    SinusoidalPosEncSequenceF32(static_cast<std::size_t>(*L), static_cast<std::size_t>(*D));
	return MathSciSimdUtil::FormatListCellFromDoubles(MathSciSimdUtil::ToF64(Out));
}

std::optional<std::string> PosEncAddCellFromReal(std::string_view SeqCell, std::string_view DimCell) {
	const auto D = ParseNum(DimCell);
	const auto In = ParseMlCell(SeqCell);
	if(!D || !In || *D <= 0)
		return std::nullopt;
	if(In->IsComplex()) {
		const std::size_t Slots = static_cast<std::size_t>(*D);
		std::vector<float> F32;
		if(In->Dtype == MlDtype::Complex64)
			F32 = In->F32;
		else if(const auto Up = CastMlTensor(*In, MlDtype::Complex64))
			F32 = Up->F32;
		else
			return std::nullopt;
		if(F32.size() % (Slots * 2) != 0)
			return std::nullopt;
		const std::size_t Len = F32.size() / (Slots * 2);
		const auto Out = AddComplexPosEncSequenceF32(F32.data(), Len, Slots);
		MlTensor T;
		T.Dtype = MlDtype::Complex64;
		T.F32 = std::move(Out);
		return FormatMlCell(T);
	}
	const auto R = ParseRealSeq(SeqCell);
	if(!R || R->empty() || static_cast<std::size_t>(*D) > MaxPosEncDim)
		return std::nullopt;
	if(R->size() % static_cast<std::size_t>(*D) != 0)
		return std::nullopt;
	const std::size_t Len = R->size() / static_cast<std::size_t>(*D);
	const auto F = ToF32(*R);
	const auto Out = AddPosEncSequenceF32(F.data(), Len, static_cast<std::size_t>(*D));
	return MathSciSimdUtil::FormatListCellFromDoubles(MathSciSimdUtil::ToF64(Out));
}

std::optional<std::string> PosEncComplexCellFromReal(std::string_view PosCell, std::string_view SlotsCell) {
	const auto P = ParseNum(PosCell);
	const auto S = ParseNum(SlotsCell);
	if(!P || !S || *P < 0 || *S <= 0 || static_cast<std::size_t>(*S) > MaxPosEncDim)
		return std::nullopt;
	const auto Out = SinusoidalComplexPosEncF32(static_cast<std::size_t>(*P), static_cast<std::size_t>(*S));
	MlTensor T;
	T.Dtype = MlDtype::Complex64;
	T.F32 = std::move(Out);
	return FormatMlCell(T);
}

std::optional<std::string> PosEncComplexAddCellFromReal(std::string_view SeqCell, std::string_view SlotsCell) {
	const auto S = ParseNum(SlotsCell);
	const auto In = ParseMlCell(SeqCell);
	if(!S || !In || *S <= 0 || !In->IsComplex())
		return std::nullopt;
	const std::size_t Slots = static_cast<std::size_t>(*S);
	std::vector<float> F32;
	if(In->Dtype == MlDtype::Complex64)
		F32 = In->F32;
	else if(const auto Up = CastMlTensor(*In, MlDtype::Complex64))
		F32 = Up->F32;
	else
		return std::nullopt;
	if(F32.size() % (Slots * 2) != 0)
		return std::nullopt;
	const std::size_t Len = F32.size() / (Slots * 2);
	const auto Out = AddComplexPosEncSequenceF32(F32.data(), Len, Slots);
	MlTensor T;
	T.Dtype = MlDtype::Complex64;
	T.F32 = std::move(Out);
	return FormatMlCell(T);
}

std::optional<std::string> DtypeCellFromReal(std::string_view Cell) {
	const auto T = ParseMlCell(Cell);
	if(!T)
		return std::nullopt;
	return std::string(DtypeName(T->Dtype));
}

std::optional<std::string> CastCellFromReal(std::string_view Cell, std::string_view DtypeCell) {
	const auto T = ParseMlCell(Cell);
	const auto D = ParseDtypeName(DtypeCell);
	if(!T || !D)
		return std::nullopt;
	if(const auto Out = CastMlTensor(*T, *D))
		return FormatMlCell(*Out);
	return std::nullopt;
}

} // namespace MathSciMl
} // namespace AstralDB
