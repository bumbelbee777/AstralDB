#include <Database/MathSciComplex.hxx>

#include <Database/AdvancedTypes.hxx>
#include <Database/MathSciAutograd.hxx>
#include <IO/SIMD.hxx>

#include <cmath>
#include <sstream>
#include <string_view>

namespace AstralDB {
namespace MathSciComplex {
namespace {

std::optional<double> ParseFloat(std::string_view S) {
	if(S.empty())
		return std::nullopt;
	try {
		return std::stod(std::string(S));
	} catch(...) {
		return std::nullopt;
	}
}

std::optional<NumericVec> ParseComplexVectorCell(std::string_view Cell) {
	if(Cell.size() < 4 || Cell[0] != 'C' || Cell[1] != 'V' || Cell[2] != '[')
		return std::nullopt;
	const std::size_t Close = Cell.find(']');
	if(Close == std::string_view::npos || Close + 2 >= Cell.size() || Cell[Close + 1] != ':')
		return std::nullopt;
	const auto Decl = ParseFloat(Cell.substr(3, Close - 3));
	if(!Decl || *Decl <= 0)
		return std::nullopt;
	const std::size_t Slots = static_cast<std::size_t>(*Decl);
	std::vector<float> Out;
	Out.reserve(Slots * 2);
	std::string_view Body = Cell.substr(Close + 2);
	size_t Pos = 0;
	while(Pos < Body.size() && Out.size() < Slots * 2) {
		size_t End = Pos;
		while(End < Body.size() && Body[End] != ',')
			++End;
		const auto N = ParseFloat(Body.substr(Pos, End - Pos));
		if(!N)
			return std::nullopt;
		Out.push_back(static_cast<float>(*N));
		Pos = End + (End < Body.size() ? 1 : 0);
	}
	if(Out.size() != Slots * 2)
		return std::nullopt;
	NumericVec V;
	V.Kind = NumericKind::Complex;
	V.Values = std::move(Out);
	return V;
}

NumericVec PromoteToComplexVec(const NumericVec &V) {
	if(V.IsComplex())
		return V;
	NumericVec Out;
	Out.Kind = NumericKind::Complex;
	Out.Values.reserve(V.Dim() * 2);
	for(float X : V.Values) {
		Out.Values.push_back(X);
		Out.Values.push_back(0.f);
	}
	return Out;
}

} // namespace

std::optional<NumericVec> ParseNumericVec(std::string_view Cell) {
	if(Cell.empty())
		return std::nullopt;
	if(const auto Cv = ParseComplexVectorCell(Cell))
		return Cv;
	if(const auto C = AdvancedTypes::ParseComplexCell(Cell)) {
		NumericVec V;
		V.Kind = NumericKind::Complex;
		V.Values = {static_cast<float>(C->first), static_cast<float>(C->second)};
		return V;
	}
	if(const auto V = AdvancedTypes::ParseVectorCell(Cell)) {
		if(V->size() > MaxNumericLen)
			return std::nullopt;
		NumericVec Out;
		Out.Kind = NumericKind::Real;
		Out.Values.reserve(V->size());
		for(double X : *V)
			Out.Values.push_back(static_cast<float>(X));
		return Out;
	}
	// Parse L[…] / V[…] lists before autograd interleaved-complex heuristics so even-length
	// real lists (e.g. L[2]:1,0 for PREDICT) stay real. Complex autograd uses ParseComplexSeq.
	if(const auto L = AdvancedTypes::ParseListCell(Cell)) {
		if(L->empty())
			return NumericVec{};
		if(L->front().size() >= 2 && L->front()[0] == 'C' && L->front()[1] == '(') {
			NumericVec V;
			V.Kind = NumericKind::Complex;
			V.Values.reserve(L->size() * 2);
			for(const auto &Item : *L) {
				const auto C = AdvancedTypes::ParseComplexCell(Item);
				if(!C)
					return std::nullopt;
				V.Values.push_back(static_cast<float>(C->first));
				V.Values.push_back(static_cast<float>(C->second));
			}
			return V;
		}
		NumericVec V;
		V.Kind = NumericKind::Real;
		V.Values.reserve(L->size());
		for(const auto &Item : *L) {
			const auto N = ParseFloat(Item);
			if(!N)
				return std::nullopt;
			V.Values.push_back(static_cast<float>(*N));
		}
		return V;
	}
	return std::nullopt;
}

std::optional<NumericMat> ParseNumericMat(std::string_view Cell) {
	if(const auto M = AdvancedTypes::DecodeMatrixCell(Cell)) {
		NumericMat Out;
		Out.Kind = NumericKind::Real;
		Out.Rows = M->Rows;
		Out.Cols = M->Cols;
		Out.Flat.reserve(M->Flat.size());
		for(double X : M->Flat)
			Out.Flat.push_back(static_cast<float>(X));
		return Out;
	}
	if(Cell.size() >= 3 && Cell[0] == 'T' && Cell[1] == 'C' && Cell[2] == '[') {
		const std::size_t Br = Cell.find(']');
		if(Br == std::string_view::npos || Br + 2 >= Cell.size())
			return std::nullopt;
		const std::string_view Head = Cell.substr(3, Br - 3);
		const std::size_t Comma = Head.find(',');
		if(Comma == std::string_view::npos)
			return std::nullopt;
		const auto R = ParseFloat(Head.substr(0, Comma));
		const auto C = ParseFloat(Head.substr(Comma + 1));
		if(!R || !C)
			return std::nullopt;
		const std::size_t Rows = static_cast<std::size_t>(*R);
		const std::size_t Cols = static_cast<std::size_t>(*C);
		const std::size_t Need = Rows * Cols * 2;
		std::vector<float> Flat;
		Flat.reserve(Need);
		std::string_view Body = Cell.substr(Br + 2);
		size_t Pos = 0;
		while(Pos < Body.size() && Flat.size() < Need) {
			size_t End = Pos;
			while(End < Body.size() && Body[End] != ',')
				++End;
			const auto N = ParseFloat(Body.substr(Pos, End - Pos));
			if(!N)
				return std::nullopt;
			Flat.push_back(static_cast<float>(*N));
			Pos = End + (End < Body.size() ? 1 : 0);
		}
		if(Flat.size() != Need)
			return std::nullopt;
		NumericMat M;
		M.Kind = NumericKind::Complex;
		M.Rows = Rows;
		M.Cols = Cols;
		M.Flat = std::move(Flat);
		return M;
	}
	return std::nullopt;
}

std::string FormatNumericVec(const NumericVec &V) {
	if(V.IsComplex()) {
		if(V.Dim() == 1 && V.Values.size() == 2)
			return AdvancedTypes::FormatComplexCell(V.Values[0], V.Values[1]);
		std::ostringstream O;
		O << "CV[" << V.Dim() << "]:";
		for(std::size_t I = 0; I < V.Values.size(); ++I) {
			if(I)
				O << ',';
			O << V.Values[I];
		}
		return std::move(O).str();
	}
	std::vector<double> D(V.Values.begin(), V.Values.end());
	return AdvancedTypes::FormatVectorCell(D);
}

std::string FormatComplexScalar(float Re, float Im) { return AdvancedTypes::FormatComplexCell(Re, Im); }

float DotRealF32(const float *A, const float *B, std::size_t N) { return Simd::DotProductF32(A, B, N); }

void DotComplexHermitianF32(const float *A, const float *B, std::size_t Slots, float &OutRe, float &OutIm) {
	Simd::ComplexDotHermitianInterleavedF32(A, B, Slots, OutRe, OutIm);
}

float NormRealF32(const float *A, std::size_t N) { return std::sqrt(DotRealF32(A, A, N)); }

void NormComplexF32(const float *A, std::size_t Slots, float &OutRe, float &OutIm) {
	Simd::ComplexNormSqInterleavedF32(A, Slots, OutRe, OutIm);
	OutRe = std::sqrt(OutRe);
	OutIm = 0.f;
}

NumericVec Add(const NumericVec &A, const NumericVec &B) {
	NumericVec Out;
	Out.Kind = A.Kind;
	if(A.Kind != B.Kind || A.Dim() != B.Dim())
		return {};
	Out.Values.resize(A.FloatCount());
	if(A.IsComplex())
		Simd::ComplexAddInterleavedF32(Out.Values.data(), A.Values.data(), B.Values.data(), A.Dim());
	else
		Simd::AddF32(Out.Values.data(), A.Values.data(), B.Values.data(), A.Dim());
	return Out;
}

std::optional<float> DotReal(const NumericVec &A, const NumericVec &B) {
	if(A.IsComplex() || B.IsComplex() || A.Dim() != B.Dim())
		return std::nullopt;
	return DotRealF32(A.Values.data(), B.Values.data(), A.Dim());
}

std::optional<std::pair<float, float>> DotComplex(const NumericVec &A, const NumericVec &B) {
	if(!A.IsComplex() || !B.IsComplex() || A.Dim() != B.Dim())
		return std::nullopt;
	float Re = 0.f;
	float Im = 0.f;
	DotComplexHermitianF32(A.Values.data(), B.Values.data(), A.Dim(), Re, Im);
	return std::make_pair(Re, Im);
}

std::optional<float> NormReal(const NumericVec &A) {
	if(A.IsComplex())
		return std::nullopt;
	return NormRealF32(A.Values.data(), A.Dim());
}

std::optional<std::pair<float, float>> NormComplex(const NumericVec &A) {
	if(!A.IsComplex())
		return std::nullopt;
	float Re = 0.f;
	float Im = 0.f;
	NormComplexF32(A.Values.data(), A.Dim(), Re, Im);
	return std::make_pair(Re, Im);
}

std::optional<NumericVec> MatVec(const NumericMat &M, const NumericVec &V) {
	if(M.Kind != V.Kind)
		return std::nullopt;
	NumericVec Out;
	Out.Kind = M.Kind;
	if(M.Kind == NumericKind::Real) {
		if(V.Dim() != M.Cols)
			return std::nullopt;
		Out.Values.resize(M.Rows);
		Simd::MatrixVectorMulF32(M.Flat.data(), V.Values.data(), Out.Values.data(), M.Rows, M.Cols);
		return Out;
	}
	if(V.Dim() != M.Cols)
		return std::nullopt;
	Out.Values.assign(M.Rows * 2, 0.f);
	Simd::ComplexMatVecInterleavedF32(M.Flat.data(), V.Values.data(), Out.Values.data(), M.Rows, M.Cols);
	return Out;
}

std::optional<double> CosineSim(const NumericVec &A, const NumericVec &B) {
	if(A.Kind != B.Kind || A.Dim() != B.Dim())
		return std::nullopt;
	if(A.IsComplex()) {
		const auto Dot = DotComplex(A, B);
		const auto Na = NormComplex(A);
		const auto Nb = NormComplex(B);
		if(!Dot || !Na || !Nb || Na->first == 0.f || Nb->first == 0.f)
			return std::nullopt;
		const double Num = static_cast<double>(Dot->first);
		const double Den = static_cast<double>(Na->first) * static_cast<double>(Nb->first);
		return Num / Den;
	}
	const auto Dot = DotReal(A, B);
	const auto Na = NormReal(A);
	const auto Nb = NormReal(B);
	if(!Dot || !Na || !Nb || *Na == 0.f || *Nb == 0.f)
		return std::nullopt;
	return static_cast<double>(*Dot) / (static_cast<double>(*Na) * static_cast<double>(*Nb));
}

NumericVec MeanPool(const std::vector<NumericVec> &Rows) {
	if(Rows.empty())
		return {};
	NumericVec Out;
	Out.Kind = Rows.front().Kind;
	Out.Values.assign(Rows.front().FloatCount(), 0.f);
	for(const auto &R : Rows) {
		if(R.Kind != Out.Kind || R.FloatCount() != Out.FloatCount())
			return {};
		if(Out.IsComplex())
			Simd::ComplexAddInterleavedF32(Out.Values.data(), Out.Values.data(), R.Values.data(), Out.Dim());
		else
			Simd::AddF32(Out.Values.data(), Out.Values.data(), R.Values.data(), Out.Dim());
	}
	const float Inv = 1.f / static_cast<float>(Rows.size());
	if(Out.IsComplex())
		Simd::ComplexScaleInterleavedF32(Out.Values.data(), Out.Values.data(), Inv, 0.f, Out.Dim());
	else
		Simd::ScaleF32(Out.Values.data(), Out.Values.data(), Inv, Out.Dim());
	return Out;
}

NumericVec Scale(const NumericVec &V, float Scalar) {
	NumericVec Out = V;
	Simd::ScaleF32(Out.Values.data(), V.Values.data(), Scalar, V.Dim());
	return Out;
}

NumericVec ScaleComplex(const NumericVec &V, float ScaleRe, float ScaleIm) {
	NumericVec Out;
	Out.Kind = NumericKind::Complex;
	Out.Values.resize(V.FloatCount());
	Simd::ComplexScaleInterleavedF32(Out.Values.data(), V.Values.data(), ScaleRe, ScaleIm, V.Dim());
	return Out;
}

std::optional<std::string> DotCellFromReal(const std::string &A, const std::string &B) {
	const auto Pa = ParseNumericVec(A);
	const auto Pb = ParseNumericVec(B);
	if(!Pa || !Pb)
		return std::nullopt;
	if(Pa->IsComplex() || Pb->IsComplex()) {
		const auto W = PromoteToComplexVec(*Pa);
		const auto X = PromoteToComplexVec(*Pb);
		const auto D = DotComplex(W, X);
		if(!D)
			return std::nullopt;
		return FormatComplexScalar(D->first, D->second);
	}
	const auto D = DotReal(*Pa, *Pb);
	if(!D)
		return std::nullopt;
	std::ostringstream O;
	O << *D;
	return std::move(O).str();
}

std::optional<std::string> AddCellFromReal(const std::string &A, const std::string &B) {
	const auto Pa = ParseNumericVec(A);
	const auto Pb = ParseNumericVec(B);
	if(!Pa || !Pb)
		return std::nullopt;
	return FormatNumericVec(Add(*Pa, *Pb));
}

std::optional<std::string> NormCellFromReal(const std::string &A) {
	const auto Pa = ParseNumericVec(A);
	if(!Pa)
		return std::nullopt;
	if(Pa->IsComplex()) {
		const auto N = NormComplex(*Pa);
		if(!N)
			return std::nullopt;
		return FormatComplexScalar(N->first, N->second);
	}
	const auto N = NormReal(*Pa);
	if(!N)
		return std::nullopt;
	std::ostringstream O;
	O << *N;
	return std::move(O).str();
}

std::optional<std::string> MatVecCellFromReal(const std::string &M, const std::string &V) {
	const auto Pm = ParseNumericMat(M);
	const auto Pv = ParseNumericVec(V);
	if(!Pm || !Pv)
		return std::nullopt;
	const auto Out = MatVec(*Pm, *Pv);
	if(!Out)
		return std::nullopt;
	return FormatNumericVec(*Out);
}

std::optional<std::string> CosineSimCellFromReal(const std::string &A, const std::string &B) {
	const auto Pa = ParseNumericVec(A);
	const auto Pb = ParseNumericVec(B);
	if(!Pa || !Pb)
		return std::nullopt;
	const auto S = CosineSim(*Pa, *Pb);
	if(!S)
		return std::nullopt;
	std::ostringstream O;
	O.precision(12);
	O << *S;
	return std::move(O).str();
}

} // namespace MathSciComplex
} // namespace AstralDB
