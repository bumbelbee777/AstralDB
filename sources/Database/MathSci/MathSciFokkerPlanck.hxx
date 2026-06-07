#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {
namespace MathSciFokkerPlanck {

constexpr std::size_t MaxNfpGrid = 512;
constexpr std::size_t MaxNfpMarchSteps = 256;

/**
 * One macro Fokker–Planck step (HANK-style): density g on wealth grid with
 * mean-field capital feedback, borrowing barrier, and neural drift correction.
 */
std::vector<double> NfpMacroStepFromReal(const std::vector<double> &G, const std::vector<double> &Grid,
                                           const std::vector<double> &Drift, const std::vector<double> &Diffusion,
                                           const std::vector<double> &NeuralCorr, double AggK, double KStar,
                                           double BarrierA, double Dt);

std::vector<double> NfpMacroMarchFromReal(const std::vector<double> &G, const std::vector<double> &Grid,
                                          const std::vector<double> &Drift, const std::vector<double> &Diffusion,
                                          const std::vector<double> &NeuralCorr, double AggK, double KStar,
                                          double BarrierA, double Dt, std::size_t Steps);

/** Mass, mean wealth, Gini proxy on grid. */
std::vector<double> NfpMacroMomentsFromReal(const std::vector<double> &G, const std::vector<double> &Grid);

std::optional<std::string> NfpMacroStepCellFromReal(const std::string &G, const std::string &Grid,
                                                      const std::string &Drift, const std::string &Diffusion,
                                                      const std::string &NeuralCorr, const std::string &AggK,
                                                      const std::string &KStar, const std::string &BarrierA,
                                                      const std::string &Dt);
std::optional<std::string> NfpMacroMarchCellFromReal(const std::string &G, const std::string &Grid,
                                                       const std::string &Drift, const std::string &Diffusion,
                                                       const std::string &NeuralCorr, const std::string &AggK,
                                                       const std::string &KStar, const std::string &BarrierA,
                                                       const std::string &Dt, const std::string &Steps);
std::optional<std::string> NfpMacroMomentsCellFromReal(const std::string &G, const std::string &Grid);

} // namespace MathSciFokkerPlanck
} // namespace AstralDB
