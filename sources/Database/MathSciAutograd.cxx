#include <Database/MathSciAutograd.hxx>

#include <Database/AdvancedTypes.hxx>
#include <IO/SIMD.hxx>

#include <cmath>
#include <optional>

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

} // namespace MathSciAutograd
} // namespace AstralDB
