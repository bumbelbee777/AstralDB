#include <Database/MathSci/MathSciAutograd.hxx>

#include <Database/MathSci/MathSciAutogradGraph.hxx>
#include <Database/MathSci/MathSciAutogradMicrokernels.hxx>
#include <Database/MathSci/MathSciSignal.hxx>
#include <Database/MathSci/MathSciSimdUtil.hxx>
#include <Database/Types/AdvancedTypes.hxx>
#include <IO/SIMD.hxx>

#include <cmath>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace AstralDB {
namespace MathSciAutograd {
namespace {

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

std::optional<std::vector<double>> ParseRealSeq(std::string_view Cell) {
	if(const auto L = AdvancedTypes::ParseListCell(Cell))
		return AsDoublesFromList(*L);
	if(const auto V = AdvancedTypes::ParseVectorCell(Cell))
		return *V;
	if(const auto C = AdvancedTypes::ParseComplexCell(Cell)) {
		std::vector<double> Out{C->first, C->second};
		return Out;
	}
	return std::nullopt;
}

std::vector<float> ToF32(const std::vector<double> &V) {
	std::vector<float> Out(V.size());
	for(size_t I = 0; I < V.size(); ++I)
		Out[I] = static_cast<float>(V[I]);
	return Out;
}

std::vector<double> ToF64(const std::vector<float> &V) {
	std::vector<double> Out(V.size());
	for(size_t I = 0; I < V.size(); ++I)
		Out[I] = static_cast<double>(V[I]);
	return Out;
}

} // namespace

std::optional<ComplexSeq> ParseComplexSeq(std::string_view Cell) {
	if(const auto C = AdvancedTypes::ParseComplexCell(Cell)) {
		ComplexSeq Out;
		Out.Interleaved = {static_cast<float>(C->first), static_cast<float>(C->second)};
		return Out;
	}
	const auto R = ParseRealSeq(Cell);
	if(!R || R->empty() || (R->size() % 2) != 0 || R->size() > MaxAutogradLen * 2)
		return std::nullopt;
	ComplexSeq Out;
	Out.Interleaved.reserve(R->size());
	for(double X : *R)
		Out.Interleaved.push_back(static_cast<float>(X));
	return Out;
}

std::vector<double> FormatComplexSeqInterleaved(const ComplexSeq &C) {
	return ToF64(C.Interleaved);
}

std::vector<float> AdHessianDiagF32(const float *SecondDeriv, const float *Upstream, size_t N) {
	std::vector<float> Out(N);
	Simd::MulF32(Out.data(), SecondDeriv, Upstream, N);
	return Out;
}

std::vector<float> AdHessianReluDiagF32(const float *X, const float *Upstream, size_t N) {
	(void)X;
	(void)Upstream;
	return std::vector<float>(N, 0.f);
}

std::vector<float> AdHessianSigmoidDiagF32(const float *Y, const float *Upstream, size_t N) {
	std::vector<float> Out(N);
	for(size_t I = 0; I < N; ++I) {
		const float Yi = Y[I];
		const float Sp = Yi * (1.f - Yi);
		Out[I] = Upstream[I] * Sp * (1.f - 2.f * Yi);
	}
	return Out;
}

std::vector<float> AdHessianSquareDiagF32(const float *X, const float *Upstream, size_t N) {
	(void)X;
	std::vector<float> Out(N);
	Simd::ScaleF32(Out.data(), Upstream, 2.f, N);
	return Out;
}

std::vector<float> AdHessianTanhDiagF32(const float *Y, const float *Upstream, size_t N) {
	std::vector<float> Out(N);
	for(size_t I = 0; I < N; ++I) {
		const float Yi = Y[I];
		const float Dp = 1.f - Yi * Yi;
		Out[I] = Upstream[I] * (-2.f * Yi * Dp);
	}
	return Out;
}

ComplexSeq WirtingerDzFromCartesianF32(const ComplexSeq &CartesianGrad) {
	const size_t N = CartesianGrad.Slots();
	ComplexSeq Out;
	Out.Interleaved.resize(N * 2);
	for(size_t I = 0; I < N; ++I) {
		const float Gx = CartesianGrad.Interleaved[I * 2];
		const float Gy = CartesianGrad.Interleaved[I * 2 + 1];
		Out.Interleaved[I * 2] = 0.5f * Gx;
		Out.Interleaved[I * 2 + 1] = -0.5f * Gy;
	}
	return Out;
}

ComplexSeq WirtingerDzBarFromCartesianF32(const ComplexSeq &CartesianGrad) {
	const size_t N = CartesianGrad.Slots();
	ComplexSeq Out;
	Out.Interleaved.resize(N * 2);
	for(size_t I = 0; I < N; ++I) {
		const float Gx = CartesianGrad.Interleaved[I * 2];
		const float Gy = CartesianGrad.Interleaved[I * 2 + 1];
		Out.Interleaved[I * 2] = 0.5f * Gx;
		Out.Interleaved[I * 2 + 1] = 0.5f * Gy;
	}
	return Out;
}

static void ConjInPlace(ComplexSeq &C) {
	for(size_t I = 0; I < C.Slots(); ++I)
		C.Interleaved[I * 2 + 1] = -C.Interleaved[I * 2 + 1];
}

ComplexSeq AdWirtingerMulLhsF32(const ComplexSeq &A, const ComplexSeq &B, const ComplexSeq &Upstream) {
	const size_t N = A.Slots();
	ComplexSeq Bc = B;
	ConjInPlace(Bc);
	ComplexSeq Out;
	Out.Interleaved.resize(N * 2);
	for(size_t I = 0; I < N; ++I) {
		const float Ur = Upstream.Interleaved[I * 2];
		const float Ui = Upstream.Interleaved[I * 2 + 1];
		const float Br = Bc.Interleaved[I * 2];
		const float Bi = Bc.Interleaved[I * 2 + 1];
		Out.Interleaved[I * 2] = Ur * Br - Ui * Bi;
		Out.Interleaved[I * 2 + 1] = Ur * Bi + Ui * Br;
	}
	(void)A;
	return Out;
}

ComplexSeq AdWirtingerMulRhsF32(const ComplexSeq &A, const ComplexSeq &B, const ComplexSeq &Upstream) {
	const size_t N = B.Slots();
	ComplexSeq Ac = A;
	ConjInPlace(Ac);
	ComplexSeq Out;
	Out.Interleaved.resize(N * 2);
	for(size_t I = 0; I < N; ++I) {
		const float Ur = Upstream.Interleaved[I * 2];
		const float Ui = Upstream.Interleaved[I * 2 + 1];
		const float Ar = Ac.Interleaved[I * 2];
		const float Ai = Ac.Interleaved[I * 2 + 1];
		Out.Interleaved[I * 2] = Ur * Ar - Ui * Ai;
		Out.Interleaved[I * 2 + 1] = Ur * Ai + Ui * Ar;
	}
	(void)B;
	return Out;
}

ComplexSeq AdWirtingerAbs2F32(const ComplexSeq &Z, const ComplexSeq &Upstream) {
	const size_t N = Z.Slots();
	ComplexSeq Out;
	Out.Interleaved.resize(N * 2);
	for(size_t I = 0; I < N; ++I) {
		const float Zr = Z.Interleaved[I * 2];
		const float Zi = Z.Interleaved[I * 2 + 1];
		const float Ur = Upstream.Interleaved[I * 2];
		const float Ui = Upstream.Interleaved[I * 2 + 1];
		Out.Interleaved[I * 2] = Ur * Zr + Ui * Zi;
		Out.Interleaved[I * 2 + 1] = Ur * Zi - Ui * Zr;
	}
	return Out;
}

ComplexSeq AdWirtingerChainF32(const ComplexSeq &LocalJac, const ComplexSeq &Upstream) {
	const size_t N = LocalJac.Slots();
	ComplexSeq Out;
	Out.Interleaved.resize(N * 2);
	for(size_t I = 0; I < N; ++I) {
		const float Lr = LocalJac.Interleaved[I * 2];
		const float Li = LocalJac.Interleaved[I * 2 + 1];
		const float Ur = Upstream.Interleaved[I * 2];
		const float Ui = Upstream.Interleaved[I * 2 + 1];
		Out.Interleaved[I * 2] = Lr * Ur - Li * Ui;
		Out.Interleaved[I * 2 + 1] = Lr * Ui + Li * Ur;
	}
	return Out;
}

std::vector<double> AdHessianDiagFromReal(const std::vector<double> &SecondDeriv, const std::vector<double> &Upstream) {
	if(SecondDeriv.size() != Upstream.size() || SecondDeriv.empty() || SecondDeriv.size() > MaxAutogradLen)
		return {};
	return ToF64(AdHessianDiagF32(ToF32(SecondDeriv).data(), ToF32(Upstream).data(), SecondDeriv.size()));
}

std::vector<double> AdHessianReluFromReal(const std::vector<double> &X, const std::vector<double> &Upstream) {
	if(X.size() != Upstream.size() || X.empty() || X.size() > MaxAutogradLen)
		return {};
	return ToF64(AdHessianReluDiagF32(ToF32(X).data(), ToF32(Upstream).data(), X.size()));
}

std::vector<double> AdHessianSigmoidFromReal(const std::vector<double> &Y, const std::vector<double> &Upstream) {
	if(Y.size() != Upstream.size() || Y.empty() || Y.size() > MaxAutogradLen)
		return {};
	return ToF64(AdHessianSigmoidDiagF32(ToF32(Y).data(), ToF32(Upstream).data(), Y.size()));
}

std::vector<double> AdHessianSquareFromReal(const std::vector<double> &X, const std::vector<double> &Upstream) {
	if(X.size() != Upstream.size() || X.empty() || X.size() > MaxAutogradLen)
		return {};
	return ToF64(AdHessianSquareDiagF32(ToF32(X).data(), ToF32(Upstream).data(), X.size()));
}

std::vector<double> AdHessianTanhFromReal(const std::vector<double> &Y, const std::vector<double> &Upstream) {
	if(Y.size() != Upstream.size() || Y.empty() || Y.size() > MaxAutogradLen)
		return {};
	return ToF64(AdHessianTanhDiagF32(ToF32(Y).data(), ToF32(Upstream).data(), Y.size()));
}

std::optional<std::vector<double>> AdWirtingerMulLhsFromCells(std::string_view A, std::string_view B,
                                                              std::string_view Upstream) {
	const auto Pa = ParseComplexSeq(A);
	const auto Pb = ParseComplexSeq(B);
	const auto Pu = ParseComplexSeq(Upstream);
	if(!Pa || !Pb || !Pu || Pa->Slots() != Pb->Slots() || Pa->Slots() != Pu->Slots() || Pa->Slots() == 0)
		return std::nullopt;
	return FormatComplexSeqInterleaved(AdWirtingerMulLhsF32(*Pa, *Pb, *Pu));
}

std::optional<std::vector<double>> AdWirtingerMulRhsFromCells(std::string_view A, std::string_view B,
                                                              std::string_view Upstream) {
	const auto Pa = ParseComplexSeq(A);
	const auto Pb = ParseComplexSeq(B);
	const auto Pu = ParseComplexSeq(Upstream);
	if(!Pa || !Pb || !Pu || Pa->Slots() != Pb->Slots() || Pa->Slots() != Pu->Slots() || Pa->Slots() == 0)
		return std::nullopt;
	return FormatComplexSeqInterleaved(AdWirtingerMulRhsF32(*Pa, *Pb, *Pu));
}

std::optional<std::vector<double>> AdWirtingerAbs2FromCells(std::string_view Z, std::string_view Upstream) {
	const auto Pz = ParseComplexSeq(Z);
	const auto Pu = ParseComplexSeq(Upstream);
	if(!Pz || !Pu || Pz->Slots() != Pu->Slots() || Pz->Slots() == 0)
		return std::nullopt;
	return FormatComplexSeqInterleaved(AdWirtingerAbs2F32(*Pz, *Pu));
}

std::optional<std::vector<double>> AdWirtingerChainFromCells(std::string_view Local, std::string_view Upstream) {
	const auto Pl = ParseComplexSeq(Local);
	const auto Pu = ParseComplexSeq(Upstream);
	if(!Pl || !Pu || Pl->Slots() != Pu->Slots() || Pl->Slots() == 0)
		return std::nullopt;
	return FormatComplexSeqInterleaved(AdWirtingerChainF32(*Pl, *Pu));
}

std::optional<std::vector<double>> WirtingerDzFromCell(std::string_view CartesianGrad) {
	const auto P = ParseComplexSeq(CartesianGrad);
	if(!P || P->Slots() == 0)
		return std::nullopt;
	return FormatComplexSeqInterleaved(WirtingerDzFromCartesianF32(*P));
}

std::optional<std::vector<double>> WirtingerDzBarFromCell(std::string_view CartesianGrad) {
	const auto P = ParseComplexSeq(CartesianGrad);
	if(!P || P->Slots() == 0)
		return std::nullopt;
	return FormatComplexSeqInterleaved(WirtingerDzBarFromCartesianF32(*P));
}

std::optional<GraphOp> ParseGraphOp(std::string_view Name) {
	if(Name == "ADD")
		return GraphOp::Add;
	if(Name == "MUL")
		return GraphOp::Mul;
	if(Name == "RELU")
		return GraphOp::Relu;
	if(Name == "SIGMOID")
		return GraphOp::Sigmoid;
	if(Name == "TANH")
		return GraphOp::Tanh;
	if(Name == "SCALE")
		return GraphOp::Scale;
	if(Name == "CHAIN")
		return GraphOp::Chain;
	if(Name == "CONST")
		return GraphOp::Const;
	if(Name == "FUSED_MUL_RELU")
		return GraphOp::FusedMulRelu;
	if(Name == "FUSED_CHAIN_MUL")
		return GraphOp::FusedChainMul;
	if(Name == "FUSED_ADD_SCALE")
		return GraphOp::FusedAddScale;
	if(Name == "FUSED_MUL_ADD")
		return GraphOp::FusedMulAdd;
	if(Name == "FUSED_MUL_SIGMOID")
		return GraphOp::FusedMulSigmoid;
	if(Name == "FUSED_MUL_TANH")
		return GraphOp::FusedMulTanh;
	if(Name == "FUSED_RELU_SCALE")
		return GraphOp::FusedReluScale;
	if(Name == "FUSED_MUL_RELU_SCALE")
		return GraphOp::FusedMulReluScale;
	return std::nullopt;
}

const char *GraphOpName(GraphOp Op) {
	switch(Op) {
	case GraphOp::Const:
		return "CONST";
	case GraphOp::Add:
		return "ADD";
	case GraphOp::Mul:
		return "MUL";
	case GraphOp::Relu:
		return "RELU";
	case GraphOp::Sigmoid:
		return "SIGMOID";
	case GraphOp::Tanh:
		return "TANH";
	case GraphOp::Scale:
		return "SCALE";
	case GraphOp::Chain:
		return "CHAIN";
	case GraphOp::FusedMulRelu:
		return "FUSED_MUL_RELU";
	case GraphOp::FusedChainMul:
		return "FUSED_CHAIN_MUL";
	case GraphOp::FusedAddScale:
		return "FUSED_ADD_SCALE";
	case GraphOp::FusedMulAdd:
		return "FUSED_MUL_ADD";
	case GraphOp::FusedMulSigmoid:
		return "FUSED_MUL_SIGMOID";
	case GraphOp::FusedMulTanh:
		return "FUSED_MUL_TANH";
	case GraphOp::FusedReluScale:
		return "FUSED_RELU_SCALE";
	case GraphOp::FusedMulReluScale:
		return "FUSED_MUL_RELU_SCALE";
	default:
		return "INPUT";
	}
}

static bool ValidSlotIndex(std::uint16_t Slot, std::uint16_t InputCount, std::size_t NodeIndex) {
	return Slot < InputCount + NodeIndex;
}

static GraphOp GraphOpFromCode(int Code) {
	switch(Code) {
	case 1:
		return GraphOp::Const;
	case 2:
		return GraphOp::Add;
	case 3:
		return GraphOp::Mul;
	case 4:
		return GraphOp::Relu;
	case 5:
		return GraphOp::Sigmoid;
	case 6:
		return GraphOp::Tanh;
	case 7:
		return GraphOp::Scale;
	case 8:
		return GraphOp::Chain;
	case 20:
		return GraphOp::FusedMulRelu;
	case 21:
		return GraphOp::FusedChainMul;
	case 22:
		return GraphOp::FusedAddScale;
	case 23:
		return GraphOp::FusedMulAdd;
	case 24:
		return GraphOp::FusedMulSigmoid;
	case 25:
		return GraphOp::FusedMulTanh;
	case 26:
		return GraphOp::FusedReluScale;
	case 27:
		return GraphOp::FusedMulReluScale;
	default:
		return GraphOp::Input;
	}
}

static bool IsGraphOpCodeValid(int Code) { return Code >= 1 && Code <= 27; }

static int GraphOpCode(GraphOp Op) {
	return static_cast<int>(Op);
}

std::optional<CompGraph> BuildGraphFromSpec(std::uint16_t InputCount, const std::vector<double> &Spec) {
	if(InputCount == 0 || InputCount > MaxGraphInputs || Spec.empty() || (Spec.size() % 4) != 0)
		return std::nullopt;
	const std::size_t NodeCount = Spec.size() / 4;
	if(NodeCount == 0 || NodeCount > MaxGraphNodes)
		return std::nullopt;
	CompGraph Graph;
	Graph.InputCount = InputCount;
	Graph.Nodes.reserve(NodeCount);
	for(std::size_t I = 0; I < NodeCount; ++I) {
		const int OpCode = static_cast<int>(Spec[I * 4]);
		const auto In0 = static_cast<std::uint16_t>(Spec[I * 4 + 1]);
		const auto In1 = static_cast<std::uint16_t>(Spec[I * 4 + 2]);
		const float Param = static_cast<float>(Spec[I * 4 + 3]);
		if(!IsGraphOpCodeValid(OpCode))
			return std::nullopt;
		if(!ValidSlotIndex(In0, InputCount, I))
			return std::nullopt;
		if(!ValidSlotIndex(In1, InputCount, I))
			return std::nullopt;
		GraphNode Node;
		Node.Op = GraphOpFromCode(OpCode);
		Node.In0 = In0;
		Node.In1 = In1;
		Node.Param = Param;
		Graph.Nodes.push_back(Node);
	}
	return Graph;
}

std::string SerializeGraph(const CompGraph &Graph) {
	std::ostringstream O;
	O << "ADG[" << Graph.InputCount << "|";
	for(std::size_t I = 0; I < Graph.Nodes.size(); ++I) {
		if(I)
			O << ';';
		const auto &N = Graph.Nodes[I];
		O << GraphOpCode(N.Op) << ',' << N.In0 << ',' << N.In1 << ',' << N.Param;
	}
	O << ']';
	return std::move(O).str();
}

static std::optional<CompGraph> DeserializeGraphBody(std::string_view Body) {
	const auto Bar = Body.find('|');
	if(Bar == std::string_view::npos)
		return std::nullopt;
	const auto InputCount = static_cast<std::uint16_t>(std::stoul(std::string(Body.substr(0, Bar))));
	if(InputCount == 0 || InputCount > MaxGraphInputs)
		return std::nullopt;
	const std::string_view NodesPart = Body.substr(Bar + 1);
	if(NodesPart.empty())
		return std::nullopt;
	CompGraph Graph;
	Graph.InputCount = InputCount;
	std::size_t Start = 0;
	while(Start < NodesPart.size()) {
		const auto End = NodesPart.find(';', Start);
		const std::string_view Token = NodesPart.substr(Start, End == std::string_view::npos ? NodesPart.size() - Start
		                                                                                        : End - Start);
		if(Token.empty())
			return std::nullopt;
		const auto C0 = Token.find(',');
		const auto C1 = Token.find(',', C0 + 1);
		const auto C2 = Token.find(',', C1 + 1);
		if(C0 == std::string_view::npos || C1 == std::string_view::npos || C2 == std::string_view::npos)
			return std::nullopt;
		GraphNode Node;
		Node.Op = GraphOpFromCode(std::stoi(std::string(Token.substr(0, C0))));
		Node.In0 = static_cast<std::uint16_t>(std::stoul(std::string(Token.substr(C0 + 1, C1 - C0 - 1))));
		Node.In1 = static_cast<std::uint16_t>(std::stoul(std::string(Token.substr(C1 + 1, C2 - C1 - 1))));
		Node.Param = static_cast<float>(std::stod(std::string(Token.substr(C2 + 1))));
		if(!ValidSlotIndex(Node.In0, InputCount, Graph.Nodes.size()) ||
		   !ValidSlotIndex(Node.In1, InputCount, Graph.Nodes.size()))
			return std::nullopt;
		Graph.Nodes.push_back(Node);
		if(Graph.Nodes.size() > MaxGraphNodes)
			return std::nullopt;
		if(End == std::string_view::npos)
			break;
		Start = End + 1;
	}
	if(Graph.Nodes.empty())
		return std::nullopt;
	return Graph;
}

std::optional<CompGraph> DeserializeGraph(std::string_view Cell) {
	if(Cell.size() < 6 || Cell.substr(0, 4) != "ADG[")
		return std::nullopt;
	if(Cell.back() != ']')
		return std::nullopt;
	return DeserializeGraphBody(Cell.substr(4, Cell.size() - 5));
}

CompGraph FuseGraph(const CompGraph &Graph) { return MathSciAutogradGraph::FuseGraph(Graph); }

size_t FusedNodeCount(const CompGraph &Before, const CompGraph &After) {
	return Before.Nodes.size() >= After.Nodes.size() ? Before.Nodes.size() - After.Nodes.size() : 0;
}

GraphPlan BuildGraphPlan(const CompGraph &Graph) { return MathSciAutogradGraph::BuildGraphPlan(Graph); }

std::size_t GraphCachePayloadBytes(const GraphForwardResult &Forward) {
	return MathSciAutogradGraph::GraphCachePayloadBytes(Forward);
}

std::optional<GraphForwardResult> ForwardGraphF32(const CompGraph &Graph,
                                                  const std::vector<std::vector<float>> &Inputs) {
	return MathSciAutogradGraph::ForwardGraphF32(Graph, Inputs);
}

std::optional<GraphBackwardResult> BackwardGraphF32(const CompGraph &Graph, const GraphForwardResult &Forward,
                                                    const std::vector<float> &Upstream) {
	return MathSciAutogradGraph::BackwardGraphF32(Graph, Forward, Upstream);
}

static std::optional<std::vector<std::vector<float>>> ParseGraphInputs(std::string_view Cell, std::uint16_t InputCount) {
	if(InputCount == 0)
		return std::nullopt;
	if(InputCount == 1) {
		const auto R = ParseRealSeq(Cell);
		if(!R || R->empty())
			return std::nullopt;
		return std::vector<std::vector<float>>{ToF32(*R)};
	}
	const auto R = ParseRealSeq(Cell);
	if(!R || R->empty())
		return std::nullopt;
	std::size_t Pos = 0;
	std::vector<std::vector<float>> Out;
	Out.reserve(InputCount);
	for(std::uint16_t I = 0; I < InputCount; ++I) {
		if(Pos >= R->size())
			return std::nullopt;
		const std::size_t Len = static_cast<std::size_t>((*R)[Pos++]);
		if(Len == 0 || Pos + Len > R->size())
			return std::nullopt;
		std::vector<float> In(Len);
		for(std::size_t J = 0; J < Len; ++J)
			In[J] = static_cast<float>((*R)[Pos++]);
		Out.push_back(std::move(In));
	}
	if(Pos != R->size())
		return std::nullopt;
	const std::size_t ExpectedLen = Out[0].size();
	for(const auto &In : Out) {
		if(In.size() != ExpectedLen)
			return std::nullopt;
	}
	return Out;
}

std::optional<std::string> BuildGraphCellFromReal(std::string_view InputCountCell, std::string_view SpecCell) {
	std::uint16_t InputCount = 0;
	if(const auto N = ParseRealSeq(InputCountCell))
		InputCount = static_cast<std::uint16_t>((*N)[0]);
	else {
		try {
			InputCount = static_cast<std::uint16_t>(std::stoul(std::string(InputCountCell)));
		} catch(...) {
			return std::nullopt;
		}
	}
	const auto Spec = ParseRealSeq(SpecCell);
	if(!Spec)
		return std::nullopt;
	const auto Graph = BuildGraphFromSpec(InputCount, *Spec);
	if(!Graph)
		return std::nullopt;
	return SerializeGraph(*Graph);
}

std::optional<std::string> FuseGraphCellFromReal(std::string_view GraphCell) {
	const auto Graph = DeserializeGraph(GraphCell);
	if(!Graph)
		return std::nullopt;
	return SerializeGraph(FuseGraph(*Graph));
}

std::optional<std::string> ForwardGraphCellFromReal(std::string_view GraphCell, std::string_view InputsCell) {
	const auto Graph = DeserializeGraph(GraphCell);
	if(!Graph)
		return std::nullopt;
	const auto Inputs = ParseGraphInputs(InputsCell, Graph->InputCount);
	if(!Inputs)
		return std::nullopt;
	const auto Forward = ForwardGraphF32(*Graph, *Inputs);
	if(!Forward)
		return std::nullopt;
	return MathSciSimdUtil::FormatListCellFromDoubles(ToF64(Forward->Output));
}

std::optional<std::string> GraphCacheCellFromReal(std::string_view GraphCell, std::string_view InputsCell) {
	const auto Graph = DeserializeGraph(GraphCell);
	if(!Graph)
		return std::nullopt;
	const auto Inputs = ParseGraphInputs(InputsCell, Graph->InputCount);
	if(!Inputs)
		return std::nullopt;
	const auto Forward = ForwardGraphF32(*Graph, *Inputs);
	if(!Forward)
		return std::nullopt;
	return MathSciAutogradGraph::FormatGraphCache(*Forward, Graph->InputCount);
}

std::optional<std::string> BackwardGraphCellFromReal(std::string_view GraphCell, std::string_view CacheCell,
                                                     std::string_view UpstreamCell) {
	const auto Graph = DeserializeGraph(GraphCell);
	if(!Graph)
		return std::nullopt;
	const auto Cache = MathSciAutogradGraph::ParseGraphCache(CacheCell);
	const auto Up = ParseRealSeq(UpstreamCell);
	if(!Cache || !Up)
		return std::nullopt;
	const auto Upf = ToF32(*Up);
	const auto Back = BackwardGraphF32(*Graph, *Cache, Upf);
	if(!Back)
		return std::nullopt;
	if(Back->InputGrads.size() == 1)
		return MathSciSimdUtil::FormatListCellFromDoubles(ToF64(Back->InputGrads[0]));
	std::vector<std::string> Subs;
	Subs.reserve(Back->InputGrads.size());
	for(const auto &G : Back->InputGrads)
		Subs.push_back(MathSciSimdUtil::FormatListCellFromDoubles(ToF64(G)));
	return AdvancedTypes::FormatListCell(Subs);
}

std::optional<std::string> GraphNodeCountCellFromReal(std::string_view GraphCell) {
	const auto Graph = DeserializeGraph(GraphCell);
	if(!Graph)
		return std::nullopt;
	return AdvancedTypes::FormatListCell({std::to_string(Graph->Nodes.size())});
}

} // namespace MathSciAutograd
} // namespace AstralDB
