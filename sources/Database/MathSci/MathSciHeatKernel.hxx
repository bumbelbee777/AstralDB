#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {
namespace MathSciHeatKernel {

constexpr std::size_t MaxGraphNodes = 4096;
constexpr std::size_t MaxGraphEdges = 65536;
constexpr std::size_t MaxSignalLen = 1u << 20;

/** One explicit heat-diffusion step: u' = u − dt·L·u (combinatorial graph Laplacian). */
std::vector<double> GraphStepFromReal(const std::vector<double> &U, const std::vector<double> &Edges, double Dt);

/** Repeat {@link GraphStepFromReal} for {@p Steps} iterations. */
std::vector<double> GraphMarchFromReal(const std::vector<double> &U, const std::vector<double> &Edges, double Dt,
                                       std::size_t Steps);

/** Apply heat flow for diffusion time {@p Tau} (internal sub-stepping). */
std::vector<double> GraphApplyFromReal(const std::vector<double> &U, const std::vector<double> &Edges, double Tau);

/** 1-D Gaussian heat kernel via {@c Conv1dSame}. */
std::vector<double> Heat1dFromReal(const std::vector<double> &Signal, double Sigma);

/** Separable 2-D Gaussian blur on flattened row-major {@p W}×{@p H} grid. */
std::vector<double> Heat2dFromReal(const std::vector<double> &Grid, std::size_t W, std::size_t H, double Sigma);

/** Gradient of 1-D Gaussian-smoothed signal. */
std::vector<double> Grad1dFromReal(const std::vector<double> &Signal, double Sigma);

std::optional<std::string> GraphStepCellFromReal(const std::string &U, const std::string &Edges, const std::string &Dt);
std::optional<std::string> GraphMarchCellFromReal(const std::string &U, const std::string &Edges, const std::string &Dt,
                                                  const std::string &Steps);
std::optional<std::string> GraphApplyCellFromReal(const std::string &U, const std::string &Edges,
                                                    const std::string &Tau);
std::optional<std::string> Heat1dCellFromReal(const std::string &Signal, const std::string &Sigma);
std::optional<std::string> Heat2dCellFromReal(const std::string &Img, const std::string &Sigma);
std::optional<std::string> Grad1dCellFromReal(const std::string &Signal, const std::string &Sigma);

} // namespace MathSciHeatKernel
} // namespace AstralDB
