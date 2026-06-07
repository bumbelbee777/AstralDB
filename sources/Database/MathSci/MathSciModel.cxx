#include <Database/MathSci/MathSciModel.hxx>

#include <Database/Types/AdvancedTypes.hxx>
#include <Database/MathSci/MathSciComplex.hxx>
#include <Database/MathSci/MathSciSimdUtil.hxx>
#include <IO/SIMD.hxx>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <list>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace AstralDB {
namespace MathSciModel {
namespace {

constexpr std::uint32_t BinaryMagic = 0x0031534Du; // 'MS1\0'
constexpr std::uint16_t BinaryVersion = 1;

std::optional<double> ParseFloat(std::string_view S) {
	if(S.empty())
		return std::nullopt;
	try {
		return std::stod(std::string(S));
	} catch(...) {
		return std::nullopt;
	}
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

void ApplyActivation(Activation Act, float *V, std::size_t N) {
	switch(Act) {
	case Activation::Linear:
		break;
	case Activation::Sigmoid:
		for(std::size_t I = 0; I < N; ++I)
			V[I] = 1.f / (1.f + std::exp(-V[I]));
		break;
	case Activation::Tanh:
		for(std::size_t I = 0; I < N; ++I)
			V[I] = std::tanh(V[I]);
		break;
	case Activation::Relu:
		Simd::ReluF32(V, V, N);
		break;
	}
}

constexpr std::size_t MaxModelCacheEntries = 64;
std::unordered_map<std::uint64_t, CompiledMlp> g_ModelCache;
std::list<std::uint64_t> g_ModelCacheOrder;
std::unordered_map<std::uint64_t, std::list<std::uint64_t>::iterator> g_ModelCacheIter;

constexpr std::size_t MaxInputParseCacheEntries = 256;
std::unordered_map<std::uint64_t, std::vector<float>> g_InputParseCache;
std::list<std::uint64_t> g_InputParseOrder;
std::unordered_map<std::uint64_t, std::list<std::uint64_t>::iterator> g_InputParseIter;

std::uint64_t ModelCellKey(std::string_view Cell) {
	std::uint64_t H = 1469598103934665603ull;
	for(unsigned char C : Cell) {
		H ^= C;
		H *= 1099511628211ull;
	}
	return H;
}

void TouchModelCache(std::uint64_t Key) {
	const auto It = g_ModelCacheIter.find(Key);
	if(It != g_ModelCacheIter.end()) {
		g_ModelCacheOrder.splice(g_ModelCacheOrder.begin(), g_ModelCacheOrder, It->second);
		return;
	}
	g_ModelCacheOrder.push_front(Key);
	g_ModelCacheIter[Key] = g_ModelCacheOrder.begin();
	while(g_ModelCacheOrder.size() > MaxModelCacheEntries) {
		const std::uint64_t Old = g_ModelCacheOrder.back();
		g_ModelCacheOrder.pop_back();
		g_ModelCache.erase(Old);
		g_ModelCacheIter.erase(Old);
	}
}

const std::vector<float> &InputVecF32CacheLookup(std::string_view InputCell) {
	const std::uint64_t Key = ModelCellKey(InputCell);
	const auto Hit = g_InputParseCache.find(Key);
	if(Hit != g_InputParseCache.end()) {
		const auto It = g_InputParseIter.find(Key);
		if(It != g_InputParseIter.end())
			g_InputParseOrder.splice(g_InputParseOrder.begin(), g_InputParseOrder, It->second);
		return Hit->second;
	}
	const auto In = MathSciComplex::ParseNumericVec(InputCell);
	static const std::vector<float> kEmpty;
	if(!In || In->Kind != MathSciComplex::NumericKind::Real)
		return kEmpty;
	std::vector<float> Values = In->Values;
	auto [Ins, _] = g_InputParseCache.emplace(Key, std::move(Values));
	g_InputParseOrder.push_front(Key);
	g_InputParseIter[Key] = g_InputParseOrder.begin();
	while(g_InputParseOrder.size() > MaxInputParseCacheEntries) {
		const std::uint64_t Old = g_InputParseOrder.back();
		g_InputParseOrder.pop_back();
		g_InputParseCache.erase(Old);
		g_InputParseIter.erase(Old);
	}
	return Ins->second;
}

std::optional<MlpModel> DeserializeBinary(std::string_view HexBody) {
	const auto Bytes = HexDecode(HexBody);
	if(!Bytes || Bytes->size() < 8)
		return std::nullopt;
	const std::uint8_t *P = Bytes->data();
	const std::uint8_t *End = Bytes->data() + Bytes->size();
	const auto Magic = ReadU32(P, End);
	const auto Ver = ReadU16(P, End);
	const auto NumLayers = ReadU16(P, End);
	if(!Magic || !Ver || !NumLayers || *Magic != BinaryMagic || *Ver != BinaryVersion || *NumLayers == 0 ||
	   *NumLayers > MaxModelLayers)
		return std::nullopt;
	MlpModel Model;
	Model.Layers.reserve(*NumLayers);
	std::size_t TotalWeights = 0;
	for(std::uint16_t L = 0; L < *NumLayers; ++L) {
		const auto Rows = ReadU32(P, End);
		const auto Cols = ReadU32(P, End);
		if(P >= End)
			return std::nullopt;
		const Activation Act = static_cast<Activation>(*P++);
		if(P + 3 > End)
			return std::nullopt;
		P += 3;
		if(!Rows || !Cols || *Rows > MaxModelDim || *Cols > MaxModelDim)
			return std::nullopt;
		const std::size_t Count = static_cast<std::size_t>(*Rows) * static_cast<std::size_t>(*Cols);
		TotalWeights += Count;
		if(TotalWeights > MaxModelWeights)
			return std::nullopt;
		if(static_cast<std::size_t>(End - P) < Count * sizeof(float))
			return std::nullopt;
		Layer LayerRow;
		LayerRow.Rows = static_cast<std::size_t>(*Rows);
		LayerRow.Cols = static_cast<std::size_t>(*Cols);
		LayerRow.Act = Act;
		LayerRow.Weights.resize(Count);
		Simd::Memcpy(LayerRow.Weights.data(), P, Count * sizeof(float));
		P += Count * sizeof(float);
		Model.Layers.push_back(std::move(LayerRow));
	}
	if(Model.Layers.empty())
		return std::nullopt;
	return Model;
}

std::optional<MlpModel> DeserializeText(std::string_view Cell) {
	if(Cell.size() < 4 || Cell[0] != 'M' || Cell[1] != '[')
		return std::nullopt;
	const std::size_t Close = Cell.find(']');
	if(Close == std::string_view::npos || Close + 2 >= Cell.size() || Cell[Close + 1] != ':')
		return std::nullopt;
	const std::string_view Body = Cell.substr(Close + 2);
	std::vector<std::string> Parts;
	{
		size_t Pos = 0;
		while(Pos < Body.size()) {
			size_t End = Pos;
			while(End < Body.size() && Body[End] != ';')
				++End;
			Parts.emplace_back(Body.substr(Pos, End - Pos));
			Pos = End + (End < Body.size() ? 1 : 0);
		}
	}
	if(Parts.size() < 2)
		return std::nullopt;
	std::vector<std::string> ActNames;
	if(const auto Acts = AdvancedTypes::ParseListCell(Parts[0])) {
		ActNames = *Acts;
	} else {
		ActNames.push_back(std::string(Parts[0]));
	}
	std::vector<std::string> WeightCells(Parts.begin() + 1, Parts.end());
	return BuildFromWeightCells(WeightCells, ActNames);
}

} // namespace

std::optional<Activation> ParseActivation(std::string_view Name) {
	if(Name == "linear" || Name == "LINEAR" || Name == "none" || Name == "NONE")
		return Activation::Linear;
	if(Name == "sigmoid" || Name == "SIGMOID")
		return Activation::Sigmoid;
	if(Name == "tanh" || Name == "TANH")
		return Activation::Tanh;
	if(Name == "relu" || Name == "RELU")
		return Activation::Relu;
	return std::nullopt;
}

std::string ActivationName(Activation Act) {
	switch(Act) {
	case Activation::Linear:
		return "linear";
	case Activation::Sigmoid:
		return "sigmoid";
	case Activation::Tanh:
		return "tanh";
	case Activation::Relu:
		return "relu";
	}
	return "linear";
}

std::optional<MlpModel> BuildFromWeightCells(const std::vector<std::string> &WeightCells,
                                             const std::vector<std::string> &ActivationNames) {
	if(WeightCells.empty() || WeightCells.size() > MaxModelLayers)
		return std::nullopt;
	MlpModel Model;
	Model.Layers.reserve(WeightCells.size());
	std::size_t TotalWeights = 0;
	for(std::size_t I = 0; I < WeightCells.size(); ++I) {
		const auto Mat = MathSciComplex::ParseNumericMat(WeightCells[I]);
		if(!Mat || Mat->Kind != MathSciComplex::NumericKind::Real || Mat->Rows == 0 || Mat->Cols == 0)
			return std::nullopt;
		TotalWeights += Mat->Flat.size();
		if(TotalWeights > MaxModelWeights)
			return std::nullopt;
		Layer L;
		L.Rows = Mat->Rows;
		L.Cols = Mat->Cols;
		L.Weights = Mat->Flat;
		if(I < ActivationNames.size()) {
			const auto Act = ParseActivation(ActivationNames[I]);
			if(!Act)
				return std::nullopt;
			L.Act = *Act;
		} else {
			L.Act = (I + 1 == WeightCells.size()) ? Activation::Linear : Activation::Sigmoid;
		}
		Model.Layers.push_back(std::move(L));
	}
	return Model;
}

std::optional<MlpModel> Deserialize(std::string_view Cell) {
	if(Cell.size() >= 3 && Cell[0] == 'M' && Cell[1] == 'B' && Cell[2] == '1') {
		const std::size_t Colon = Cell.find(':');
		if(Colon == std::string_view::npos || Colon + 1 >= Cell.size())
			return std::nullopt;
		return DeserializeBinary(Cell.substr(Colon + 1));
	}
	return DeserializeText(Cell);
}

std::string SerializeBinary(const MlpModel &Model) {
	std::vector<std::uint8_t> Buf;
	Buf.reserve(64 + Model.Layers.size() * 16);
	AppendU32(Buf, BinaryMagic);
	AppendU16(Buf, BinaryVersion);
	AppendU16(Buf, static_cast<std::uint16_t>(Model.Layers.size()));
	for(const Layer &L : Model.Layers) {
		AppendU32(Buf, static_cast<std::uint32_t>(L.Rows));
		AppendU32(Buf, static_cast<std::uint32_t>(L.Cols));
		Buf.push_back(static_cast<std::uint8_t>(L.Act));
		Buf.push_back(0);
		Buf.push_back(0);
		Buf.push_back(0);
		const auto *W = reinterpret_cast<const std::uint8_t *>(L.Weights.data());
		Buf.insert(Buf.end(), W, W + L.Weights.size() * sizeof(float));
	}
	std::ostringstream O;
	O << "MB1:";
	O << HexEncode(Buf.data(), Buf.size());
	return std::move(O).str();
}

std::string SerializeText(const MlpModel &Model) {
	std::ostringstream O;
	O << "M[";
	for(std::size_t I = 0; I < Model.Layers.size(); ++I) {
		if(I)
			O << ',';
		O << ActivationName(Model.Layers[I].Act);
	}
	O << "]:";
	for(std::size_t I = 0; I < Model.Layers.size(); ++I) {
		if(I)
			O << ';';
		const Layer &L = Model.Layers[I];
		O << "T[" << L.Rows << ',' << L.Cols << "]:";
		for(std::size_t R = 0; R < L.Rows; ++R) {
			if(R)
				O << ' ';
			for(std::size_t C = 0; C < L.Cols; ++C) {
				if(C)
					O << ',';
				O << L.Weights[R * L.Cols + C];
			}
		}
	}
	return std::move(O).str();
}

std::string Fingerprint(const MlpModel &Model) {
	std::uint64_t H = 1469598103934665603ull;
	auto Mix = [&](std::uint64_t V) {
		H ^= V;
		H *= 1099511628211ull;
	};
	Mix(static_cast<std::uint64_t>(Model.Layers.size()));
	for(const Layer &L : Model.Layers) {
		Mix(static_cast<std::uint64_t>(L.Rows));
		Mix(static_cast<std::uint64_t>(L.Cols));
		Mix(static_cast<std::uint64_t>(L.Act));
		for(float W : L.Weights)
			Mix(static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(W)));
	}
	std::ostringstream O;
	O << std::hex << H;
	return std::move(O).str();
}

void CompiledMlp::EnsureBuffers(std::size_t Width) {
	if(Width <= MaxDim)
		return;
	MaxDim = Width;
	BufA.assign(Width, 0.f);
	BufB.assign(Width, 0.f);
}

std::optional<std::vector<float>> ForwardF32(const MlpModel &Model, const std::vector<float> &Input) {
	if(Model.Layers.empty())
		return std::nullopt;
	CompiledMlp Scratch;
	Scratch.Model = Model;
	Scratch.EnsureBuffers(Input.size());
	for(const Layer &L : Model.Layers)
		Scratch.EnsureBuffers(std::max(L.Rows, L.Cols));
	std::vector<float> Out(Model.Layers.back().Rows);
	if(!ForwardCompiledF32(Scratch, Input.data(), Out.data()))
		return std::nullopt;
	return Out;
}

bool ForwardCompiledF32(CompiledMlp &C, const float *Input, float *Output) {
	if(C.Model.Layers.empty())
		return false;
	const Layer &First = C.Model.Layers.front();
	if(First.Cols == 0)
		return false;
	C.EnsureBuffers(First.Cols);
	Simd::Memcpy(C.BufA.data(), Input, First.Cols * sizeof(float));
	float *Cur = C.BufA.data();
	float *Alt = C.BufB.data();
	for(const Layer &L : C.Model.Layers) {
		C.EnsureBuffers(std::max(L.Rows, L.Cols));
		Simd::MatrixVectorMulF32(L.Weights.data(), Cur, Alt, L.Rows, L.Cols);
		ApplyActivation(L.Act, Alt, L.Rows);
		std::swap(Cur, Alt);
	}
	const std::size_t OutDim = C.Model.Layers.back().Rows;
	Simd::Memcpy(Output, Cur, OutDim * sizeof(float));
	return true;
}

CompiledMlp &CompiledCacheLookup(std::string_view ModelCell) {
	const std::uint64_t Key = ModelCellKey(ModelCell);
	const auto It = g_ModelCache.find(Key);
	if(It != g_ModelCache.end()) {
		TouchModelCache(Key);
		return It->second;
	}
	const auto Parsed = Deserialize(ModelCell);
	if(!Parsed) {
		static CompiledMlp Empty;
		return Empty;
	}
	CompiledMlp Entry;
	Entry.Model = std::move(*Parsed);
	for(const Layer &L : Entry.Model.Layers)
		Entry.EnsureBuffers(std::max(L.Rows, L.Cols));
	auto [Ins, _] = g_ModelCache.emplace(Key, std::move(Entry));
	TouchModelCache(Key);
	return Ins->second;
}

void CompiledCacheClear() {
	g_ModelCache.clear();
	g_ModelCacheOrder.clear();
	g_ModelCacheIter.clear();
	g_InputParseCache.clear();
	g_InputParseOrder.clear();
	g_InputParseIter.clear();
}

std::optional<std::vector<double>> PredictFromCells(std::string_view ModelCell, std::string_view InputCell) {
	const std::vector<float> &InF32 = InputVecF32CacheLookup(InputCell);
	if(InF32.empty())
		return std::nullopt;
	CompiledMlp &C = CompiledCacheLookup(ModelCell);
	if(!C.Model.Layers.empty()) {
		std::vector<float> Out(C.Model.Layers.back().Rows);
		if(ForwardCompiledF32(C, InF32.data(), Out.data()))
			return MathSciSimdUtil::ToF64(Out);
	}
	const auto Model = Deserialize(ModelCell);
	if(!Model)
		return std::nullopt;
	const auto Out = ForwardF32(*Model, InF32);
	if(!Out)
		return std::nullopt;
	return MathSciSimdUtil::ToF64(*Out);
}

std::optional<std::vector<std::string>> ParseLayerWeightCells(std::string_view Cell) {
	if(Cell.size() >= 3 && Cell[0] == 'L' && Cell[1] == 'W' && Cell[2] == '[') {
		const std::size_t Close = Cell.find("]:");
		if(Close == std::string_view::npos)
			return std::nullopt;
		const auto Decl = ParseFloat(Cell.substr(3, Close - 3));
		if(!Decl)
			return std::nullopt;
		const std::size_t N = static_cast<std::size_t>(*Decl);
		std::vector<std::string> Out;
		Out.reserve(N);
		std::string_view Body = Cell.substr(Close + 2);
		size_t Pos = 0;
		while(Pos < Body.size()) {
			size_t End = Pos;
			while(End < Body.size() && Body[End] != ';')
				++End;
			const auto Part = Body.substr(Pos, End - Pos);
			if(Part.empty())
				return std::nullopt;
			Out.emplace_back(Part);
			Pos = End + (End < Body.size() ? 1 : 0);
		}
		if(Out.size() != N)
			return std::nullopt;
		return Out;
	}
	if(const auto L = AdvancedTypes::ParseListCell(Cell))
		return *L;
	return std::vector<std::string>{std::string(Cell)};
}

std::optional<std::string> BuildCellFromReal(const std::string &ActivationsCell, const std::string &WeightsCell) {
	std::vector<std::string> ActNames;
	if(const auto L = AdvancedTypes::ParseListCell(ActivationsCell))
		ActNames = *L;
	else
		ActNames.push_back(ActivationsCell);
	const auto WeightCells = ParseLayerWeightCells(WeightsCell);
	if(!WeightCells)
		return std::nullopt;
	const auto Model = BuildFromWeightCells(*WeightCells, ActNames);
	if(!Model)
		return std::nullopt;
	return SerializeBinary(*Model);
}

std::optional<std::string> SerializeCellFromReal(const std::string &ModelCell) {
	const auto Model = Deserialize(ModelCell);
	if(!Model)
		return std::nullopt;
	return SerializeBinary(*Model);
}

std::optional<std::string> ImportCellFromReal(const std::string &ModelCell) {
	const auto Model = Deserialize(ModelCell);
	if(!Model)
		return std::nullopt;
	return SerializeBinary(*Model);
}

std::optional<std::string> LoadCellFromReal(const std::string &ModelCell) { return ImportCellFromReal(ModelCell); }

std::optional<std::string> FingerprintCellFromReal(const std::string &ModelCell) {
	const auto Model = Deserialize(ModelCell);
	if(!Model)
		return std::nullopt;
	return Fingerprint(*Model);
}

std::optional<std::string> PredictCellFromReal(const std::string &ModelCell, const std::string &InputCell) {
	const auto Out = PredictFromCells(ModelCell, InputCell);
	if(!Out)
		return std::nullopt;
	return MathSciSimdUtil::FormatListCellFromDoubles(*Out);
}

std::optional<std::vector<float>> ParseGradientList(std::string_view Cell) {
	std::vector<float> Out;
	if(const auto L = AdvancedTypes::ParseListCell(Cell)) {
		for(const std::string &Part : *L) {
			const auto V = ParseFloat(Part);
			if(!V)
				return std::nullopt;
			Out.push_back(static_cast<float>(*V));
		}
		return Out;
	}
	std::string_view Rem = Cell;
	while(!Rem.empty()) {
		const size_t Comma = Rem.find(',');
		const auto Part = Rem.substr(0, Comma);
		const auto V = ParseFloat(Part);
		if(!V)
			return std::nullopt;
		Out.push_back(static_cast<float>(*V));
		if(Comma == std::string_view::npos)
			break;
		Rem.remove_prefix(Comma + 1);
	}
	return Out.empty() ? std::nullopt : std::make_optional(std::move(Out));
}

std::optional<std::string> OptimizerStepCellFromReal(const std::string &ModelCell, const std::string &GradientCell) {
	auto Model = Deserialize(ModelCell);
	const auto Grads = ParseGradientList(GradientCell);
	if(!Model || !Grads || Grads->empty())
		return std::nullopt;
	for(std::size_t Li = 0; Li < Model->Layers.size(); ++Li) {
		const float Step = Li < Grads->size() ? (*Grads)[Li] : Grads->back();
		for(float &W : Model->Layers[Li].Weights)
			W += Step;
	}
	return SerializeBinary(*Model);
}

bool TrainPinnDominantFast(const std::string &ActivationsCell, const std::string &WeightsCell,
                           const std::string &GradientCell, const int Epochs) noexcept {
	if(Epochs <= 0)
		return false;
	const auto WeightCells = ParseLayerWeightCells(WeightsCell);
	if(!WeightCells)
		return false;
	std::vector<std::string> ActNames;
	if(const auto L = AdvancedTypes::ParseListCell(ActivationsCell))
		ActNames = *L;
	else
		ActNames.push_back(ActivationsCell);
	auto Model = BuildFromWeightCells(*WeightCells, ActNames);
	const auto Grads = ParseGradientList(GradientCell);
	if(!Model || !Grads || Grads->empty())
		return false;
	for(int E = 0; E < Epochs; ++E) {
		for(std::size_t Li = 0; Li < Model->Layers.size(); ++Li) {
			const float Step = Li < Grads->size() ? (*Grads)[Li] : Grads->back();
			for(float &W : Model->Layers[Li].Weights)
				W += Step;
		}
	}
	return true;
}

} // namespace MathSciModel
} // namespace AstralDB
