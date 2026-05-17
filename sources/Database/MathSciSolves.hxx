#pragma once

#include <cstddef>
#include <vector>

namespace AstralDB {
namespace MathSciSolves {

constexpr size_t MaxSolveLen = 1u << 20;

/** One explicit Euler step: y + dt·slope (scalar). */
double OdeEulerScalar(double Y0, double Dt, double Slope);

/** One explicit Euler step (vector). */
std::vector<float> OdeEulerVecF32(const float *Y, const float *Slope, size_t N, float Dt);

/** Classical RK4 step with caller-supplied stage slopes (same length as \p Y). */
std::vector<float> OdeRk4VecF32(const float *Y, const float *K1, const float *K2, const float *K3, const float *K4,
                               size_t N, float Dt);

/** Euler–Maruyama: y + drift·dt + diffusion·√dt·z. */
std::vector<float> SdeEulerVecF32(const float *Y, const float *Drift, const float *Diffusion, const float *NoiseZ,
                                  size_t N, float Dt);

/** One-step geometric Brownian motion (scalar): y·exp((μ − ½σ²)dt + σ√dt·z). */
double SdeGbmScalar(double Y, double Mu, double Sigma, double Dt, double Z);

/** Ornstein–Uhlenbeck exact transition (scalar). */
double SdeOuScalar(double X, double Mu, double Theta, double Sigma, double Dt, double Z);

/** 1-D heat equation explicit FTCS step (zero Dirichlet at edges). */
std::vector<float> PdeHeat1dStepF32(const float *U, size_t N, float Alpha, float Dt, float Dx);

/** 1-D Poisson −u'' = f, one Jacobi relaxation with weight \p Omega. */
std::vector<float> PdePoisson1dJacobiF32(const float *U, const float *F, size_t N, float Omega);

std::vector<double> OdeEulerFromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &Slope);
std::vector<double> OdeRk4FromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &K1,
                                     const std::vector<double> &K2, const std::vector<double> &K3,
                                     const std::vector<double> &K4);
std::vector<double> SdeEulerFromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &Drift,
                                     const std::vector<double> &Diffusion, const std::vector<double> &Z);
std::vector<double> PdeHeat1dFromReal(const std::vector<double> &U, double Alpha, double Dt, double Dx);
std::vector<double> PdePoisson1dFromReal(const std::vector<double> &U, const std::vector<double> &F, double Omega);

} // namespace MathSciSolves
} // namespace AstralDB
