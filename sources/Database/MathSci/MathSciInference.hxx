#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {
namespace MathSciInference {

constexpr std::size_t MaxMctsActions = 64;
constexpr std::size_t MaxMctsIterations = 4096;
constexpr std::size_t MaxBayesGrid = 4096;

/** UCT action index from visit stats (toy bandit / shallow tree). */
std::size_t MctsUctPickFromReal(const std::vector<double> &Q, const std::vector<double> &Visits,
                                const std::vector<double> &Priors, double ParentVisits, double Exploration);

/** Flat MCTS over fixed action set; returns best action index. */
std::size_t MctsSearchFromReal(const std::vector<double> &Rewards, const std::vector<double> &Priors,
                               std::size_t Iterations, double Exploration);

/** Beta–Binomial conjugate update (alpha, beta). */
std::pair<double, double> BayesBetaPostFromReal(double Alpha0, double Beta0, double Successes, double Failures);

/** Normal–Normal conjugate (known σ); returns (mu_post, tau_post). */
std::pair<double, double> BayesNormalPostFromReal(double Mu0, double Tau0, double SampleMean, double SampleCount,
                                                  double Sigma);

/** Normalized discrete posterior from log prior and log likelihood. */
std::vector<double> BayesGridPostFromReal(const std::vector<double> &LogPrior, const std::vector<double> &LogLike);

/** Log marginal ∑ exp(log prior + log like) with log-sum-exp. */
double BayesLogEvidenceFromReal(const std::vector<double> &LogPrior, const std::vector<double> &LogLike);

std::optional<std::string> MctsSearchCellFromReal(const std::string &Rewards, const std::string &Priors,
                                                  const std::string &Iterations, const std::string &Exploration);
std::optional<std::string> MctsUctPickCellFromReal(const std::string &Q, const std::string &Visits,
                                                     const std::string &Priors, const std::string &ParentVisits,
                                                     const std::string &Exploration);
std::optional<std::string> BayesBetaPostCellFromReal(const std::string &Alpha0, const std::string &Beta0,
                                                       const std::string &Successes, const std::string &Failures);
std::optional<std::string> BayesNormalPostCellFromReal(const std::string &Mu0, const std::string &Tau0,
                                                         const std::string &SampleMean, const std::string &SampleCount,
                                                         const std::string &Sigma);
std::optional<std::string> BayesGridPostCellFromReal(const std::string &LogPrior, const std::string &LogLike);
std::optional<std::string> BayesLogEvidenceCellFromReal(const std::string &LogPrior, const std::string &LogLike);

} // namespace MathSciInference
} // namespace AstralDB
