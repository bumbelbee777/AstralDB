#include <Database/MathSci/MathSciHeatKernel.hxx>

#include <Database/MathSci/MathSciListCache.hxx>
#include <Database/MathSci/MathSciSimdUtil.hxx>
#include <Database/MathSci/MathSciSignal.hxx>
#include <Database/MathSci/MathSciVision.hxx>
#include <Database/Types/AdvancedTypes.hxx>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace AstralDB {
namespace MathSciHeatKernel {
namespace {

std::optional<double> ToNum(std::string_view S) {
	if(S.empty())
		return std::nullopt;
	try {
		return std::stod(std::string(S));
	} catch(...) {
		return std::nullopt;
	}
}

std::optional<std::size_t> ToSize(std::string_view S) {
	const auto N = ToNum(S);
	if(!N || *N < 0.0)
		return std::nullopt;
	return static_cast<std::size_t>(*N);
}

std::optional<std::vector<double>> ParseList(std::string_view Cell) {
	if(const auto *Cached = MathSciListCache::LookupDoubles(Cell))
		return *Cached;
	if(const auto L = AdvancedTypes::ParseListCell(Cell)) {
		std::vector<double> Out;
		Out.reserve(L->size());
		for(const auto &S : *L) {
			const auto V = ToNum(S);
			if(!V)
				return std::nullopt;
			Out.push_back(*V);
		}
		return Out;
	}
	if(const auto V = AdvancedTypes::ParseVectorCell(Cell))
		return *V;
	return std::nullopt;
}

void ApplyGraphLaplacian(const std::vector<double> &U, const std::vector<std::pair<std::size_t, std::size_t>> &Edges,
                         std::vector<double> &Out) {
	const std::size_t N = U.size();
	Out.assign(N, 0.0);
	std::vector<double> Deg(N, 0.0);
	for(const auto &[I, J] : Edges) {
		if(I >= N || J >= N)
			continue;
		Deg[I] += 1.0;
		Deg[J] += 1.0;
	}
	for(std::size_t I = 0; I < N; ++I)
		Out[I] = Deg[I] * U[I];
	for(const auto &[I, J] : Edges) {
		if(I >= N || J >= N)
			continue;
		Out[I] -= U[J];
		Out[J] -= U[I];
	}
}

std::optional<std::vector<std::pair<std::size_t, std::size_t>>>
ParseEdges(const std::vector<double> &Raw, std::size_t NumNodes) {
	if(Raw.empty() || (Raw.size() % 2) != 0 || Raw.size() > MaxGraphEdges * 2)
		return std::nullopt;
	std::vector<std::pair<std::size_t, std::size_t>> Edges;
	Edges.reserve(Raw.size() / 2);
	for(std::size_t I = 0; I + 1 < Raw.size(); I += 2) {
		const auto A = static_cast<std::size_t>(Raw[I]);
		const auto B = static_cast<std::size_t>(Raw[I + 1]);
		if(A >= NumNodes || B >= NumNodes || A == B)
			return std::nullopt;
		Edges.emplace_back(A, B);
	}
	return Edges;
}

std::vector<float> GaussianKernel1d(float Sigma, std::size_t &Radius) {
	if(Sigma <= 0.f)
		Sigma = 1.f;
	Radius = static_cast<std::size_t>(std::ceil(3.f * Sigma));
	const std::size_t K = 2 * Radius + 1;
	std::vector<float> Kern(K);
	float Sum = 0.f;
	const float Inv2Sig2 = 1.f / (2.f * Sigma * Sigma);
	for(std::size_t I = 0; I < K; ++I) {
		const float X = static_cast<float>(static_cast<int>(I) - static_cast<int>(Radius));
		Kern[I] = std::exp(-X * X * Inv2Sig2);
		Sum += Kern[I];
	}
	if(Sum > 0.f)
		for(float &V : Kern)
			V /= Sum;
	return Kern;
}

} // namespace

std::vector<double> GraphStepFromReal(const std::vector<double> &U, const std::vector<double> &EdgesRaw, double Dt) {
	if(U.empty() || U.size() > MaxGraphNodes || Dt <= 0.0)
		return {};
	const auto Edges = ParseEdges(EdgesRaw, U.size());
	if(!Edges)
		return {};
	std::vector<double> Lu;
	ApplyGraphLaplacian(U, *Edges, Lu);
	std::vector<double> Out(U.size());
	for(std::size_t I = 0; I < U.size(); ++I)
		Out[I] = U[I] - Dt * Lu[I];
	return Out;
}

std::vector<double> GraphMarchFromReal(const std::vector<double> &U, const std::vector<double> &EdgesRaw, double Dt,
                                       std::size_t Steps) {
	if(Steps == 0 || Steps > 8192)
		return U;
	std::vector<double> Cur = U;
	for(std::size_t S = 0; S < Steps; ++S) {
		Cur = GraphStepFromReal(Cur, EdgesRaw, Dt);
		if(Cur.empty())
			return {};
	}
	return Cur;
}

std::vector<double> GraphApplyFromReal(const std::vector<double> &U, const std::vector<double> &EdgesRaw, double Tau) {
	if(Tau <= 0.0)
		return U;
	const std::size_t Steps = static_cast<std::size_t>(std::min(512.0, std::max(1.0, std::ceil(Tau * 32.0))));
	const double Dt = Tau / static_cast<double>(Steps);
	return GraphMarchFromReal(U, EdgesRaw, Dt, Steps);
}

std::vector<double> Heat1dFromReal(const std::vector<double> &Signal, double Sigma) {
	if(Signal.empty() || Signal.size() > MaxSignalLen)
		return {};
	std::size_t Radius = 0;
	const auto Kern = GaussianKernel1d(static_cast<float>(Sigma), Radius);
	const auto Inf = MathSciSimdUtil::SeqToF32(Signal);
	const auto Out = MathSciSignal::Conv1dSameF32(Inf.data(), Inf.size(), Kern.data(), Kern.size());
	return MathSciSimdUtil::ToF64(Out);
}

static std::vector<double> Heat2dPlaneFromReal(const std::vector<double> &Grid, std::size_t W, std::size_t H,
                                               const std::vector<double> &KernD) {
	std::vector<double> Tmp(Grid.size());
	std::vector<double> Out(Grid.size());
	for(std::size_t Y = 0; Y < H; ++Y) {
		std::vector<double> Row(W);
		for(std::size_t X = 0; X < W; ++X)
			Row[X] = Grid[Y * W + X];
		const auto Sm = MathSciSignal::Conv1dSameFromReal(Row, KernD);
		for(std::size_t X = 0; X < W; ++X)
			Tmp[Y * W + X] = Sm[X];
	}
	for(std::size_t X = 0; X < W; ++X) {
		std::vector<double> Col(H);
		for(std::size_t Y = 0; Y < H; ++Y)
			Col[Y] = Tmp[Y * W + X];
		const auto Sm = MathSciSignal::Conv1dSameFromReal(Col, KernD);
		for(std::size_t Y = 0; Y < H; ++Y)
			Out[Y * W + X] = Sm[Y];
	}
	return Out;
}

std::vector<double> Heat2dFromReal(const std::vector<double> &Grid, std::size_t W, std::size_t H, double Sigma) {
	if(W == 0 || H == 0 || W * H > MaxSignalLen || Grid.size() % (W * H) != 0)
		return {};
	const std::size_t C = Grid.size() / (W * H);
	std::size_t Radius = 0;
	const auto Kern = GaussianKernel1d(static_cast<float>(Sigma), Radius);
	const auto KernD = MathSciSimdUtil::ToF64(Kern);
	std::vector<double> Out(Grid.size());
	const std::size_t Plane = W * H;
	for(std::size_t Ch = 0; Ch < C; ++Ch) {
		std::vector<double> PlaneIn(Plane);
		for(std::size_t I = 0; I < Plane; ++I)
			PlaneIn[I] = Grid[I * C + Ch];
		const auto PlaneOut = Heat2dPlaneFromReal(PlaneIn, W, H, KernD);
		for(std::size_t I = 0; I < Plane; ++I)
			Out[I * C + Ch] = PlaneOut[I];
	}
	return Out;
}

std::vector<double> Grad1dFromReal(const std::vector<double> &Signal, double Sigma) {
	if(Signal.empty() || Signal.size() > MaxSignalLen)
		return {};
	const auto Smooth = Heat1dFromReal(Signal, Sigma);
	if(Smooth.size() < 2)
		return {};
	std::vector<double> Out(Smooth.size(), 0.0);
	for(std::size_t I = 1; I + 1 < Smooth.size(); ++I)
		Out[I] = 0.5 * (Smooth[I + 1] - Smooth[I - 1]);
	if(Smooth.size() >= 2) {
		Out[0] = Smooth[1] - Smooth[0];
		Out[Smooth.size() - 1] = Smooth[Smooth.size() - 1] - Smooth[Smooth.size() - 2];
	}
	return Out;
}

std::optional<std::string> GraphStepCellFromReal(const std::string &U, const std::string &Edges, const std::string &Dt) {
	const auto Uv = ParseList(U);
	const auto Ev = ParseList(Edges);
	const auto Dtv = ToNum(Dt);
	if(!Uv || !Ev || !Dtv)
		return std::nullopt;
	const auto Out = GraphStepFromReal(*Uv, *Ev, *Dtv);
	if(Out.empty())
		return std::nullopt;
	return MathSciSimdUtil::FormatListCellFromDoubles(Out);
}

std::optional<std::string> GraphMarchCellFromReal(const std::string &U, const std::string &Edges, const std::string &Dt,
                                                  const std::string &Steps) {
	const auto Uv = ParseList(U);
	const auto Ev = ParseList(Edges);
	const auto Dtv = ToNum(Dt);
	const auto St = ToSize(Steps);
	if(!Uv || !Ev || !Dtv || !St)
		return std::nullopt;
	const auto Out = GraphMarchFromReal(*Uv, *Ev, *Dtv, *St);
	if(Out.empty())
		return std::nullopt;
	return MathSciSimdUtil::FormatListCellFromDoubles(Out);
}

std::optional<std::string> GraphApplyCellFromReal(const std::string &U, const std::string &Edges,
                                                    const std::string &Tau) {
	const auto Uv = ParseList(U);
	const auto Ev = ParseList(Edges);
	const auto Tv = ToNum(Tau);
	if(!Uv || !Ev || !Tv)
		return std::nullopt;
	const auto Out = GraphApplyFromReal(*Uv, *Ev, *Tv);
	if(Out.empty())
		return std::nullopt;
	return MathSciSimdUtil::FormatListCellFromDoubles(Out);
}

std::optional<std::string> Heat1dCellFromReal(const std::string &Signal, const std::string &Sigma) {
	const auto Sv = ParseList(Signal);
	const auto Sig = ToNum(Sigma);
	if(!Sv || !Sig)
		return std::nullopt;
	const auto Out = Heat1dFromReal(*Sv, *Sig);
	if(Out.empty())
		return std::nullopt;
	return MathSciSimdUtil::FormatListCellFromDoubles(Out);
}

std::optional<std::string> Heat2dCellFromReal(const std::string &Img, const std::string &Sigma) {
	const auto Parsed = MathSciVision::ParseImageCell(Img);
	const auto Sig = ToNum(Sigma);
	if(!Parsed || !Sig)
		return std::nullopt;
	std::vector<double> Grid(Parsed->Data.size());
	for(std::size_t I = 0; I < Parsed->Data.size(); ++I)
		Grid[I] = static_cast<double>(Parsed->Data[I]);
	const auto Out = Heat2dFromReal(Grid, Parsed->W, Parsed->H, *Sig);
	if(Out.empty())
		return std::nullopt;
	MathSciVision::ImageF32 Res{Parsed->W, Parsed->H, Parsed->C, {}};
	Res.Data.resize(Out.size());
	for(std::size_t I = 0; I < Out.size(); ++I)
		Res.Data[I] = static_cast<float>(Out[I]);
	return MathSciVision::FormatImageCell(Res);
}

std::optional<std::string> Grad1dCellFromReal(const std::string &Signal, const std::string &Sigma) {
	const auto Sv = ParseList(Signal);
	const auto Sig = ToNum(Sigma);
	if(!Sv || !Sig)
		return std::nullopt;
	const auto Out = Grad1dFromReal(*Sv, *Sig);
	if(Out.empty())
		return std::nullopt;
	return MathSciSimdUtil::FormatListCellFromDoubles(Out);
}

} // namespace MathSciHeatKernel
} // namespace AstralDB
