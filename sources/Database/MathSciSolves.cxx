#include <Database/MathSciSolves.hxx>

#include <IO/SIMD.hxx>

#include <cmath>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {
namespace MathSciSolves {

double OdeEulerScalar(double Y0, double Dt, double Slope) { return Y0 + Dt * Slope; }

std::vector<float> OdeEulerVecF32(const float *Y, const float *Slope, size_t N, float Dt) {
	std::vector<float> Out(N);
	std::vector<float> Scaled(N);
	Simd::ScaleF32(Scaled.data(), Slope, Dt, N);
	Simd::AddF32(Out.data(), Y, Scaled.data(), N);
	return Out;
}

std::vector<float> OdeRk4VecF32(const float *Y, const float *K1, const float *K2, const float *K3, const float *K4,
                               size_t N, float Dt) {
	std::vector<float> Out(N);
	const float H = Dt;
	for(size_t I = 0; I < N; ++I) {
		const float Yv = Y[I];
		const float Ksum = K1[I] + 2.f * K2[I] + 2.f * K3[I] + K4[I];
		Out[I] = Yv + (H / 6.f) * Ksum;
	}
	return Out;
}

std::vector<float> SdeEulerVecF32(const float *Y, const float *Drift, const float *Diffusion, const float *NoiseZ,
                                  size_t N, float Dt) {
	const float SqrtDt = std::sqrt(Dt);
	std::vector<float> Out(N);
	for(size_t I = 0; I < N; ++I)
		Out[I] = Y[I] + Drift[I] * Dt + Diffusion[I] * SqrtDt * NoiseZ[I];
	return Out;
}

double SdeGbmScalar(double Y, double Mu, double Sigma, double Dt, double Z) {
	const float SqrtDt = std::sqrt(static_cast<float>(Dt));
	const float Expo = static_cast<float>((Mu - 0.5 * Sigma * Sigma) * Dt + Sigma * SqrtDt * Z);
	return Y * static_cast<double>(std::exp(Expo));
}

double SdeOuScalar(double X, double Mu, double Theta, double Sigma, double Dt, double Z) {
	if(Theta <= 0.0)
		return X;
	const double Decay = std::exp(-Theta * Dt);
	const double Var =
	    (Sigma * Sigma) * (1.0 - std::exp(-2.0 * Theta * Dt)) / (2.0 * Theta);
	return Mu + (X - Mu) * Decay + std::sqrt(Var) * Z;
}

std::vector<float> PdeHeat1dStepF32(const float *U, size_t N, float Alpha, float Dt, float Dx) {
	if(N < 3)
		return {};
	const float R = Alpha * Dt / (Dx * Dx);
	if(R > 0.5f)
		return std::vector<float>(U, U + N);
	std::vector<float> Out(N);
	Out[0] = U[0];
	Out[N - 1] = U[N - 1];
	for(size_t I = 1; I + 1 < N; ++I)
		Out[I] = U[I] + R * (U[I - 1] - 2.f * U[I] + U[I + 1]);
	return Out;
}

std::vector<float> PdePoisson1dJacobiF32(const float *U, const float *F, size_t N, float Omega) {
	if(N < 3)
		return {};
	std::vector<float> Out(N);
	Out[0] = U[0];
	Out[N - 1] = U[N - 1];
	const float Dx2 = 1.f;
	for(size_t I = 1; I + 1 < N; ++I) {
		const float NewU = 0.5f * (U[I - 1] + U[I + 1] + F[I] * Dx2);
		Out[I] = (1.f - Omega) * U[I] + Omega * NewU;
	}
	return Out;
}

namespace {

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

std::vector<double> OdeEulerFromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &Slope) {
	if(Y.size() != Slope.size() || Y.empty() || Y.size() > MaxSolveLen)
		return {};
	return ToF64(OdeEulerVecF32(ToF32(Y).data(), ToF32(Slope).data(), Y.size(), static_cast<float>(Dt)));
}

std::vector<double> OdeRk4FromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &K1,
                                   const std::vector<double> &K2, const std::vector<double> &K3,
                                   const std::vector<double> &K4) {
	if(Y.empty() || Y.size() != K1.size() || Y.size() != K2.size() || Y.size() != K3.size() || Y.size() != K4.size() ||
	   Y.size() > MaxSolveLen)
		return {};
	return ToF64(OdeRk4VecF32(ToF32(Y).data(), ToF32(K1).data(), ToF32(K2).data(), ToF32(K3).data(), ToF32(K4).data(),
	                          Y.size(), static_cast<float>(Dt)));
}

std::vector<double> SdeEulerFromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &Drift,
                                     const std::vector<double> &Diffusion, const std::vector<double> &Z) {
	if(Y.size() != Drift.size() || Y.size() != Diffusion.size() || Y.size() != Z.size() || Y.empty() ||
	   Y.size() > MaxSolveLen)
		return {};
	return ToF64(SdeEulerVecF32(ToF32(Y).data(), ToF32(Drift).data(), ToF32(Diffusion).data(), ToF32(Z).data(),
	                           Y.size(), static_cast<float>(Dt)));
}

std::vector<float> OdeHeunVecF32(const float *Y, const float *K1, const float *K2, size_t N, float Dt) {
	std::vector<float> Out(N);
	const float H = Dt * 0.5f;
	for(size_t I = 0; I < N; ++I)
		Out[I] = Y[I] + H * (K1[I] + K2[I]);
	return Out;
}

std::vector<float> OdeMidpointVecF32(const float *Y, const float *KMid, size_t N, float Dt) {
	std::vector<float> Out(N);
	for(size_t I = 0; I < N; ++I)
		Out[I] = Y[I] + Dt * KMid[I];
	return Out;
}

std::vector<float> OdeImplicitEulerVecF32(const float *Y, const float *Lambda, size_t N, float Dt) {
	std::vector<float> Out(N);
	for(size_t I = 0; I < N; ++I) {
		const float Den = 1.f - Dt * Lambda[I];
		Out[I] = (std::fabs(Den) < 1e-12f) ? Y[I] : Y[I] / Den;
	}
	return Out;
}

double SdeMilsteinScalar(double Y, double Mu, double Sigma, double Dt, double Z) {
	const float SqrtDt = std::sqrt(static_cast<float>(Dt));
	const float Expo = static_cast<float>((Mu - 0.5 * Sigma * Sigma) * Dt + Sigma * SqrtDt * Z +
	                                      0.5 * Sigma * Sigma * (Z * Z - 1.0) * Dt);
	return Y * static_cast<double>(std::exp(Expo));
}

std::vector<float> PdeAdvection1dStepF32(const float *U, size_t N, float C, float CflDt, float Dx) {
	if(N < 2)
		return {};
	const float Courant = C * CflDt / Dx;
	std::vector<float> Out(N);
	Out[0] = U[0];
	for(size_t I = 1; I < N; ++I) {
		if(Courant >= 0.f)
			Out[I] = U[I] - Courant * (U[I] - U[I - 1]);
		else
			Out[I] = U[I] - Courant * (U[I + 1 < N ? I + 1 : I] - U[I]);
	}
	return Out;
}

std::vector<float> PdeWave1dStepF32(const float *UPrev, const float *UCurr, size_t N, float C, float Dt, float Dx) {
	if(N < 3)
		return {};
	const float R2 = (C * Dt / Dx) * (C * Dt / Dx);
	std::vector<float> Out(N);
	Out[0] = UCurr[0];
	Out[N - 1] = UCurr[N - 1];
	for(size_t I = 1; I + 1 < N; ++I)
		Out[I] = 2.f * UCurr[I] - UPrev[I] + R2 * (UCurr[I - 1] - 2.f * UCurr[I] + UCurr[I + 1]);
	return Out;
}

std::vector<double> OdeSolveFromReal(std::string_view Method, const std::vector<double> &Y, double Dt,
                                     const std::vector<double> &A, const std::vector<double> &B,
                                     const std::vector<double> &C, const std::vector<double> &D) {
	std::string M(Method);
	for(char &Ch : M)
		Ch = static_cast<char>(std::toupper(static_cast<unsigned char>(Ch)));
	if(M == "EULER")
		return OdeEulerFromReal(Y, Dt, A);
	if(M == "HEUN" || M == "RK2")
		return OdeHeunFromReal(Y, Dt, A, B);
	if(M == "MIDPOINT")
		return OdeMidpointFromReal(Y, Dt, A);
	if(M == "RK4")
		return OdeRk4FromReal(Y, Dt, A, B, C, D);
	if(M == "IMPLICIT" || M == "IMPLICIT_EULER")
		return OdeImplicitEulerFromReal(Y, Dt, A);
	return {};
}

std::vector<double> PdeHeat1dFromReal(const std::vector<double> &U, double Alpha, double Dt, double Dx) {
	if(U.empty() || U.size() > MaxSolveLen)
		return {};
	return ToF64(PdeHeat1dStepF32(ToF32(U).data(), U.size(), static_cast<float>(Alpha), static_cast<float>(Dt),
	                              static_cast<float>(Dx)));
}

std::vector<double> PdePoisson1dFromReal(const std::vector<double> &U, const std::vector<double> &F, double Omega) {
	if(U.size() != F.size() || U.empty() || U.size() > MaxSolveLen)
		return {};
	return ToF64(PdePoisson1dJacobiF32(ToF32(U).data(), ToF32(F).data(), U.size(), static_cast<float>(Omega)));
}

std::vector<double> OdeHeunFromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &K1,
                                    const std::vector<double> &K2) {
	if(Y.size() != K1.size() || Y.size() != K2.size() || Y.empty() || Y.size() > MaxSolveLen)
		return {};
	return ToF64(OdeHeunVecF32(ToF32(Y).data(), ToF32(K1).data(), ToF32(K2).data(), Y.size(), static_cast<float>(Dt)));
}

std::vector<double> OdeMidpointFromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &KMid) {
	if(Y.size() != KMid.size() || Y.empty() || Y.size() > MaxSolveLen)
		return {};
	return ToF64(OdeMidpointVecF32(ToF32(Y).data(), ToF32(KMid).data(), Y.size(), static_cast<float>(Dt)));
}

std::vector<double> OdeImplicitEulerFromReal(const std::vector<double> &Y, double Dt,
                                             const std::vector<double> &Lambda) {
	if(Y.size() != Lambda.size() || Y.empty() || Y.size() > MaxSolveLen)
		return {};
	return ToF64(
	    OdeImplicitEulerVecF32(ToF32(Y).data(), ToF32(Lambda).data(), Y.size(), static_cast<float>(Dt)));
}

std::vector<double> PdeAdvection1dFromReal(const std::vector<double> &U, double C, double Dt, double Dx) {
	if(U.empty() || U.size() > MaxSolveLen)
		return {};
	return ToF64(PdeAdvection1dStepF32(ToF32(U).data(), U.size(), static_cast<float>(C), static_cast<float>(Dt),
	                                   static_cast<float>(Dx)));
}

std::vector<double> PdeWave1dFromReal(const std::vector<double> &UPrev, const std::vector<double> &UCurr, double C,
                                      double Dt, double Dx) {
	if(UPrev.size() != UCurr.size() || UPrev.empty() || UPrev.size() > MaxSolveLen)
		return {};
	return ToF64(PdeWave1dStepF32(ToF32(UPrev).data(), ToF32(UCurr).data(), UPrev.size(), static_cast<float>(C),
	                              static_cast<float>(Dt), static_cast<float>(Dx)));
}

} // namespace MathSciSolves
} // namespace AstralDB
