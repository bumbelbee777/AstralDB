#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {
namespace MathSciSolves {

constexpr size_t MaxSolveLen = 1u << 20;
/** Max explicit ODE steps per \c DEQ_INTEGRATE call (keeps wall time in the tens of ms). */
constexpr size_t MaxOdeIntegrateSteps = 8192;

enum class OdeMethodTag : std::uint8_t {
	Euler,
	Heun,
	Midpoint,
	Rk3,
	Rk4,
	Implicit,
	Trapezoid,
	SemiImplicit,
	CrankNicolson,
	AdamsBashforth2,
	Unknown,
};

OdeMethodTag ParseOdeMethod(std::string_view Method);

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

/** Heun (RK2): y + (dt/2)(k1 + k2). */
std::vector<float> OdeHeunVecF32(const float *Y, const float *K1, const float *K2, size_t N, float Dt);

/** Explicit midpoint: y + dt·k_mid. */
std::vector<float> OdeMidpointVecF32(const float *Y, const float *KMid, size_t N, float Dt);

/** Implicit Euler for y' = λ y (diagonal Jacobian): y / (1 − dt·λ) per component. */
std::vector<float> OdeImplicitEulerVecF32(const float *Y, const float *Lambda, size_t N, float Dt);

/** Explicit trapezoid: y + (dt/2)(k0 + k1). */
std::vector<float> OdeTrapezoidVecF32(const float *Y, const float *K0, const float *K1, size_t N, float Dt);

/** Staged semi-implicit: y* = y + dt·k1, y* + dt·k2. */
std::vector<float> OdeSemiImplicitVecF32(const float *Y, const float *K1, const float *K2, size_t N, float Dt);

/** Diagonal Crank–Nicolson for y' = λ⊙y with slope k: (y + (dt/2)·k) / (1 − (dt/2)·λ). */
std::vector<float> OdeCrankNicolsonVecF32(const float *Y, const float *Lambda, const float *K, size_t N, float Dt);

/** Scalar Milstein for additive noise SDE with constant diffusion σ. */
double SdeMilsteinScalar(double Y, double Mu, double Sigma, double Dt, double Z);

/** 1-D upwind advection u_t + c u_x = 0. */
std::vector<float> PdeAdvection1dStepF32(const float *U, size_t N, float C, float Dt, float Dx);

/** 1-D wave equation leapfrog step (u_prev, u_curr → u_next). */
std::vector<float> PdeWave1dStepF32(const float *UPrev, const float *UCurr, size_t N, float C, float Dt, float Dx);

/** Dispatch \p Method : EULER, HEUN, MIDPOINT, RK3, RK4, IMPLICIT, TRAPEZOID, SEMI_IMPLICIT, CRANK_NICOLSON, AB2. */
std::vector<double> OdeSolveFromReal(std::string_view Method, const std::vector<double> &Y, double Dt,
                                     const std::vector<double> &A, const std::vector<double> &B,
                                     const std::vector<double> &C, const std::vector<double> &D);

std::vector<double> OdeEulerFromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &Slope);
std::vector<double> OdeRk4FromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &K1,
                                     const std::vector<double> &K2, const std::vector<double> &K3,
                                     const std::vector<double> &K4);
std::vector<double> SdeEulerFromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &Drift,
                                     const std::vector<double> &Diffusion, const std::vector<double> &Z);
std::vector<double> PdeHeat1dFromReal(const std::vector<double> &U, double Alpha, double Dt, double Dx);
std::vector<double> PdePoisson1dFromReal(const std::vector<double> &U, const std::vector<double> &F, double Omega);
std::vector<double> OdeHeunFromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &K1,
                                    const std::vector<double> &K2);
std::vector<double> OdeMidpointFromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &KMid);
std::vector<double> OdeImplicitEulerFromReal(const std::vector<double> &Y, double Dt,
                                             const std::vector<double> &Lambda);
std::vector<double> OdeTrapezoidFromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &K0,
                                        const std::vector<double> &K1);
std::vector<double> OdeSemiImplicitFromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &K1,
                                            const std::vector<double> &K2);
std::vector<double> OdeCrankNicolsonFromReal(const std::vector<double> &Y, double Dt,
                                             const std::vector<double> &Lambda, const std::vector<double> &K);
std::vector<double> PdeAdvection1dFromReal(const std::vector<double> &U, double C, double Dt, double Dx);
std::vector<double> PdeWave1dFromReal(const std::vector<double> &UPrev, const std::vector<double> &UCurr, double C,
                                      double Dt, double Dx);

/** Classical RK3 (SSP) with caller-supplied stage slopes. */
std::vector<float> OdeRk3VecF32(const float *Y, const float *K1, const float *K2, const float *K3, size_t N, float Dt);

/** Adams–Bashforth order 2: \p Y + dt·(1.5·k_curr − 0.5·k_prev). */
std::vector<float> OdeAdamsBashforth2VecF32(const float *Y, const float *KCurr, const float *KPrev, size_t N, float Dt);

/** One Jacobi step for dense \f$Ax=b\f$ (square \p N×\p N matrix, row-major). */
std::vector<float> LinearJacobiStepF32(const float *X, const float *A, const float *B, size_t N);

/** One Gauss–Seidel step for dense \f$Ax=b\f$. */
std::vector<float> LinearGaussSeidelStepF32(const float *X, const float *A, const float *B, size_t N);

/** One SOR step with relaxation \p Omega. */
std::vector<float> LinearSorStepF32(const float *X, const float *A, const float *B, size_t N, float Omega);

/** Richardson: \p X + α·(b − Ax). */
std::vector<float> LinearRichardsonStepF32(const float *X, const float *A, const float *B, size_t N, float Alpha);

/** Conjugate gradient for SPD \f$Ax=b\f$ (returns approximate solution). */
std::vector<float> LinearCgSolveF32(const float *X0, const float *A, const float *B, size_t N, size_t MaxIters,
                                    float Tol);

/** 1-D Poisson: one Gauss–Seidel relaxation. */
std::vector<float> PdePoisson1dGaussSeidelF32(const float *U, const float *F, size_t N);

/** 1-D Poisson: one SOR relaxation with weight \p Omega. */
std::vector<float> PdePoisson1dSorF32(const float *U, const float *F, size_t N, float Omega);

/** Scalar Newton step: \p X − f(x)/f′(x). */
double RootNewtonStepScalar(double X, double Fx, double Dfx);

/** Scalar secant step. */
double RootSecantStepScalar(double X0, double X1, double F0, double F1);

/** Scalar bisection midpoint (requires \p F0·\p F1 ≤ 0). */
double RootBisectStepScalar(double Lo, double Hi, double Flo, double Fhi);

/** Scalar Halley step. */
double RootHalleyStepScalar(double X, double Fx, double Dfx, double D2fx);

std::vector<double> OdeRk3FromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &K1,
                                   const std::vector<double> &K2, const std::vector<double> &K3);
std::vector<double> OdeAdamsBashforth2FromReal(const std::vector<double> &Y, double Dt,
                                               const std::vector<double> &KCurr, const std::vector<double> &KPrev);
std::vector<double> OdeMarchFromReal(std::string_view Method, const std::vector<double> &Y, double Dt,
                                     const std::vector<double> &A, const std::vector<double> &B,
                                     const std::vector<double> &C, const std::vector<double> &D, size_t Steps);
std::vector<double> SdeMarchFromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &Drift,
                                     const std::vector<double> &Diffusion, const std::vector<double> &Z, size_t Steps);

std::vector<double> LinearJacobiStepFromMat(const std::string &Mat, const std::vector<double> &X,
                                            const std::vector<double> &B);
std::vector<double> LinearGaussSeidelStepFromMat(const std::string &Mat, const std::vector<double> &X,
                                                 const std::vector<double> &B);
std::vector<double> LinearSorStepFromMat(const std::string &Mat, const std::vector<double> &X,
                                         const std::vector<double> &B, double Omega);
std::vector<double> LinearRichardsonStepFromMat(const std::string &Mat, const std::vector<double> &X,
                                                const std::vector<double> &B, double Alpha);
std::vector<double> LinearCgSolveFromMat(const std::string &Mat, const std::vector<double> &X0,
                                         const std::vector<double> &B, size_t MaxIters, double Tol);
std::vector<double> LinearSolveFromReal(std::string_view Method, const std::string &Mat, const std::vector<double> &X,
                                        const std::vector<double> &B, size_t MaxIters, double Tol, double Param);

std::vector<double> PdePoissonGsStepFromReal(const std::vector<double> &U, const std::vector<double> &F);
std::vector<double> PdePoissonSorStepFromReal(const std::vector<double> &U, const std::vector<double> &F, double Omega);
std::vector<double> PdePoissonSolveFromReal(const std::vector<double> &U, const std::vector<double> &F, double Omega,
                                            size_t MaxIters, double Tol, std::string_view Method);
std::vector<double> PdeHeatMarchFromReal(const std::vector<double> &U, double Alpha, double Dt, double Dx,
                                         size_t Steps);
std::vector<double> PdeSolveFromReal(std::string_view Method, const std::vector<double> &U, const std::vector<double> &F,
                                     double Omega, size_t MaxIters, double Tol, double Alpha, double Dt, double Dx,
                                     size_t Steps);

double RootNewtonStepFromReal(double X, double Fx, double Dfx);
double RootSecantStepFromReal(double X0, double X1, double F0, double F1);
double RootBisectStepFromReal(double Lo, double Hi, double Flo, double Fhi);
double RootHalleyStepFromReal(double X, double Fx, double Dfx, double D2fx);
double RootSolveStepFromReal(std::string_view Method, double A, double B, double C, double D);

/** Fused explicit march (float path, no per-step heap). \p K2–\p K4 may be empty for low-order methods. */
std::vector<double> DeqIntegrateFromReal(std::string_view Method, const std::vector<double> &Y, double Dt,
                                          size_t Steps, const std::vector<double> &K1, const std::vector<double> &K2,
                                          const std::vector<double> &K3, const std::vector<double> &K4);

/** Adaptive RK4 pair (step-doubling) with constant slopes; advances from \p T0 toward \p T1. */
std::vector<double> DeqAdaptFromReal(std::string_view Method, const std::vector<double> &Y, double T0, double T1,
                                     double HInit, double Rtol, double Atol, const std::vector<double> &K1,
                                     const std::vector<double> &K2, const std::vector<double> &K3,
                                     const std::vector<double> &K4);

/** Uniform time grid \p T0..\p T1 with \p N points (inclusive). */
std::optional<std::vector<double>> DeqLinspaceFromReal(double T0, double T1, size_t N);

std::optional<std::string> DeqIntegrateCellFromReal(const std::string &Method, const std::string &Y, const std::string &Dt,
                                                  const std::string &Steps, const std::string &K1,
                                                  const std::string &K2, const std::string &K3, const std::string &K4);
std::optional<std::string> DeqAdaptCellFromReal(const std::string &Method, const std::string &Y, const std::string &T0,
                                                const std::string &T1, const std::string &HInit,
                                                const std::string &Rtol, const std::string &Atol,
                                                const std::string &K1, const std::string &K2, const std::string &K3,
                                                const std::string &K4);
std::optional<std::string> DeqLinspaceCellFromReal(const std::string &T0, const std::string &T1, const std::string &N);

} // namespace MathSciSolves
} // namespace AstralDB
