#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {
namespace MathSciMl {

constexpr std::size_t MaxMlLen = 1u << 20;
constexpr std::size_t MaxPosEncDim = 4096;

/** NumPy-aligned ML dtypes (real + complex). */
enum class MlDtype : std::uint8_t {
	Float32 = 0,
	Float64 = 1,
	Float16 = 2,
	Int8 = 3,
	Complex64 = 4,
	Complex128 = 5,
	Complex16 = 6,
	ComplexInt8 = 7,
	Int4 = 8,
	ComplexInt4 = 9,
};

enum class PrecTag : std::uint8_t { Fp32 = 0, Fp16 = 1 };

struct QuantizedSeq {
	float Scale = 1.f;
	std::vector<std::int8_t> Codes;
};

struct QuantizedLayer {
	std::size_t Rows = 0;
	std::size_t Cols = 0;
	std::uint8_t Act = 0;
	float Scale = 1.f;
	std::vector<std::int8_t> Weights;
};

struct QuantizedMlp {
	std::vector<QuantizedLayer> Layers;
};

/** Unified real/complex tensor for ML memory ops. */
struct MlTensor {
	MlDtype Dtype = MlDtype::Float32;
	float I8Scale = 1.f;
	bool IsMatrix = false;
	std::size_t MatRows = 0;
	std::size_t MatCols = 0;
	std::vector<float> F32;
	std::vector<double> F64;
	std::vector<std::uint16_t> F16;
	std::vector<std::int8_t> I8;
	std::vector<std::uint8_t> I4;

	bool IsComplex() const {
		return Dtype == MlDtype::Complex64 || Dtype == MlDtype::Complex128 || Dtype == MlDtype::Complex16 ||
		       Dtype == MlDtype::ComplexInt8 || Dtype == MlDtype::ComplexInt4;
	}
	std::size_t ComplexSlots() const {
		if(!IsComplex())
			return 0;
		if(Dtype == MlDtype::Complex128)
			return F64.size() / 2;
		if(Dtype == MlDtype::ComplexInt8)
			return I8.size() / 2;
		if(Dtype == MlDtype::ComplexInt4)
			return I4.size();
		if(Dtype == MlDtype::Complex16)
			return F16.size() / 2;
		return F32.size() / 2;
	}
	std::size_t RealLen() const {
		switch(Dtype) {
		case MlDtype::Float64:
			return F64.size();
		case MlDtype::Float16:
			return F16.size();
		case MlDtype::Int8:
			return I8.size();
		case MlDtype::Int4:
			return I4.size() * 2;
		default:
			return F32.size();
		}
	}
};

const char *DtypeName(MlDtype D);
std::optional<MlDtype> ParseDtypeName(std::string_view Name);

std::int8_t QuantizeSymF32(float Value, float Scale);
float DequantizeSymF32(std::int8_t Code, float Scale);
float QuantizeScaleSymF32(const float *Values, std::size_t N);

QuantizedSeq QuantizeInt8F32(const float *Values, std::size_t N);
std::vector<float> DequantizeInt8F32(const QuantizedSeq &Q);

std::uint16_t Fp32ToFp16Bits(float Value);
float Fp16BitsToFp32(std::uint16_t Bits);
std::vector<std::uint16_t> Fp32ToFp16Bits(const float *Values, std::size_t N);
std::vector<float> Fp16BitsToFp32(const std::uint16_t *Bits, std::size_t N);

std::vector<float> PruneMagnitudesF32(const float *Values, std::size_t N, float Threshold);
std::vector<float> PruneComplexMagnitudesF32(const float *Interleaved, std::size_t Slots, float Threshold);
std::size_t CountPrunedZerosF32(const float *Values, std::size_t N);

std::vector<float> SinusoidalPosEncF32(std::size_t Pos, std::size_t Dim, float Base = 10000.f);
std::vector<float> SinusoidalComplexPosEncF32(std::size_t Pos, std::size_t Slots, float Base = 10000.f);
std::vector<float> SinusoidalPosEncSequenceF32(std::size_t Len, std::size_t Dim, float Base = 10000.f);
std::vector<float> AddPosEncSequenceF32(const float *Seq, std::size_t Len, std::size_t Dim, float Base = 10000.f);
std::vector<float> AddComplexPosEncSequenceF32(const float *Seq, std::size_t Len, std::size_t Slots, float Base = 10000.f);

std::optional<QuantizedSeq> ParseQuantizedCell(std::string_view Cell);
std::optional<QuantizedSeq> ParseQuantizedComplexCell(std::string_view Cell);
std::string FormatQuantizedCell(const QuantizedSeq &Q);
std::string FormatQuantizedComplexCell(const QuantizedSeq &Q);

std::optional<MlTensor> ParseMlCell(std::string_view Cell);
std::optional<std::string> FormatMlCell(const MlTensor &T);
std::optional<MlTensor> CastMlTensor(const MlTensor &Src, MlDtype Dst);

std::optional<QuantizedMlp> DeserializeQuantizedModel(std::string_view Cell);
std::string SerializeQuantizedModel(const QuantizedMlp &Model);
std::optional<std::vector<float>> ForwardQuantizedF32(const QuantizedMlp &Model, const std::vector<float> &Input);

std::optional<std::string> QuantizeInt8CellFromReal(std::string_view SeqCell);
std::optional<std::string> DequantizeInt8CellFromReal(std::string_view QuantCell);
std::optional<std::string> QuantizeModelCellFromReal(std::string_view ModelCell);
std::optional<std::string> DequantizeModelCellFromReal(std::string_view QuantCell);
std::optional<std::string> MixedPrecCellFromReal(std::string_view SeqCell, std::string_view ModeCell);
std::optional<std::string> MixedPrecDequantCellFromReal(std::string_view F16Cell);
std::optional<std::string> MixedPrecModelCellFromReal(std::string_view ModelCell, std::string_view ModeCell);
std::optional<std::string> PredictQuantModelCellFromReal(std::string_view QuantCell, std::string_view InputCell);
std::optional<std::string> PruneCellFromReal(std::string_view SeqCell, std::string_view ThresholdCell);
std::optional<std::string> PruneModelCellFromReal(std::string_view ModelCell, std::string_view ThresholdCell);
std::optional<std::string> PosEncCellFromReal(std::string_view PosCell, std::string_view DimCell);
std::optional<std::string> PosEncSequenceCellFromReal(std::string_view LenCell, std::string_view DimCell);
std::optional<std::string> PosEncAddCellFromReal(std::string_view SeqCell, std::string_view DimCell);
std::optional<std::string> PosEncComplexCellFromReal(std::string_view PosCell, std::string_view SlotsCell);
std::optional<std::string> PosEncComplexAddCellFromReal(std::string_view SeqCell, std::string_view SlotsCell);
std::optional<std::string> DtypeCellFromReal(std::string_view Cell);
std::optional<std::string> CastCellFromReal(std::string_view Cell, std::string_view DtypeCell);

} // namespace MathSciMl
} // namespace AstralDB
