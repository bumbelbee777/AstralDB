#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace AstralDB {
namespace MathSciAutograd {

constexpr size_t MaxAutogradLen = 1u << 20;
constexpr size_t MaxGraphInputs = 32;
constexpr size_t MaxGraphNodes = 4096;

enum class GraphOp : std::uint8_t {
	Input = 0,
	Const = 1,
	Add = 2,
	Mul = 3,
	Relu = 4,
	Sigmoid = 5,
	Tanh = 6,
	Scale = 7,
	Chain = 8,
	FusedMulRelu = 20,
	FusedChainMul = 21,
	FusedAddScale = 22,
	FusedMulAdd = 23,
	FusedMulSigmoid = 24,
	FusedMulTanh = 25,
	FusedReluScale = 26,
	FusedMulReluScale = 27,
};

enum class ActSaveKind : std::uint8_t { None = 0, Full = 1, ReluMask = 2 };

struct SavedActivation {
	ActSaveKind Kind = ActSaveKind::None;
	std::vector<float> Full;
	std::vector<std::uint8_t> ReluMaskBits;
};

struct GraphNode {
	GraphOp Op = GraphOp::Input;
	std::uint16_t In0 = 0;
	std::uint16_t In1 = 0;
	float Param = 0.f;
};

struct CompGraph {
	std::uint16_t InputCount = 0;
	std::vector<GraphNode> Nodes;
};

struct GraphPlan {
	std::vector<std::uint16_t> GradUseCount;
	std::vector<ActSaveKind> NodeSaveKind;
};

struct GraphForwardResult {
	std::vector<std::vector<float>> Inputs;
	std::vector<float> Output;
	std::size_t SeqLen = 0;
	std::vector<SavedActivation> NodeSaves;
};

struct GraphBackwardResult {
	std::vector<std::vector<float>> InputGrads;
};

/** Interleaved complex buffer [re0, im0, re1, im1, …], length \p N complex slots. */
struct ComplexSeq {
	std::vector<float> Interleaved;
	size_t Slots() const { return Interleaved.size() / 2; }
};

std::optional<ComplexSeq> ParseComplexSeq(std::string_view Cell);
std::vector<double> FormatComplexSeqInterleaved(const ComplexSeq &C);

/** Diagonal Hessian contribution: second_deriv ⊙ upstream (elementwise). */
std::vector<float> AdHessianDiagF32(const float *SecondDeriv, const float *Upstream, size_t N);

std::vector<float> AdHessianReluDiagF32(const float *X, const float *Upstream, size_t N);
std::vector<float> AdHessianSigmoidDiagF32(const float *Y, const float *Upstream, size_t N);
std::vector<float> AdHessianSquareDiagF32(const float *X, const float *Upstream, size_t N);
std::vector<float> AdHessianTanhDiagF32(const float *Y, const float *Upstream, size_t N);

/** Wirtinger ∂L/∂z and ∂L/∂z̄ from Cartesian (∂L/∂x, ∂L/∂y) interleaved upstream. */
ComplexSeq WirtingerDzFromCartesianF32(const ComplexSeq &CartesianGrad);
ComplexSeq WirtingerDzBarFromCartesianF32(const ComplexSeq &CartesianGrad);

/** Complex elementwise multiply: w = a ⊙ b; upstream is ∂L/∂w (interleaved). */
ComplexSeq AdWirtingerMulLhsF32(const ComplexSeq &A, const ComplexSeq &B, const ComplexSeq &Upstream);
ComplexSeq AdWirtingerMulRhsF32(const ComplexSeq &A, const ComplexSeq &B, const ComplexSeq &Upstream);

/** For f = |z|² = z·z̄: Wirtinger grads w.r.t. z (returns ∂L/∂z as interleaved). */
ComplexSeq AdWirtingerAbs2F32(const ComplexSeq &Z, const ComplexSeq &Upstream);

/** Elementwise Wirtinger chain: local_jacobian ⊙ upstream (complex). */
ComplexSeq AdWirtingerChainF32(const ComplexSeq &LocalJac, const ComplexSeq &Upstream);

std::vector<double> AdHessianDiagFromReal(const std::vector<double> &SecondDeriv, const std::vector<double> &Upstream);
std::vector<double> AdHessianReluFromReal(const std::vector<double> &X, const std::vector<double> &Upstream);
std::vector<double> AdHessianSigmoidFromReal(const std::vector<double> &Y, const std::vector<double> &Upstream);
std::vector<double> AdHessianSquareFromReal(const std::vector<double> &X, const std::vector<double> &Upstream);
std::vector<double> AdHessianTanhFromReal(const std::vector<double> &Y, const std::vector<double> &Upstream);

std::optional<std::vector<double>> AdWirtingerMulLhsFromCells(std::string_view A, std::string_view B,
                                                              std::string_view Upstream);
std::optional<std::vector<double>> AdWirtingerMulRhsFromCells(std::string_view A, std::string_view B,
                                                              std::string_view Upstream);
std::optional<std::vector<double>> AdWirtingerAbs2FromCells(std::string_view Z, std::string_view Upstream);
std::optional<std::vector<double>> AdWirtingerChainFromCells(std::string_view Local, std::string_view Upstream);
std::optional<std::vector<double>> WirtingerDzFromCell(std::string_view CartesianGrad);
std::optional<std::vector<double>> WirtingerDzBarFromCell(std::string_view CartesianGrad);

std::optional<GraphOp> ParseGraphOp(std::string_view Name);
const char *GraphOpName(GraphOp Op);

std::optional<CompGraph> BuildGraphFromSpec(std::uint16_t InputCount, const std::vector<double> &Spec);
std::optional<CompGraph> DeserializeGraph(std::string_view Cell);
std::string SerializeGraph(const CompGraph &Graph);

CompGraph FuseGraph(const CompGraph &Graph);
size_t FusedNodeCount(const CompGraph &Before, const CompGraph &After);

GraphPlan BuildGraphPlan(const CompGraph &Graph);
std::size_t GraphCachePayloadBytes(const GraphForwardResult &Forward);

std::optional<GraphForwardResult> ForwardGraphF32(const CompGraph &Graph, const std::vector<std::vector<float>> &Inputs);
std::optional<GraphBackwardResult> BackwardGraphF32(const CompGraph &Graph, const GraphForwardResult &Forward,
                                                    const std::vector<float> &Upstream);

std::optional<std::string> BuildGraphCellFromReal(std::string_view InputCountCell, std::string_view SpecCell);
std::optional<std::string> FuseGraphCellFromReal(std::string_view GraphCell);
std::optional<std::string> ForwardGraphCellFromReal(std::string_view GraphCell, std::string_view InputsCell);
std::optional<std::string> GraphCacheCellFromReal(std::string_view GraphCell, std::string_view InputsCell);
std::optional<std::string> BackwardGraphCellFromReal(std::string_view GraphCell, std::string_view CacheCell,
                                                     std::string_view UpstreamCell);
std::optional<std::string> GraphNodeCountCellFromReal(std::string_view GraphCell);

} // namespace MathSciAutograd
} // namespace AstralDB
