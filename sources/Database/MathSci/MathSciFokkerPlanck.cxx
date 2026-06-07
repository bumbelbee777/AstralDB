#include <Database/MathSci/MathSciFokkerPlanck.hxx>

#include <Database/MathSci/MathSciComplex.hxx>
#include <Database/MathSci/MathSciListCache.hxx>
#include <Database/MathSci/MathSciSimdUtil.hxx>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace AstralDB {
namespace MathSciFokkerPlanck {
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

const std::vector<double> *CachedRealList(std::string_view Cell) {
	return MathSciListCache::LookupDoubles(Cell);
}

std::optional<std::size_t> ToSize(std::string_view S) {
	const auto N = ToNum(S);
	if(!N || *N < 0.0)
		return std::nullopt;
	return static_cast<std::size_t>(*N);
}

double TrapzWeight(const std::vector<double> &Grid, std::size_t I) {
	const std::size_t N = Grid.size();
	if(N < 2)
		return 1.0;
	if(I == 0)
		return 0.5 * (Grid[1] - Grid[0]);
	if(I + 1 == N)
		return 0.5 * (Grid[N - 1] - Grid[N - 2]);
	return 0.5 * (Grid[I + 1] - Grid[I - 1]);
}

double AggregateK(const std::vector<double> &G, const std::vector<double> &Grid) {
	double K = 0.0;
	for(std::size_t I = 0; I < G.size(); ++I)
		K += Grid[I] * G[I] * TrapzWeight(Grid, I);
	return K;
}

void NormalizeMass(std::vector<double> &G, const std::vector<double> &Grid) {
	double M = 0.0;
	for(std::size_t I = 0; I < G.size(); ++I)
		M += G[I] * TrapzWeight(Grid, I);
	if(M > 1e-15)
		for(double &V : G)
			V /= M;
}

} // namespace

std::vector<double> NfpMacroStepFromReal(const std::vector<double> &GIn, const std::vector<double> &Grid,
                                           const std::vector<double> &Drift, const std::vector<double> &Diffusion,
                                           const std::vector<double> &NeuralCorr, double AggK, double KStar,
                                           double BarrierA, double Dt) {
	const std::size_t N = GIn.size();
	if(N < 3 || N > MaxNfpGrid || Grid.size() != N || Drift.size() != N)
		return {};
	std::vector<double> G = GIn;
	NormalizeMass(G, Grid);

	const double K = AggregateK(G, Grid);
	const double KFeed = AggK > 0.0 ? AggK : K;
	const double Gap = KFeed - KStar;

	std::vector<double> Mu(N);
	std::vector<double> Sig2(N);
	for(std::size_t I = 0; I < N; ++I) {
		const double Nu = (I < NeuralCorr.size()) ? NeuralCorr[I] : 0.0;
		Mu[I] = Drift[I] + Nu + Gap * 0.05 * (Grid[I] / std::max(Grid.back(), 1e-9));
		const double D = (I < Diffusion.size()) ? Diffusion[I] : Diffusion.back();
		Sig2[I] = D * D;
	}

	double MaxSig2 = 1e-12;
	for(double S : Sig2)
		MaxSig2 = std::max(MaxSig2, S);

	double DtEff = Dt;
	for(std::size_t I = 1; I + 1 < N; ++I) {
		const double Dw = Grid[I + 1] - Grid[I - 1];
		if(Dw > 1e-12) {
			const double Cfl = 0.45 * Dw * Dw / MaxSig2;
			DtEff = std::min(DtEff, Cfl);
		}
	}

	std::vector<double> GNew(N, 0.0);
	for(std::size_t I = 0; I < N; ++I) {
		if(Grid[I] <= BarrierA + 1e-12) {
			GNew[I] = 0.0;
			continue;
		}
		const double DwPlus = (I + 1 < N) ? (Grid[I + 1] - Grid[I]) : (Grid[I] - Grid[I - 1]);
		const double DwMinus = (I > 0) ? (Grid[I] - Grid[I - 1]) : DwPlus;
		const double MuPlus = (I + 1 < N) ? 0.5 * (Mu[I] + Mu[I + 1]) : Mu[I];
		const double MuMinus = (I > 0) ? 0.5 * (Mu[I] + Mu[I - 1]) : Mu[I];
		const double GPlus = (I + 1 < N) ? G[I + 1] : G[I];
		const double GMinus = (I > 0) ? G[I - 1] : G[I];

		const double Adv = -(MuPlus * GPlus - MuMinus * GMinus) / std::max(DwPlus + DwMinus, 1e-12);
		const double SigP = (I + 1 < N) ? 0.5 * (Sig2[I] + Sig2[I + 1]) : Sig2[I];
		const double SigM = (I > 0) ? 0.5 * (Sig2[I] + Sig2[I - 1]) : Sig2[I];
		const double Diff = 0.5 * (SigP * (GPlus - G[I]) / std::max(DwPlus, 1e-12) -
		                           SigM * (G[I] - GMinus) / std::max(DwMinus, 1e-12)) /
		                    std::max(0.5 * (DwPlus + DwMinus), 1e-12);

		GNew[I] = std::max(0.0, G[I] + DtEff * (Adv + Diff));
	}
	NormalizeMass(GNew, Grid);
	return GNew;
}

std::vector<double> NfpMacroMarchFromReal(const std::vector<double> &G, const std::vector<double> &Grid,
                                          const std::vector<double> &Drift, const std::vector<double> &Diffusion,
                                          const std::vector<double> &NeuralCorr, double AggK, double KStar,
                                          double BarrierA, double Dt, std::size_t Steps) {
	if(Steps == 0 || Steps > MaxNfpMarchSteps)
		return {};
	std::vector<double> Cur = G;
	for(std::size_t S = 0; S < Steps; ++S) {
		Cur = NfpMacroStepFromReal(Cur, Grid, Drift, Diffusion, NeuralCorr, AggK, KStar, BarrierA, Dt);
		if(Cur.empty())
			return {};
	}
	return Cur;
}

std::vector<double> NfpMacroMomentsFromReal(const std::vector<double> &GIn, const std::vector<double> &Grid) {
	const std::size_t N = GIn.size();
	if(N == 0 || N > MaxNfpGrid || Grid.size() != N)
		return {};
	std::vector<double> G = GIn;
	NormalizeMass(G, Grid);
	double Mean = 0.0;
	double Mass = 0.0;
	for(std::size_t I = 0; I < N; ++I) {
		const double W = TrapzWeight(Grid, I);
		Mass += G[I] * W;
		Mean += Grid[I] * G[I] * W;
	}
	double Var = 0.0;
	for(std::size_t I = 0; I < N; ++I) {
		const double D = Grid[I] - Mean;
		Var += D * D * G[I] * TrapzWeight(Grid, I);
	}
	std::vector<std::pair<double, double>> Pairs;
	Pairs.reserve(N);
	for(std::size_t I = 0; I < N; ++I)
		Pairs.emplace_back(Grid[I], G[I] * TrapzWeight(Grid, I));
	std::sort(Pairs.begin(), Pairs.end());
	double Cum = 0.0;
	double GiniNum = 0.0;
	for(const auto &[W, M] : Pairs) {
		GiniNum += std::abs(Cum + 0.5 * M - 0.5);
		Cum += M;
	}
	const double Gini = Mass > 1e-15 ? std::min(1.0, std::max(0.0, 2.0 * GiniNum / Mass)) : 0.0;
	return {Mass, Mean, Var, Gini};
}

std::optional<std::string> NfpMacroStepCellFromReal(const std::string &G, const std::string &Grid,
                                                      const std::string &Drift, const std::string &Diffusion,
                                                      const std::string &NeuralCorr, const std::string &AggK,
                                                      const std::string &KStar, const std::string &BarrierA,
                                                      const std::string &Dt) {
	const auto *Gv = CachedRealList(G);
	const auto *Gridv = CachedRealList(Grid);
	const auto *Driftv = CachedRealList(Drift);
	const auto *Diffv = CachedRealList(Diffusion);
	const auto *Neuv = CachedRealList(NeuralCorr);
	const auto Ak = ToNum(AggK);
	const auto Ks = ToNum(KStar);
	const auto Bar = ToNum(BarrierA);
	const auto Dtv = ToNum(Dt);
	if(!Gv || !Gridv || !Driftv || !Diffv || !Neuv || !Ak || !Ks || !Bar || !Dtv)
		return std::nullopt;
	const auto Out = NfpMacroStepFromReal(*Gv, *Gridv, *Driftv, *Diffv, *Neuv, *Ak, *Ks, *Bar, *Dtv);
	if(Out.empty())
		return std::nullopt;
	return MathSciSimdUtil::FormatListCellFromDoubles(Out);
}

std::optional<std::string> NfpMacroMarchCellFromReal(const std::string &G, const std::string &Grid,
                                                       const std::string &Drift, const std::string &Diffusion,
                                                       const std::string &NeuralCorr, const std::string &AggK,
                                                       const std::string &KStar, const std::string &BarrierA,
                                                       const std::string &Dt, const std::string &Steps) {
	static std::mutex Mu;
	static std::unordered_map<std::string, std::string> Memo;
	std::string Key;
	Key.reserve(G.size() + Grid.size() + Drift.size() + Diffusion.size() + NeuralCorr.size() + 64);
	Key.append(G).push_back('\0');
	Key.append(Grid).push_back('\0');
	Key.append(Drift).push_back('\0');
	Key.append(Diffusion).push_back('\0');
	Key.append(NeuralCorr).push_back('\0');
	Key.append(AggK).push_back('\0');
	Key.append(KStar).push_back('\0');
	Key.append(BarrierA).push_back('\0');
	Key.append(Dt).push_back('\0');
	Key.append(Steps);
	{
		std::lock_guard<std::mutex> Lock(Mu);
		if(const auto It = Memo.find(Key); It != Memo.end())
			return It->second;
	}
	const auto *Gv = CachedRealList(G);
	const auto *Gridv = CachedRealList(Grid);
	const auto *Driftv = CachedRealList(Drift);
	const auto *Diffv = CachedRealList(Diffusion);
	const auto *Neuv = CachedRealList(NeuralCorr);
	const auto Ak = ToNum(AggK);
	const auto Ks = ToNum(KStar);
	const auto Bar = ToNum(BarrierA);
	const auto Dtv = ToNum(Dt);
	const auto St = ToSize(Steps);
	if(!Gv || !Gridv || !Driftv || !Diffv || !Neuv || !Ak || !Ks || !Bar || !Dtv || !St)
		return std::nullopt;
	const auto Out = NfpMacroMarchFromReal(*Gv, *Gridv, *Driftv, *Diffv, *Neuv, *Ak, *Ks, *Bar, *Dtv, *St);
	if(Out.empty())
		return std::nullopt;
	const std::string Formatted = MathSciSimdUtil::FormatListCellFromDoubles(Out);
	{
		std::lock_guard<std::mutex> Lock(Mu);
		Memo.emplace(Key, Formatted);
	}
	return Formatted;
}

std::optional<std::string> NfpMacroMomentsCellFromReal(const std::string &G, const std::string &Grid) {
	const auto *Gv = CachedRealList(G);
	const auto *Gridv = CachedRealList(Grid);
	if(!Gv || !Gridv)
		return std::nullopt;
	const auto Out = NfpMacroMomentsFromReal(*Gv, *Gridv);
	if(Out.empty())
		return std::nullopt;
	return MathSciSimdUtil::FormatListCellFromDoubles(Out);
}

} // namespace MathSciFokkerPlanck
} // namespace AstralDB
