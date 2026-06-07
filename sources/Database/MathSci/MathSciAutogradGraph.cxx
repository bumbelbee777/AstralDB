#include <Database/MathSci/MathSciAutogradGraph.hxx>

#include <Database/MathSci/MathSciAutogradMicrokernels.hxx>
#include <Database/MathSci/MathSciSignal.hxx>
#include <Database/Types/AdvancedTypes.hxx>
#include <IO/SIMD.hxx>

#include <cmath>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace AstralDB {
namespace MathSciAutogradGraph {
namespace {

using MathSciAutograd::ActSaveKind;
using MathSciAutograd::GraphNode;
using MathSciAutograd::GraphOp;
using MathSciAutograd::MaxAutogradLen;
using MathSciAutograd::SavedActivation;

std::uint16_t NodeSlot(std::uint16_t InputCount, std::size_t NodeIndex) {
	return static_cast<std::uint16_t>(InputCount + NodeIndex);
}

bool NodeHasSingleConsumer(const CompGraph &Graph, std::size_t ProducerIndex, std::size_t SkipIndex) {
	const std::uint16_t Slot = NodeSlot(Graph.InputCount, ProducerIndex);
	for(std::size_t I = 0; I < Graph.Nodes.size(); ++I) {
		if(I == SkipIndex)
			continue;
		const auto &N = Graph.Nodes[I];
		if(N.In0 == Slot || N.In1 == Slot)
			return false;
	}
	return true;
}

void RemapSlotRef(std::uint16_t &Slot, std::uint16_t InputCount, std::size_t ErasedIdx) {
	if(Slot < InputCount)
		return;
	const std::size_t NodeIdx = Slot - InputCount;
	if(NodeIdx > ErasedIdx)
		Slot = static_cast<std::uint16_t>(Slot - 1);
}

void RemapParamSlot(float &Param, std::uint16_t InputCount, std::size_t ErasedIdx) {
	auto Slot = static_cast<std::uint16_t>(Param);
	if(Slot < InputCount)
		return;
	const std::size_t NodeIdx = Slot - InputCount;
	if(NodeIdx > ErasedIdx)
		Param = static_cast<float>(Slot - 1);
}

void RemapGraphAfterErase(CompGraph &Graph, std::size_t ErasedIdx) {
	for(auto &N : Graph.Nodes) {
		RemapSlotRef(N.In0, Graph.InputCount, ErasedIdx);
		RemapSlotRef(N.In1, Graph.InputCount, ErasedIdx);
		RemapParamSlot(N.Param, Graph.InputCount, ErasedIdx);
	}
}

enum class FuseEdge : std::uint8_t {
	ProducerOnIn0 = 0,
	ProducerOnIn1 = 1,
};

struct FuseRule {
	GraphOp Producer;
	GraphOp Consumer;
	FuseEdge Edge;
	GraphOp Result;
};

static constexpr FuseRule kFuseRules[] = {
    {GraphOp::Mul, GraphOp::Relu, FuseEdge::ProducerOnIn0, GraphOp::FusedMulRelu},
    {GraphOp::Mul, GraphOp::Sigmoid, FuseEdge::ProducerOnIn0, GraphOp::FusedMulSigmoid},
    {GraphOp::Mul, GraphOp::Tanh, FuseEdge::ProducerOnIn0, GraphOp::FusedMulTanh},
    {GraphOp::Mul, GraphOp::Chain, FuseEdge::ProducerOnIn1, GraphOp::FusedChainMul},
    {GraphOp::Add, GraphOp::Scale, FuseEdge::ProducerOnIn0, GraphOp::FusedAddScale},
    {GraphOp::Relu, GraphOp::Scale, FuseEdge::ProducerOnIn0, GraphOp::FusedReluScale},
    {GraphOp::FusedMulRelu, GraphOp::Scale, FuseEdge::ProducerOnIn0, GraphOp::FusedMulReluScale},
    {GraphOp::FusedMulSigmoid, GraphOp::Scale, FuseEdge::ProducerOnIn0, GraphOp::FusedMulSigmoid},
    {GraphOp::FusedMulTanh, GraphOp::Scale, FuseEdge::ProducerOnIn0, GraphOp::FusedMulTanh},
};

static std::optional<std::size_t> ProducerIndex(const CompGraph &Graph, const GraphNode &Cons, FuseEdge Edge) {
	const std::uint16_t Slot = Edge == FuseEdge::ProducerOnIn0 ? Cons.In0 : Cons.In1;
	if(Slot < Graph.InputCount)
		return std::nullopt;
	return Slot - Graph.InputCount;
}

static bool TryApplyFuseRule(CompGraph &Out, std::size_t ConsumerIdx, const FuseRule &Rule) {
	auto &Cons = Out.Nodes[ConsumerIdx];
	if(Cons.Op != Rule.Consumer)
		return false;
	const auto PreIdx = ProducerIndex(Out, Cons, Rule.Edge);
	if(!PreIdx || *PreIdx >= ConsumerIdx)
		return false;
	const auto &Pre = Out.Nodes[*PreIdx];
	if(Pre.Op != Rule.Producer)
		return false;
	if(!NodeHasSingleConsumer(Out, *PreIdx, ConsumerIdx))
		return false;
	GraphNode Fused;
	Fused.Op = Rule.Result;
	switch(Rule.Result) {
	case GraphOp::FusedChainMul:
		Fused.In0 = Cons.In0;
		Fused.In1 = Pre.In0;
		Fused.Param = static_cast<float>(Pre.In1);
		break;
	case GraphOp::FusedReluScale:
		Fused.In0 = Pre.In0;
		Fused.In1 = 0;
		Fused.Param = Cons.Param;
		break;
	case GraphOp::FusedMulReluScale:
		Fused.In0 = Pre.In0;
		Fused.In1 = Pre.In1;
		Fused.Param = Cons.Param;
		break;
	case GraphOp::FusedMulSigmoid:
	case GraphOp::FusedMulTanh:
		if(Rule.Producer == GraphOp::FusedMulSigmoid || Rule.Producer == GraphOp::FusedMulTanh) {
			Fused.In0 = Pre.In0;
			Fused.In1 = Pre.In1;
			Fused.Param = Pre.Param * Cons.Param;
		} else {
			Fused.In0 = Pre.In0;
			Fused.In1 = Pre.In1;
			Fused.Param = Cons.Param;
		}
		break;
	default:
		Fused.In0 = Pre.In0;
		Fused.In1 = Pre.In1;
		Fused.Param = Cons.Param;
		break;
	}
	Out.Nodes[*PreIdx] = Fused;
	Out.Nodes.erase(Out.Nodes.begin() + static_cast<std::ptrdiff_t>(ConsumerIdx));
	RemapGraphAfterErase(Out, ConsumerIdx);
	return true;
}

static bool TryFuseScaleChain(CompGraph &Out, std::size_t ConsumerIdx) {
	auto &Cons = Out.Nodes[ConsumerIdx];
	if(Cons.Op != GraphOp::Scale)
		return false;
	if(Cons.In0 < Out.InputCount)
		return false;
	const std::size_t PreIdx = Cons.In0 - Out.InputCount;
	if(PreIdx >= ConsumerIdx)
		return false;
	auto &Pre = Out.Nodes[PreIdx];
	if(Pre.Op != GraphOp::Scale)
		return false;
	if(!NodeHasSingleConsumer(Out, PreIdx, ConsumerIdx))
		return false;
	Pre.Param *= Cons.Param;
	Out.Nodes.erase(Out.Nodes.begin() + static_cast<std::ptrdiff_t>(ConsumerIdx));
	RemapGraphAfterErase(Out, ConsumerIdx);
	return true;
}

static bool TryFuseMulAdd(CompGraph &Out, std::size_t ConsumerIdx) {
	auto &Cons = Out.Nodes[ConsumerIdx];
	if(Cons.Op != GraphOp::Add)
		return false;
	if(Cons.In0 < Out.InputCount)
		return false;
	const std::size_t MulIdx = Cons.In0 - Out.InputCount;
	if(MulIdx >= ConsumerIdx)
		return false;
	const auto &MulNode = Out.Nodes[MulIdx];
	if(MulNode.Op != GraphOp::Mul)
		return false;
	if(Cons.In1 >= Out.InputCount + ConsumerIdx)
		return false;
	if(!NodeHasSingleConsumer(Out, MulIdx, ConsumerIdx))
		return false;
	GraphNode Fused;
	Fused.Op = GraphOp::FusedMulAdd;
	Fused.In0 = MulNode.In0;
	Fused.In1 = MulNode.In1;
	Fused.Param = static_cast<float>(Cons.In1);
	Out.Nodes[MulIdx] = Fused;
	Out.Nodes.erase(Out.Nodes.begin() + static_cast<std::ptrdiff_t>(ConsumerIdx));
	RemapGraphAfterErase(Out, ConsumerIdx);
	return true;
}

const std::vector<float> *ResolveSlot(const std::vector<std::vector<float>> &Inputs,
                                      const std::vector<std::vector<float>> &Memo, std::uint16_t InputCount,
                                      std::uint16_t Slot) {
	if(Slot < InputCount)
		return Slot < Inputs.size() ? &Inputs[Slot] : nullptr;
	const std::size_t NodeIdx = Slot - InputCount;
	return NodeIdx < Memo.size() && !Memo[NodeIdx].empty() ? &Memo[NodeIdx] : nullptr;
}

static bool NeedsReluMask(GraphOp Op) {
	switch(Op) {
	case GraphOp::Relu:
	case GraphOp::FusedMulRelu:
	case GraphOp::FusedReluScale:
	case GraphOp::FusedMulReluScale:
		return true;
	default:
		return false;
	}
}

static bool NeedsFullSave(GraphOp Op) {
	switch(Op) {
	case GraphOp::Sigmoid:
	case GraphOp::Tanh:
	case GraphOp::FusedMulSigmoid:
	case GraphOp::FusedMulTanh:
		return true;
	default:
		return false;
	}
}

static void SaveReluMask(SavedActivation &Save, const float *PreAct, std::size_t N) {
	Save.Kind = ActSaveKind::ReluMask;
	Save.ReluMaskBits.assign((N + 7) / 8, 0);
	MathSciAutogradMicrokernels::PackReluMaskF32(PreAct, Save.ReluMaskBits.data(), N);
}

static bool ExecuteNodeForward(const GraphNode &Node, const std::vector<std::vector<float>> &Inputs,
                               const std::vector<std::vector<float>> &Memo, std::uint16_t InputCount,
                               std::vector<float> &Out, SavedActivation *Save) {
	const auto *A = ResolveSlot(Inputs, Memo, InputCount, Node.In0);
	const auto *B = ResolveSlot(Inputs, Memo, InputCount, Node.In1);
	switch(Node.Op) {
	case GraphOp::Const:
		Out.assign(1, Node.Param);
		return true;
	case GraphOp::Add:
		if(!A || !B || A->size() != B->size() || A->empty() || A->size() > MaxAutogradLen)
			return false;
		Out.resize(A->size());
		Simd::AddF32(Out.data(), A->data(), B->data(), A->size());
		return true;
	case GraphOp::Mul:
		if(!A || !B || A->size() != B->size() || A->empty() || A->size() > MaxAutogradLen)
			return false;
		Out.resize(A->size());
		Simd::MulF32(Out.data(), A->data(), B->data(), A->size());
		return true;
	case GraphOp::Relu:
		if(!A || A->empty() || A->size() > MaxAutogradLen)
			return false;
		Out.resize(A->size());
		if(Save)
			SaveReluMask(*Save, A->data(), A->size());
		Simd::ReluF32(Out.data(), A->data(), A->size());
		return true;
	case GraphOp::Sigmoid:
		if(!A || A->empty() || A->size() > MaxAutogradLen)
			return false;
		Out.resize(A->size());
		for(size_t I = 0; I < A->size(); ++I)
			Out[I] = 1.f / (1.f + std::exp(-(*A)[I]));
		if(Save) {
			Save->Kind = ActSaveKind::Full;
			Save->Full = Out;
		}
		return true;
	case GraphOp::Tanh:
		if(!A || A->empty() || A->size() > MaxAutogradLen)
			return false;
		Out.resize(A->size());
		for(size_t I = 0; I < A->size(); ++I)
			Out[I] = std::tanh((*A)[I]);
		if(Save) {
			Save->Kind = ActSaveKind::Full;
			Save->Full = Out;
		}
		return true;
	case GraphOp::Scale:
		if(!A || A->empty() || A->size() > MaxAutogradLen)
			return false;
		Out.resize(A->size());
		Simd::ScaleF32(Out.data(), A->data(), Node.Param, A->size());
		return true;
	case GraphOp::Chain:
		if(!A || !B || A->size() != B->size() || A->empty() || A->size() > MaxAutogradLen)
			return false;
		Out.resize(A->size());
		MathSciAutogradMicrokernels::FusedChainMulF32(Out.data(), A->data(), B->data(), A->size());
		return true;
	case GraphOp::FusedMulRelu:
		if(!A || !B || A->size() != B->size() || A->empty() || A->size() > MaxAutogradLen)
			return false;
		Out.resize(A->size());
		if(Save) {
			std::vector<float> PreAct(A->size());
			Simd::MulF32(PreAct.data(), A->data(), B->data(), A->size());
			SaveReluMask(*Save, PreAct.data(), A->size());
		}
		MathSciAutogradMicrokernels::FusedMulReluF32(Out.data(), A->data(), B->data(), A->size());
		return true;
	case GraphOp::FusedChainMul: {
		const auto *Local = ResolveSlot(Inputs, Memo, InputCount, Node.In0);
		const auto *Lhs = ResolveSlot(Inputs, Memo, InputCount, Node.In1);
		const auto *Rhs = ResolveSlot(Inputs, Memo, InputCount, static_cast<std::uint16_t>(Node.Param));
		if(!Local || !Lhs || !Rhs || Local->size() != Lhs->size() || Lhs->size() != Rhs->size() || Local->empty())
			return false;
		std::vector<float> Prod(Lhs->size());
		Simd::MulF32(Prod.data(), Lhs->data(), Rhs->data(), Lhs->size());
		Out.resize(Local->size());
		MathSciAutogradMicrokernels::FusedChainMulF32(Out.data(), Local->data(), Prod.data(), Local->size());
		return true;
	}
	case GraphOp::FusedAddScale:
		if(!A || !B || A->size() != B->size() || A->empty() || A->size() > MaxAutogradLen)
			return false;
		Out.resize(A->size());
		MathSciAutogradMicrokernels::FusedAddScaleF32(Out.data(), A->data(), B->data(), Node.Param, A->size());
		return true;
	case GraphOp::FusedMulAdd: {
		const auto *C = ResolveSlot(Inputs, Memo, InputCount, static_cast<std::uint16_t>(Node.Param));
		if(!A || !B || !C || A->size() != B->size() || B->size() != C->size() || A->empty())
			return false;
		Out.resize(A->size());
		MathSciAutogradMicrokernels::FusedMulAddF32(Out.data(), A->data(), B->data(), C->data(), A->size());
		return true;
	}
	case GraphOp::FusedMulSigmoid:
		if(!A || !B || A->size() != B->size() || A->empty() || A->size() > MaxAutogradLen)
			return false;
		Out.resize(A->size());
		MathSciAutogradMicrokernels::FusedMulSigmoidF32(Out.data(), A->data(), B->data(), A->size());
		if(Save && Node.Param != 1.f)
			Simd::ScaleF32(Out.data(), Out.data(), Node.Param, A->size());
		if(Save) {
			Save->Kind = ActSaveKind::Full;
			Save->Full = Out;
		}
		return true;
	case GraphOp::FusedMulTanh:
		if(!A || !B || A->size() != B->size() || A->empty() || A->size() > MaxAutogradLen)
			return false;
		Out.resize(A->size());
		MathSciAutogradMicrokernels::FusedMulTanhF32(Out.data(), A->data(), B->data(), A->size());
		if(Save && Node.Param != 1.f)
			Simd::ScaleF32(Out.data(), Out.data(), Node.Param, A->size());
		if(Save) {
			Save->Kind = ActSaveKind::Full;
			Save->Full = Out;
		}
		return true;
	case GraphOp::FusedReluScale:
		if(!A || A->empty() || A->size() > MaxAutogradLen)
			return false;
		Out.resize(A->size());
		if(Save)
			SaveReluMask(*Save, A->data(), A->size());
		MathSciAutogradMicrokernels::FusedReluScaleF32(Out.data(), A->data(), Node.Param, A->size());
		return true;
	case GraphOp::FusedMulReluScale:
		if(!A || !B || A->size() != B->size() || A->empty() || A->size() > MaxAutogradLen)
			return false;
		Out.resize(A->size());
		if(Save) {
			std::vector<float> PreAct(A->size());
			Simd::MulF32(PreAct.data(), A->data(), B->data(), A->size());
			SaveReluMask(*Save, PreAct.data(), A->size());
		}
		MathSciAutogradMicrokernels::FusedMulReluScaleF32(Out.data(), A->data(), B->data(), Node.Param, A->size());
		return true;
	default:
		return false;
	}
}

static bool RecomputeNode(const CompGraph &Graph, std::size_t NodeIdx, const std::vector<std::vector<float>> &Inputs,
                          std::vector<std::vector<float>> &Memo) {
	if(NodeIdx < Memo.size() && !Memo[NodeIdx].empty())
		return true;
	if(NodeIdx >= Graph.Nodes.size())
		return false;
	const auto &Node = Graph.Nodes[NodeIdx];
	const auto Touch = [&](std::uint16_t Slot) -> bool {
		if(Slot < Graph.InputCount)
			return true;
		return RecomputeNode(Graph, Slot - Graph.InputCount, Inputs, Memo);
	};
	if(!Touch(Node.In0) || !Touch(Node.In1))
		return false;
	if(Node.Op == GraphOp::FusedChainMul || Node.Op == GraphOp::FusedMulAdd)
		if(!Touch(static_cast<std::uint16_t>(Node.Param)))
			return false;
	if(Memo.size() <= NodeIdx)
		Memo.resize(NodeIdx + 1);
	return ExecuteNodeForward(Node, Inputs, Memo, Graph.InputCount, Memo[NodeIdx], nullptr);
}

static const std::vector<float> *SlotValues(const CompGraph &Graph, const GraphForwardResult &Forward,
                                            std::uint16_t Slot, std::vector<std::vector<float>> &Memo) {
	if(Slot < Graph.InputCount)
		return Slot < Forward.Inputs.size() ? &Forward.Inputs[Slot] : nullptr;
	const std::size_t NodeIdx = Slot - Graph.InputCount;
	if(NodeIdx >= Graph.Nodes.size())
		return nullptr;
	if(RecomputeNode(Graph, NodeIdx, Forward.Inputs, Memo))
		return &Memo[NodeIdx];
	return nullptr;
}

static void AccumulateGrad(std::vector<std::vector<float>> &Grads, std::uint16_t Slot, const std::vector<float> &Delta) {
	if(Slot >= Grads.size() || Delta.empty())
		return;
	if(Grads[Slot].empty()) {
		Grads[Slot] = Delta;
		return;
	}
	Simd::AddF32(Grads[Slot].data(), Grads[Slot].data(), Delta.data(), Delta.size());
}

static bool ExecuteNodeBackward(const GraphNode &Node, std::size_t NodeIndex, const CompGraph &Graph,
                                const GraphForwardResult &Forward, std::vector<std::vector<float>> &Memo,
                                const std::vector<float> &Upstream, std::vector<std::vector<float>> &Grads) {
	const auto *A = SlotValues(Graph, Forward, Node.In0, Memo);
	const auto *B = SlotValues(Graph, Forward, Node.In1, Memo);
	const SavedActivation *Save =
	    NodeIndex < Forward.NodeSaves.size() ? &Forward.NodeSaves[NodeIndex] : nullptr;
	switch(Node.Op) {
	case GraphOp::Add:
		AccumulateGrad(Grads, Node.In0, Upstream);
		AccumulateGrad(Grads, Node.In1, Upstream);
		return true;
	case GraphOp::Mul: {
		if(!A || !B)
			return false;
		std::vector<float> Glhs(B->size());
		std::vector<float> Grhs(A->size());
		Simd::MulF32(Glhs.data(), B->data(), Upstream.data(), B->size());
		Simd::MulF32(Grhs.data(), A->data(), Upstream.data(), A->size());
		AccumulateGrad(Grads, Node.In0, Glhs);
		AccumulateGrad(Grads, Node.In1, Grhs);
		return true;
	}
	case GraphOp::Relu: {
		if(!A)
			return false;
		std::vector<float> Gx(A->size());
		if(Save && Save->Kind == ActSaveKind::ReluMask)
			MathSciAutogradMicrokernels::ReluMaskBackwardF32(Gx.data(), Save->ReluMaskBits.data(), Upstream.data(),
			                                                 A->size());
		else
			for(size_t I = 0; I < A->size(); ++I)
				Gx[I] = (*A)[I] > 0.f ? Upstream[I] : 0.f;
		AccumulateGrad(Grads, Node.In0, Gx);
		return true;
	}
	case GraphOp::Sigmoid: {
		const float *Y = Save && Save->Kind == ActSaveKind::Full ? Save->Full.data() : nullptr;
		if(!Y && !RecomputeNode(Graph, NodeIndex, Forward.Inputs, Memo))
			return false;
		if(!Y)
			Y = Memo[NodeIndex].data();
		auto Gx = MathSciSignal::AdGradSigmoidF32(Y, Upstream.data(), Forward.SeqLen);
		AccumulateGrad(Grads, Node.In0, Gx);
		return true;
	}
	case GraphOp::Tanh: {
		const float *Y = Save && Save->Kind == ActSaveKind::Full ? Save->Full.data() : nullptr;
		if(!Y && !RecomputeNode(Graph, NodeIndex, Forward.Inputs, Memo))
			return false;
		if(!Y)
			Y = Memo[NodeIndex].data();
		auto Gx = MathSciSignal::AdGradTanhF32(Y, Upstream.data(), Forward.SeqLen);
		AccumulateGrad(Grads, Node.In0, Gx);
		return true;
	}
	case GraphOp::Scale: {
		if(!A)
			return false;
		std::vector<float> Gx(A->size());
		Simd::ScaleF32(Gx.data(), Upstream.data(), Node.Param, A->size());
		AccumulateGrad(Grads, Node.In0, Gx);
		return true;
	}
	case GraphOp::Chain: {
		if(!A || !B)
			return false;
		std::vector<float> Glocal(A->size());
		std::vector<float> Gprev(B->size());
		Simd::MulF32(Glocal.data(), B->data(), Upstream.data(), A->size());
		Simd::MulF32(Gprev.data(), A->data(), Upstream.data(), B->size());
		AccumulateGrad(Grads, Node.In0, Glocal);
		AccumulateGrad(Grads, Node.In1, Gprev);
		return true;
	}
	case GraphOp::Const:
		return true;
	case GraphOp::FusedMulRelu:
	case GraphOp::FusedMulReluScale: {
		if(!A || !B)
			return false;
		std::vector<float> GradMul(A->size());
		if(Save && Save->Kind == ActSaveKind::ReluMask) {
			MathSciAutogradMicrokernels::ReluMaskBackwardF32(GradMul.data(), Save->ReluMaskBits.data(), Upstream.data(),
			                                                 A->size());
		} else {
			std::vector<float> PreAct(A->size());
			Simd::MulF32(PreAct.data(), A->data(), B->data(), A->size());
			MathSciAutogradMicrokernels::FusedMulReluBackwardMulF32(GradMul.data(), PreAct.data(), Upstream.data(),
			                                                        A->size());
		}
		if(Node.Op == GraphOp::FusedMulReluScale)
			Simd::ScaleF32(GradMul.data(), GradMul.data(), Node.Param, A->size());
		std::vector<float> Glhs(A->size());
		std::vector<float> Grhs(B->size());
		MathSciAutogradMicrokernels::FusedMulReluBackwardLhsF32(Glhs.data(), B->data(), GradMul.data(), A->size());
		MathSciAutogradMicrokernels::FusedMulReluBackwardRhsF32(Grhs.data(), A->data(), GradMul.data(), B->size());
		AccumulateGrad(Grads, Node.In0, Glhs);
		AccumulateGrad(Grads, Node.In1, Grhs);
		return true;
	}
	case GraphOp::FusedChainMul: {
		const auto *Local = SlotValues(Graph, Forward, Node.In0, Memo);
		const auto *Lhs = SlotValues(Graph, Forward, Node.In1, Memo);
		const auto *Rhs = SlotValues(Graph, Forward, static_cast<std::uint16_t>(Node.Param), Memo);
		if(!Local || !Lhs || !Rhs)
			return false;
		std::vector<float> Prod(Lhs->size());
		Simd::MulF32(Prod.data(), Lhs->data(), Rhs->data(), Lhs->size());
		std::vector<float> Glocal(Local->size());
		MathSciAutogradMicrokernels::FusedChainMulF32(Glocal.data(), Upstream.data(), Prod.data(), Local->size());
		std::vector<float> Gprod(Lhs->size());
		MathSciAutogradMicrokernels::FusedChainMulF32(Gprod.data(), Local->data(), Upstream.data(), Lhs->size());
		std::vector<float> Glhs(Lhs->size());
		std::vector<float> Grhs(Rhs->size());
		Simd::MulF32(Glhs.data(), Gprod.data(), Rhs->data(), Lhs->size());
		Simd::MulF32(Grhs.data(), Gprod.data(), Lhs->data(), Rhs->size());
		AccumulateGrad(Grads, Node.In0, Glocal);
		AccumulateGrad(Grads, Node.In1, Glhs);
		AccumulateGrad(Grads, static_cast<std::uint16_t>(Node.Param), Grhs);
		return true;
	}
	case GraphOp::FusedAddScale: {
		if(!A || !B)
			return false;
		std::vector<float> Scaled(Upstream.size());
		Simd::ScaleF32(Scaled.data(), Upstream.data(), Node.Param, Upstream.size());
		AccumulateGrad(Grads, Node.In0, Scaled);
		AccumulateGrad(Grads, Node.In1, Scaled);
		return true;
	}
	case GraphOp::FusedMulAdd: {
		const auto *C = SlotValues(Graph, Forward, static_cast<std::uint16_t>(Node.Param), Memo);
		if(!A || !B || !C)
			return false;
		std::vector<float> Glhs(B->size());
		std::vector<float> Grhs(A->size());
		Simd::MulF32(Glhs.data(), B->data(), Upstream.data(), B->size());
		Simd::MulF32(Grhs.data(), A->data(), Upstream.data(), A->size());
		AccumulateGrad(Grads, Node.In0, Glhs);
		AccumulateGrad(Grads, Node.In1, Grhs);
		AccumulateGrad(Grads, static_cast<std::uint16_t>(Node.Param), Upstream);
		return true;
	}
	case GraphOp::FusedMulSigmoid:
	case GraphOp::FusedMulTanh: {
		const float *Y = Save && Save->Kind == ActSaveKind::Full ? Save->Full.data() : nullptr;
		if(!Y && !RecomputeNode(Graph, NodeIndex, Forward.Inputs, Memo))
			return false;
		std::vector<float> Ybuf;
		if(!Y) {
			Ybuf = Memo[NodeIndex];
			Y = Ybuf.data();
		}
		std::vector<float> Yunscaled(Forward.SeqLen);
		if(Node.Param != 1.f && Node.Param != 0.f) {
			for(std::size_t J = 0; J < Forward.SeqLen; ++J)
				Yunscaled[J] = Y[J] / Node.Param;
			Y = Yunscaled.data();
		} else if(Node.Param == 0.f) {
			std::vector<float> Zeros(Forward.SeqLen, 0.f);
			Yunscaled = Zeros;
			Y = Yunscaled.data();
		} else {
			Yunscaled.assign(Y, Y + Forward.SeqLen);
			Y = Yunscaled.data();
		}
		std::vector<float> Gpre(Forward.SeqLen);
		if(Node.Op == GraphOp::FusedMulSigmoid)
			Gpre = MathSciSignal::AdGradSigmoidF32(Y, Upstream.data(), Forward.SeqLen);
		else
			Gpre = MathSciSignal::AdGradTanhF32(Y, Upstream.data(), Forward.SeqLen);
		if(Node.Param != 1.f)
			Simd::ScaleF32(Gpre.data(), Gpre.data(), Node.Param, Forward.SeqLen);
		if(!A || !B)
			return false;
		std::vector<float> Glhs(B->size());
		std::vector<float> Grhs(A->size());
		Simd::MulF32(Glhs.data(), B->data(), Gpre.data(), B->size());
		Simd::MulF32(Grhs.data(), A->data(), Gpre.data(), A->size());
		AccumulateGrad(Grads, Node.In0, Glhs);
		AccumulateGrad(Grads, Node.In1, Grhs);
		return true;
	}
	case GraphOp::FusedReluScale: {
		if(!A)
			return false;
		std::vector<float> Gpre(A->size());
		if(Save && Save->Kind == ActSaveKind::ReluMask)
			MathSciAutogradMicrokernels::ReluMaskBackwardF32(Gpre.data(), Save->ReluMaskBits.data(), Upstream.data(),
			                                                 A->size());
		else
			for(size_t I = 0; I < A->size(); ++I)
				Gpre[I] = (*A)[I] > 0.f ? Upstream[I] : 0.f;
		Simd::ScaleF32(Gpre.data(), Gpre.data(), Node.Param, A->size());
		AccumulateGrad(Grads, Node.In0, Gpre);
		return true;
	}
	default:
		return false;
	}
}

static void IncrementGradUse(const CompGraph &Graph, std::vector<std::uint16_t> &Use, std::uint16_t Slot) {
	if(Slot < Graph.InputCount + Graph.Nodes.size())
		++Use[Slot];
}

} // namespace

CompGraph FuseGraph(const CompGraph &Graph) {
	CompGraph Out = Graph;
	bool Changed = true;
	while(Changed) {
		Changed = false;
		for(std::size_t I = 0; I < Out.Nodes.size(); ++I) {
			if(TryFuseScaleChain(Out, I)) {
				Changed = true;
				break;
			}
			if(TryFuseMulAdd(Out, I)) {
				Changed = true;
				break;
			}
			for(const auto &Rule : kFuseRules) {
				if(TryApplyFuseRule(Out, I, Rule)) {
					Changed = true;
					break;
				}
			}
			if(Changed)
				break;
		}
	}
	return Out;
}

GraphPlan BuildGraphPlan(const CompGraph &Graph) {
	GraphPlan Plan;
	const std::size_t SlotCount = Graph.InputCount + Graph.Nodes.size();
	Plan.GradUseCount.assign(SlotCount, 0);
	for(const auto &N : Graph.Nodes) {
		IncrementGradUse(Graph, Plan.GradUseCount, N.In0);
		IncrementGradUse(Graph, Plan.GradUseCount, N.In1);
		if(N.Op == GraphOp::FusedChainMul || N.Op == GraphOp::FusedMulAdd)
			IncrementGradUse(Graph, Plan.GradUseCount, static_cast<std::uint16_t>(N.Param));
	}
	Plan.NodeSaveKind.resize(Graph.Nodes.size(), ActSaveKind::None);
	for(std::size_t I = 0; I < Graph.Nodes.size(); ++I) {
		if(NeedsFullSave(Graph.Nodes[I].Op))
			Plan.NodeSaveKind[I] = ActSaveKind::Full;
		else if(NeedsReluMask(Graph.Nodes[I].Op))
			Plan.NodeSaveKind[I] = ActSaveKind::ReluMask;
	}
	return Plan;
}

std::size_t GraphCachePayloadBytes(const GraphForwardResult &Forward) {
	std::size_t Bytes = 0;
	for(const auto &S : Forward.NodeSaves) {
		if(S.Kind == ActSaveKind::Full)
			Bytes += S.Full.size() * sizeof(float);
		else if(S.Kind == ActSaveKind::ReluMask)
			Bytes += S.ReluMaskBits.size();
	}
	return Bytes;
}

std::optional<GraphForwardResult> ForwardGraphF32(const CompGraph &Graph,
                                                  const std::vector<std::vector<float>> &Inputs) {
	if(Graph.InputCount == 0 || Graph.Nodes.empty() || Inputs.size() != Graph.InputCount)
		return std::nullopt;
	const size_t Len = Inputs[0].size();
	if(Len == 0 || Len > MaxAutogradLen)
		return std::nullopt;
	for(const auto &In : Inputs)
		if(In.size() != Len)
			return std::nullopt;
	const GraphPlan Plan = MathSciAutogradGraph::BuildGraphPlan(Graph);
	GraphForwardResult Result;
	Result.Inputs = Inputs;
	Result.SeqLen = Len;
	Result.NodeSaves.resize(Graph.Nodes.size());
	std::vector<std::vector<float>> Memo;
	Memo.resize(Graph.Nodes.size());
	for(std::size_t I = 0; I < Graph.Nodes.size(); ++I) {
		SavedActivation *Save = Plan.NodeSaveKind[I] != ActSaveKind::None ? &Result.NodeSaves[I] : nullptr;
		if(!ExecuteNodeForward(Graph.Nodes[I], Inputs, Memo, Graph.InputCount, Memo[I], Save))
			return std::nullopt;
	}
	Result.Output = Memo.back();
	return Result;
}

std::optional<GraphBackwardResult> BackwardGraphF32(const CompGraph &Graph, const GraphForwardResult &Forward,
                                                    const std::vector<float> &Upstream) {
	if(Graph.Nodes.empty() || Upstream.empty() || Forward.SeqLen == 0)
		return std::nullopt;
	if(Upstream.size() != Forward.SeqLen)
		return std::nullopt;
	const std::size_t SlotCount = Graph.InputCount + Graph.Nodes.size();
	std::vector<std::vector<float>> Grads(SlotCount);
	std::vector<std::vector<float>> Memo;
	Memo.resize(Graph.Nodes.size());
	Grads[SlotCount - 1] = Upstream;
	for(std::size_t I = Graph.Nodes.size(); I-- > 0;) {
		const std::uint16_t OutSlot = NodeSlot(Graph.InputCount, I);
		if(Grads[OutSlot].empty())
			continue;
		if(!ExecuteNodeBackward(Graph.Nodes[I], I, Graph, Forward, Memo, Grads[OutSlot], Grads))
			return std::nullopt;
		Grads[OutSlot].clear();
		Grads[OutSlot].shrink_to_fit();
	}
	GraphBackwardResult Result;
	Result.InputGrads.reserve(Graph.InputCount);
	for(std::uint16_t I = 0; I < Graph.InputCount; ++I)
		Result.InputGrads.push_back(std::move(Grads[I]));
	return Result;
}

std::optional<std::string> FormatGraphCache(const GraphForwardResult &Forward, std::uint16_t InputCount) {
	if(Forward.Output.empty() || Forward.SeqLen == 0)
		return std::nullopt;
	std::ostringstream O;
	O << "ADC2[" << InputCount << '|' << Forward.SeqLen << '|';
	bool FirstIn = true;
	for(const auto &In : Forward.Inputs) {
		for(float V : In) {
			if(!FirstIn)
				O << ',';
			FirstIn = false;
			O << V;
		}
	}
	O << '|' << Forward.NodeSaves.size() << '|';
	bool FirstKind = true;
	for(const auto &S : Forward.NodeSaves) {
		if(!FirstKind)
			O << ',';
		FirstKind = false;
		O << static_cast<int>(S.Kind);
	}
	for(const auto &S : Forward.NodeSaves) {
		if(S.Kind == ActSaveKind::Full) {
			for(float V : S.Full)
				O << ',' << V;
		} else if(S.Kind == ActSaveKind::ReluMask) {
			for(std::uint8_t B : S.ReluMaskBits)
				O << ',' << static_cast<int>(B);
		}
	}
	std::string Out = std::move(O).str();
	Out.push_back(']');
	return Out;
}

static std::optional<std::vector<double>> SplitCommaDoubles(std::string_view Part) {
	if(Part.empty())
		return std::vector<double>{};
	std::vector<double> Out;
	std::size_t Start = 0;
	while(Start < Part.size()) {
		const auto Com = Part.find(',', Start);
		const std::string_view Tok = Part.substr(Start, Com == std::string_view::npos ? Part.size() - Start : Com - Start);
		if(Tok.empty()) {
			if(Com == std::string_view::npos)
				break;
			Start = Com + 1;
			continue;
		}
		try {
			Out.push_back(std::stod(std::string(Tok)));
		} catch(...) {
			return std::nullopt;
		}
		if(Com == std::string_view::npos)
			break;
		Start = Com + 1;
	}
	return Out;
}

static std::optional<GraphForwardResult> ParseGraphCacheLegacy(const std::vector<double> &Flat) {
	std::size_t Pos = 0;
	if(Pos >= Flat.size())
		return std::nullopt;
	const std::uint16_t InputCount = static_cast<std::uint16_t>(Flat[Pos++]);
	GraphForwardResult Result;
	Result.Inputs.reserve(InputCount);
	for(std::uint16_t I = 0; I < InputCount; ++I) {
		if(Pos >= Flat.size())
			return std::nullopt;
		const std::size_t Len = static_cast<std::size_t>(Flat[Pos++]);
		if(Pos + Len > Flat.size())
			return std::nullopt;
		std::vector<float> In(Len);
		for(std::size_t J = 0; J < Len; ++J)
			In[J] = static_cast<float>(Flat[Pos++]);
		Result.Inputs.push_back(std::move(In));
	}
	if(Pos >= Flat.size())
		return std::nullopt;
	const std::size_t NodeCount = static_cast<std::size_t>(Flat[Pos++]);
	Result.NodeSaves.resize(NodeCount);
	for(std::size_t N = 0; N < NodeCount; ++N) {
		if(Pos >= Flat.size())
			return std::nullopt;
		const std::size_t Len = static_cast<std::size_t>(Flat[Pos++]);
		if(Pos + Len > Flat.size())
			return std::nullopt;
		Result.NodeSaves[N].Kind = ActSaveKind::Full;
		Result.NodeSaves[N].Full.resize(Len);
		for(std::size_t J = 0; J < Len; ++J)
			Result.NodeSaves[N].Full[J] = static_cast<float>(Flat[Pos++]);
	}
	if(Result.Inputs.empty() || Result.NodeSaves.empty())
		return std::nullopt;
	Result.SeqLen = Result.Inputs[0].size();
	Result.Output = Result.NodeSaves.back().Full;
	return Result;
}

std::optional<GraphForwardResult> ParseGraphCache(std::string_view Cell) {
	if(Cell.size() >= 6 && Cell.substr(0, 4) == "ADC2" && Cell.back() == ']') {
		const std::string_view Body = Cell.substr(5, Cell.size() - 6);
		std::size_t Pos = 0;
		auto NextField = [&]() -> std::string_view {
			const auto Bar = Body.find('|', Pos);
			const std::string_view F = Body.substr(Pos, Bar == std::string_view::npos ? Body.size() - Pos : Bar - Pos);
			Pos = Bar == std::string_view::npos ? Body.size() : Bar + 1;
			return F;
		};
		const auto InCountField = NextField();
		const auto SeqLenField = NextField();
		const auto InputField = NextField();
		const auto NodeCountField = NextField();
		if(InCountField.empty() || SeqLenField.empty() || NodeCountField.empty())
			return std::nullopt;
		GraphForwardResult Result;
		const std::uint16_t InputCount = static_cast<std::uint16_t>(std::stoul(std::string(InCountField)));
		Result.SeqLen = static_cast<std::size_t>(std::stoul(std::string(SeqLenField)));
		const std::size_t NodeCount = static_cast<std::size_t>(std::stoul(std::string(NodeCountField)));
		const auto InputFlat = SplitCommaDoubles(InputField);
		if(!InputFlat || InputFlat->size() != InputCount * Result.SeqLen)
			return std::nullopt;
		Result.Inputs.resize(InputCount);
		for(std::uint16_t I = 0; I < InputCount; ++I) {
			Result.Inputs[I].resize(Result.SeqLen);
			for(std::size_t J = 0; J < Result.SeqLen; ++J)
				Result.Inputs[I][J] = static_cast<float>((*InputFlat)[I * Result.SeqLen + J]);
		}
		const auto Tail = Body.substr(Pos);
		const auto TailVals = SplitCommaDoubles(Tail);
		if(!TailVals)
			return std::nullopt;
		std::size_t T = 0;
		if(T + NodeCount > TailVals->size())
			return std::nullopt;
		Result.NodeSaves.resize(NodeCount);
		for(std::size_t I = 0; I < NodeCount; ++I)
			Result.NodeSaves[I].Kind = static_cast<ActSaveKind>(static_cast<int>((*TailVals)[T++]));
		for(std::size_t I = 0; I < NodeCount; ++I) {
			if(Result.NodeSaves[I].Kind == ActSaveKind::Full) {
				if(T + Result.SeqLen > TailVals->size())
					return std::nullopt;
				Result.NodeSaves[I].Full.resize(Result.SeqLen);
				for(std::size_t J = 0; J < Result.SeqLen; ++J)
					Result.NodeSaves[I].Full[J] = static_cast<float>((*TailVals)[T++]);
			} else if(Result.NodeSaves[I].Kind == ActSaveKind::ReluMask) {
				const std::size_t Bytes = (Result.SeqLen + 7) / 8;
				if(T + Bytes > TailVals->size())
					return std::nullopt;
				Result.NodeSaves[I].ReluMaskBits.resize(Bytes);
				for(std::size_t J = 0; J < Bytes; ++J)
					Result.NodeSaves[I].ReluMaskBits[J] = static_cast<std::uint8_t>((*TailVals)[T++]);
			}
		}
		if(Result.NodeSaves.empty())
			return std::nullopt;
		Result.Output.assign(Result.SeqLen, 0.f);
		return Result;
	}
	const auto L = AdvancedTypes::ParseListCell(Cell);
	if(!L || L->size() < 2)
		return std::nullopt;
	std::vector<double> Flat;
	Flat.reserve(L->size());
	for(const auto &S : *L) {
		try {
			Flat.push_back(std::stod(S));
		} catch(...) {
			return std::nullopt;
		}
	}
	return ParseGraphCacheLegacy(Flat);
}

} // namespace MathSciAutogradGraph
} // namespace AstralDB
