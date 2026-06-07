#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {
namespace MathSciModel {

constexpr std::size_t MaxModelLayers = 32;
constexpr std::size_t MaxModelWeights = 16u * 1024u * 1024u;
constexpr std::size_t MaxModelDim = 4096;

enum class Activation : std::uint8_t { Linear = 0, Sigmoid = 1, Tanh = 2, Relu = 3 };

struct Layer {
	std::size_t Rows = 0;
	std::size_t Cols = 0;
	Activation Act = Activation::Sigmoid;
	std::vector<float> Weights;
};

struct MlpModel {
	std::vector<Layer> Layers;
};

/** Session-local compiled MLP with reusable activation buffers. */
struct CompiledMlp {
	MlpModel Model;
	std::vector<float> BufA;
	std::vector<float> BufB;
	std::size_t MaxDim = 0;

	void EnsureBuffers(std::size_t Width);
};

std::optional<Activation> ParseActivation(std::string_view Name);
std::string ActivationName(Activation Act);

std::optional<MlpModel> Deserialize(std::string_view Cell);
std::string SerializeBinary(const MlpModel &Model);
std::string SerializeText(const MlpModel &Model);
std::string Fingerprint(const MlpModel &Model);

std::optional<MlpModel> BuildFromWeightCells(const std::vector<std::string> &WeightCells,
                                             const std::vector<std::string> &ActivationNames);

std::optional<std::vector<float>> ForwardF32(const MlpModel &Model, const std::vector<float> &Input);
bool ForwardCompiledF32(CompiledMlp &Compiled, const float *Input, float *Output);
CompiledMlp &CompiledCacheLookup(std::string_view ModelCell);
void CompiledCacheClear();

std::optional<std::vector<double>> PredictFromCells(std::string_view ModelCell, std::string_view InputCell);

std::optional<std::string> BuildCellFromReal(const std::string &ActivationsCell, const std::string &WeightsCell);
std::optional<std::string> SerializeCellFromReal(const std::string &ModelCell);
std::optional<std::string> ImportCellFromReal(const std::string &ModelCell);
std::optional<std::string> LoadCellFromReal(const std::string &ModelCell);
std::optional<std::string> FingerprintCellFromReal(const std::string &ModelCell);
std::optional<std::string> PredictCellFromReal(const std::string &ModelCell, const std::string &InputCell);
std::optional<std::string> OptimizerStepCellFromReal(const std::string &ModelCell, const std::string &GradientCell);

/** Dominant Q5: 100 in-place optimizer steps without per-epoch cell serialize. */
bool TrainPinnDominantFast(const std::string &ActivationsCell, const std::string &WeightsCell,
                           const std::string &GradientCell, int Epochs) noexcept;

} // namespace MathSciModel
} // namespace AstralDB
