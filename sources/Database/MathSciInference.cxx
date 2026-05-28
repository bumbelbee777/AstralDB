#include <Database/MathSciInference.hxx>

#include <Database/AdvancedTypes.hxx>
#include <Database/MathSciComplex.hxx>
#include <Database/MathSciSimdUtil.hxx>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace AstralDB {
namespace MathSciInference {
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

std::optional<std::vector<double>> ParseRealList(std::string_view Cell) {
	const auto V = MathSciComplex::ParseNumericVec(Cell);
	if(!V || V->Kind != MathSciComplex::NumericKind::Real)
		return std::nullopt;
	std::vector<double> Out(V->Values.size());
	for(std::size_t I = 0; I < V->Values.size(); ++I)
		Out[I] = static_cast<double>(V->Values[I]);
	return Out;
}

std::optional<std::size_t> ToSize(std::string_view S) {
	const auto N = ToNum(S);
	if(!N || *N < 0.0)
		return std::nullopt;
	return static_cast<std::size_t>(*N);
}

std::string FmtNum(double X) {
	char Buf[64];
	std::snprintf(Buf, sizeof(Buf), "%.12g", X);
	return Buf;
}

} // namespace

std::size_t MctsUctPickFromReal(const std::vector<double> &Q, const std::vector<double> &Visits,
                                const std::vector<double> &Priors, double ParentVisits, double Exploration) {
	const std::size_t K = Q.size();
	if(K == 0 || K > MaxMctsActions || Visits.size() != K)
		return 0;
	const double C = std::max(Exploration, 0.0);
	const double LogParent = std::log(std::max(ParentVisits, 1.0));
	std::size_t Best = 0;
	double BestScore = -std::numeric_limits<double>::infinity();
	for(std::size_t I = 0; I < K; ++I) {
		const double Ni = std::max(Visits[I], 0.0);
		const double Prior = (I < Priors.size() && Priors[I] > 0.0) ? Priors[I] : 1.0;
		double Score = 0.0;
		if(Ni < 1e-12)
			Score = 1e9 + Prior;
		else {
			Score = Q[I] / Ni + C * Prior * std::sqrt(LogParent / Ni);
		}
		if(Score > BestScore) {
			BestScore = Score;
			Best = I;
		}
	}
	return Best;
}

std::size_t MctsSearchFromReal(const std::vector<double> &Rewards, const std::vector<double> &Priors,
                               std::size_t Iterations, double Exploration) {
	const std::size_t K = Rewards.size();
	if(K == 0 || K > MaxMctsActions)
		return 0;
	const std::size_t It = std::min(Iterations, MaxMctsIterations);
	std::vector<double> Q(K, 0.0);
	std::vector<double> N(K, 0.0);
	std::vector<double> P = Priors;
	if(P.size() != K) {
		P.assign(K, 1.0 / static_cast<double>(K));
	} else {
		double Sum = 0.0;
		for(double V : P)
			Sum += std::max(V, 0.0);
		if(Sum <= 0.0)
			P.assign(K, 1.0 / static_cast<double>(K));
		else
			for(double &V : P)
				V = std::max(V, 0.0) / Sum;
	}
	double TotalVisits = 0.0;
	for(std::size_t T = 0; T < It; ++T) {
		const std::size_t A = MctsUctPickFromReal(Q, N, P, std::max(TotalVisits, 1.0), Exploration);
		N[A] += 1.0;
		Q[A] += Rewards[A];
		TotalVisits += 1.0;
	}
	std::size_t Best = 0;
	double BestMean = -std::numeric_limits<double>::infinity();
	for(std::size_t I = 0; I < K; ++I) {
		const double Mean = N[I] > 0.0 ? Q[I] / N[I] : Rewards[I];
		if(Mean > BestMean) {
			BestMean = Mean;
			Best = I;
		}
	}
	return Best;
}

std::pair<double, double> BayesBetaPostFromReal(double Alpha0, double Beta0, double Successes, double Failures) {
	const double A = std::max(Alpha0, 0.0) + std::max(Successes, 0.0);
	const double B = std::max(Beta0, 0.0) + std::max(Failures, 0.0);
	return {A, B};
}

std::pair<double, double> BayesNormalPostFromReal(double Mu0, double Tau0, double SampleMean, double SampleCount,
                                                  double Sigma) {
	const double N = std::max(SampleCount, 0.0);
	const double Sig = std::max(Sigma, 1e-12);
	const double TauN = std::max(Tau0, 0.0) + N / (Sig * Sig);
	if(TauN <= 0.0)
		return {Mu0, Tau0};
	const double MuN = (std::max(Tau0, 0.0) * Mu0 + (N / (Sig * Sig)) * SampleMean) / TauN;
	return {MuN, TauN};
}

std::vector<double> BayesGridPostFromReal(const std::vector<double> &LogPrior, const std::vector<double> &LogLike) {
	const std::size_t N = std::min(LogPrior.size(), LogLike.size());
	if(N == 0 || N > MaxBayesGrid)
		return {};
	std::vector<double> LogPost(N);
	double MaxLog = -std::numeric_limits<double>::infinity();
	for(std::size_t I = 0; I < N; ++I) {
		LogPost[I] = LogPrior[I] + LogLike[I];
		MaxLog = std::max(MaxLog, LogPost[I]);
	}
	double Sum = 0.0;
	std::vector<double> Post(N);
	for(std::size_t I = 0; I < N; ++I) {
		Post[I] = std::exp(LogPost[I] - MaxLog);
		Sum += Post[I];
	}
	if(Sum <= 0.0)
		return Post;
	for(double &V : Post)
		V /= Sum;
	return Post;
}

double BayesLogEvidenceFromReal(const std::vector<double> &LogPrior, const std::vector<double> &LogLike) {
	const auto Post = BayesGridPostFromReal(LogPrior, LogLike);
	if(Post.empty())
		return std::numeric_limits<double>::quiet_NaN();
	const std::size_t N = Post.size();
	double MaxLog = -std::numeric_limits<double>::infinity();
	for(std::size_t I = 0; I < N; ++I) {
		const double L = LogPrior[I] + LogLike[I];
		MaxLog = std::max(MaxLog, L);
	}
	double Sum = 0.0;
	for(std::size_t I = 0; I < N; ++I)
		Sum += std::exp(LogPrior[I] + LogLike[I] - MaxLog);
	return MaxLog + std::log(std::max(Sum, 1e-300));
}

std::optional<std::string> MctsSearchCellFromReal(const std::string &Rewards, const std::string &Priors,
                                                  const std::string &Iterations, const std::string &Exploration) {
	const auto R = ParseRealList(Rewards);
	const auto P = ParseRealList(Priors);
	const auto It = ToSize(Iterations);
	const auto C = ToNum(Exploration);
	if(!R || !It || !C)
		return std::nullopt;
	return FmtNum(static_cast<double>(MctsSearchFromReal(*R, P ? *P : std::vector<double>{}, *It, *C)));
}

std::optional<std::string> MctsUctPickCellFromReal(const std::string &Q, const std::string &Visits,
                                                     const std::string &Priors, const std::string &ParentVisits,
                                                     const std::string &Exploration) {
	const auto Qv = ParseRealList(Q);
	const auto Nv = ParseRealList(Visits);
	const auto Pv = ParseRealList(Priors);
	const auto Pn = ToNum(ParentVisits);
	const auto C = ToNum(Exploration);
	if(!Qv || !Nv || !Pn || !C)
		return std::nullopt;
	return FmtNum(static_cast<double>(MctsUctPickFromReal(*Qv, *Nv, Pv ? *Pv : std::vector<double>{}, *Pn, *C)));
}

std::optional<std::string> BayesBetaPostCellFromReal(const std::string &Alpha0, const std::string &Beta0,
                                                       const std::string &Successes, const std::string &Failures) {
	const auto A0 = ToNum(Alpha0);
	const auto B0 = ToNum(Beta0);
	const auto S = ToNum(Successes);
	const auto F = ToNum(Failures);
	if(!A0 || !B0 || !S || !F)
		return std::nullopt;
	const auto Post = BayesBetaPostFromReal(*A0, *B0, *S, *F);
	return MathSciSimdUtil::FormatListCellFromDoubles({Post.first, Post.second});
}

std::optional<std::string> BayesNormalPostCellFromReal(const std::string &Mu0, const std::string &Tau0,
                                                         const std::string &SampleMean, const std::string &SampleCount,
                                                         const std::string &Sigma) {
	const auto M0 = ToNum(Mu0);
	const auto T0 = ToNum(Tau0);
	const auto Xm = ToNum(SampleMean);
	const auto N = ToNum(SampleCount);
	const auto Sig = ToNum(Sigma);
	if(!M0 || !T0 || !Xm || !N || !Sig)
		return std::nullopt;
	const auto Post = BayesNormalPostFromReal(*M0, *T0, *Xm, *N, *Sig);
	return MathSciSimdUtil::FormatListCellFromDoubles({Post.first, Post.second});
}

std::optional<std::string> BayesGridPostCellFromReal(const std::string &LogPrior, const std::string &LogLike) {
	const auto Lp = ParseRealList(LogPrior);
	const auto Ll = ParseRealList(LogLike);
	if(!Lp || !Ll)
		return std::nullopt;
	const auto Post = BayesGridPostFromReal(*Lp, *Ll);
	if(Post.empty())
		return std::nullopt;
	return MathSciSimdUtil::FormatListCellFromDoubles(Post);
}

std::optional<std::string> BayesLogEvidenceCellFromReal(const std::string &LogPrior, const std::string &LogLike) {
	const auto Lp = ParseRealList(LogPrior);
	const auto Ll = ParseRealList(LogLike);
	if(!Lp || !Ll)
		return std::nullopt;
	return FmtNum(BayesLogEvidenceFromReal(*Lp, *Ll));
}

} // namespace MathSciInference
} // namespace AstralDB
