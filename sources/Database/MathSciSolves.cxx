#include <Database/MathSciSolves.hxx>

#include <Database/AdvancedTypes.hxx>
#include <Database/MathSciSimdUtil.hxx>
#include <IO/SIMD.hxx>

#include <cmath>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {
namespace MathSciSolves {
namespace {

float MatAt(const float *A, size_t N, size_t I, size_t J) { return A[I * N + J]; }

float ResidualInfNorm(const float *A, const float *B, const float *X, size_t N) {
	float Max = 0.f;
	for(size_t I = 0; I < N; ++I) {
		float Sum = B[I];
		for(size_t J = 0; J < N; ++J)
			Sum -= MatAt(A, N, I, J) * X[J];
		Max = std::max(Max, std::fabs(Sum));
	}
	return Max;
}

std::optional<std::pair<size_t, std::vector<float>>> ParseSquareMatF32(std::string_view MatCell) {
	const auto Mp = AdvancedTypes::DecodeMatrixCell(MatCell);
	if(!Mp || Mp->Rows != Mp->Cols || Mp->Rows == 0 || Mp->Flat.size() > MaxSolveLen)
		return std::nullopt;
	const size_t N = Mp->Rows;
	std::vector<float> Af(MathSciSimdUtil::SeqToF32(Mp->Flat));
	return std::pair{N, std::move(Af)};
}

bool LinArgsOk(size_t N, const std::vector<double> &X, const std::vector<double> &B) {
	return N > 0 && X.size() == N && B.size() == N && N <= MaxSolveLen;
}

} // namespace

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

std::vector<double> OdeEulerFromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &Slope) {
	if(Y.size() != Slope.size() || Y.empty() || Y.size() > MaxSolveLen)
		return {};
	return MathSciSimdUtil::ToF64(OdeEulerVecF32(MathSciSimdUtil::SeqToF32(Y).data(), MathSciSimdUtil::SeqToF32(Slope).data(), Y.size(), static_cast<float>(Dt)));
}

std::vector<double> OdeRk4FromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &K1,
                                   const std::vector<double> &K2, const std::vector<double> &K3,
                                   const std::vector<double> &K4) {
	if(Y.empty() || Y.size() != K1.size() || Y.size() != K2.size() || Y.size() != K3.size() || Y.size() != K4.size() ||
	   Y.size() > MaxSolveLen)
		return {};
	return MathSciSimdUtil::ToF64(OdeRk4VecF32(MathSciSimdUtil::SeqToF32(Y).data(), MathSciSimdUtil::SeqToF32(K1).data(), MathSciSimdUtil::SeqToF32(K2).data(), MathSciSimdUtil::SeqToF32(K3).data(), MathSciSimdUtil::SeqToF32(K4).data(),
	                          Y.size(), static_cast<float>(Dt)));
}

std::vector<double> SdeEulerFromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &Drift,
                                     const std::vector<double> &Diffusion, const std::vector<double> &Z) {
	if(Y.size() != Drift.size() || Y.size() != Diffusion.size() || Y.size() != Z.size() || Y.empty() ||
	   Y.size() > MaxSolveLen)
		return {};
	return MathSciSimdUtil::ToF64(SdeEulerVecF32(MathSciSimdUtil::SeqToF32(Y).data(), MathSciSimdUtil::SeqToF32(Drift).data(), MathSciSimdUtil::SeqToF32(Diffusion).data(), MathSciSimdUtil::SeqToF32(Z).data(),
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

std::vector<float> OdeTrapezoidVecF32(const float *Y, const float *K0, const float *K1, size_t N, float Dt) {
	return OdeHeunVecF32(Y, K0, K1, N, Dt);
}

std::vector<float> OdeSemiImplicitVecF32(const float *Y, const float *K1, const float *K2, size_t N, float Dt) {
	std::vector<float> Out(N);
	for(size_t I = 0; I < N; ++I) {
		const float YStar = Y[I] + Dt * K1[I];
		Out[I] = YStar + Dt * K2[I];
	}
	return Out;
}

std::vector<float> OdeCrankNicolsonVecF32(const float *Y, const float *Lambda, const float *K, size_t N, float Dt) {
	std::vector<float> Out(N);
	const float Half = Dt * 0.5f;
	for(size_t I = 0; I < N; ++I) {
		const float Num = Y[I] + Half * K[I];
		const float Den = 1.f - Half * Lambda[I];
		Out[I] = (std::fabs(Den) < 1e-12f) ? Y[I] : Num / Den;
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
	if(M == "TRAPEZOID")
		return OdeTrapezoidFromReal(Y, Dt, A, B);
	if(M == "SEMI_IMPLICIT" || M == "SEMI")
		return OdeSemiImplicitFromReal(Y, Dt, A, B);
	if(M == "CRANK_NICOLSON" || M == "CN")
		return OdeCrankNicolsonFromReal(Y, Dt, A, B);
	if(M == "RK3")
		return OdeRk3FromReal(Y, Dt, A, B, C);
	if(M == "ADAMS_BASHFORTH2" || M == "AB2")
		return OdeAdamsBashforth2FromReal(Y, Dt, A, B);
	return {};
}

std::vector<double> PdeHeat1dFromReal(const std::vector<double> &U, double Alpha, double Dt, double Dx) {
	if(U.empty() || U.size() > MaxSolveLen)
		return {};
	return MathSciSimdUtil::ToF64(PdeHeat1dStepF32(MathSciSimdUtil::SeqToF32(U).data(), U.size(), static_cast<float>(Alpha), static_cast<float>(Dt),
	                              static_cast<float>(Dx)));
}

std::vector<double> PdePoisson1dFromReal(const std::vector<double> &U, const std::vector<double> &F, double Omega) {
	if(U.size() != F.size() || U.empty() || U.size() > MaxSolveLen)
		return {};
	return MathSciSimdUtil::ToF64(PdePoisson1dJacobiF32(MathSciSimdUtil::SeqToF32(U).data(), MathSciSimdUtil::SeqToF32(F).data(), U.size(), static_cast<float>(Omega)));
}

std::vector<double> OdeHeunFromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &K1,
                                    const std::vector<double> &K2) {
	if(Y.size() != K1.size() || Y.size() != K2.size() || Y.empty() || Y.size() > MaxSolveLen)
		return {};
	return MathSciSimdUtil::ToF64(OdeHeunVecF32(MathSciSimdUtil::SeqToF32(Y).data(), MathSciSimdUtil::SeqToF32(K1).data(), MathSciSimdUtil::SeqToF32(K2).data(), Y.size(), static_cast<float>(Dt)));
}

std::vector<double> OdeMidpointFromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &KMid) {
	if(Y.size() != KMid.size() || Y.empty() || Y.size() > MaxSolveLen)
		return {};
	return MathSciSimdUtil::ToF64(OdeMidpointVecF32(MathSciSimdUtil::SeqToF32(Y).data(), MathSciSimdUtil::SeqToF32(KMid).data(), Y.size(), static_cast<float>(Dt)));
}

std::vector<double> OdeImplicitEulerFromReal(const std::vector<double> &Y, double Dt,
                                             const std::vector<double> &Lambda) {
	if(Y.size() != Lambda.size() || Y.empty() || Y.size() > MaxSolveLen)
		return {};
	return MathSciSimdUtil::ToF64(
	    OdeImplicitEulerVecF32(MathSciSimdUtil::SeqToF32(Y).data(), MathSciSimdUtil::SeqToF32(Lambda).data(), Y.size(), static_cast<float>(Dt)));
}

std::vector<double> OdeTrapezoidFromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &K0,
                                        const std::vector<double> &K1) {
	if(Y.size() != K0.size() || Y.size() != K1.size() || Y.empty() || Y.size() > MaxSolveLen)
		return {};
	return MathSciSimdUtil::ToF64(OdeTrapezoidVecF32(MathSciSimdUtil::SeqToF32(Y).data(), MathSciSimdUtil::SeqToF32(K0).data(), MathSciSimdUtil::SeqToF32(K1).data(), Y.size(),
	                                static_cast<float>(Dt)));
}

std::vector<double> OdeSemiImplicitFromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &K1,
                                            const std::vector<double> &K2) {
	if(Y.size() != K1.size() || Y.size() != K2.size() || Y.empty() || Y.size() > MaxSolveLen)
		return {};
	return MathSciSimdUtil::ToF64(OdeSemiImplicitVecF32(MathSciSimdUtil::SeqToF32(Y).data(), MathSciSimdUtil::SeqToF32(K1).data(), MathSciSimdUtil::SeqToF32(K2).data(), Y.size(),
	                                   static_cast<float>(Dt)));
}

std::vector<double> OdeCrankNicolsonFromReal(const std::vector<double> &Y, double Dt,
                                             const std::vector<double> &Lambda, const std::vector<double> &K) {
	if(Y.size() != Lambda.size() || Y.size() != K.size() || Y.empty() || Y.size() > MaxSolveLen)
		return {};
	return MathSciSimdUtil::ToF64(OdeCrankNicolsonVecF32(MathSciSimdUtil::SeqToF32(Y).data(), MathSciSimdUtil::SeqToF32(Lambda).data(), MathSciSimdUtil::SeqToF32(K).data(), Y.size(),
	                                    static_cast<float>(Dt)));
}

std::vector<double> PdeAdvection1dFromReal(const std::vector<double> &U, double C, double Dt, double Dx) {
	if(U.empty() || U.size() > MaxSolveLen)
		return {};
	return MathSciSimdUtil::ToF64(PdeAdvection1dStepF32(MathSciSimdUtil::SeqToF32(U).data(), U.size(), static_cast<float>(C), static_cast<float>(Dt),
	                                   static_cast<float>(Dx)));
}

std::vector<double> PdeWave1dFromReal(const std::vector<double> &UPrev, const std::vector<double> &UCurr, double C,
                                      double Dt, double Dx) {
	if(UPrev.size() != UCurr.size() || UPrev.empty() || UPrev.size() > MaxSolveLen)
		return {};
	return MathSciSimdUtil::ToF64(PdeWave1dStepF32(MathSciSimdUtil::SeqToF32(UPrev).data(), MathSciSimdUtil::SeqToF32(UCurr).data(), UPrev.size(), static_cast<float>(C),
	                              static_cast<float>(Dt), static_cast<float>(Dx)));
}

std::vector<float> OdeRk3VecF32(const float *Y, const float *K1, const float *K2, const float *K3, size_t N,
                                float Dt) {
	std::vector<float> Out(N);
	const float H = Dt;
	for(size_t I = 0; I < N; ++I)
		Out[I] = Y[I] + (H / 6.f) * (K1[I] + 4.f * K2[I] + K3[I]);
	return Out;
}

std::vector<float> OdeAdamsBashforth2VecF32(const float *Y, const float *KCurr, const float *KPrev, size_t N,
                                            float Dt) {
	std::vector<float> Out(N);
	for(size_t I = 0; I < N; ++I)
		Out[I] = Y[I] + Dt * (1.5f * KCurr[I] - 0.5f * KPrev[I]);
	return Out;
}

std::vector<float> LinearJacobiStepF32(const float *X, const float *A, const float *B, size_t N) {
	std::vector<float> Out(N);
	for(size_t I = 0; I < N; ++I) {
		float Sum = B[I];
		for(size_t J = 0; J < N; ++J) {
			if(J != I)
				Sum -= MatAt(A, N, I, J) * X[J];
		}
		const float Diag = MatAt(A, N, I, I);
		Out[I] = (std::fabs(Diag) < 1e-12f) ? X[I] : Sum / Diag;
	}
	return Out;
}

std::vector<float> LinearGaussSeidelStepF32(const float *X, const float *A, const float *B, size_t N) {
	std::vector<float> Out(X, X + N);
	for(size_t I = 0; I < N; ++I) {
		float Sum = B[I];
		for(size_t J = 0; J < N; ++J) {
			if(J != I)
				Sum -= MatAt(A, N, I, J) * Out[J];
		}
		const float Diag = MatAt(A, N, I, I);
		if(std::fabs(Diag) >= 1e-12f)
			Out[I] = Sum / Diag;
	}
	return Out;
}

std::vector<float> LinearSorStepF32(const float *X, const float *A, const float *B, size_t N, float Omega) {
	std::vector<float> Out(X, X + N);
	for(size_t I = 0; I < N; ++I) {
		float Sum = B[I];
		for(size_t J = 0; J < N; ++J) {
			if(J != I)
				Sum -= MatAt(A, N, I, J) * Out[J];
		}
		const float Diag = MatAt(A, N, I, I);
		if(std::fabs(Diag) < 1e-12f)
			continue;
		const float Gs = Sum / Diag;
		Out[I] = (1.f - Omega) * Out[I] + Omega * Gs;
	}
	return Out;
}

std::vector<float> LinearRichardsonStepF32(const float *X, const float *A, const float *B, size_t N, float Alpha) {
	std::vector<float> Out(N);
	for(size_t I = 0; I < N; ++I) {
		float Ax = 0.f;
		for(size_t J = 0; J < N; ++J)
			Ax += MatAt(A, N, I, J) * X[J];
		Out[I] = X[I] + Alpha * (B[I] - Ax);
	}
	return Out;
}

std::vector<float> LinearCgSolveF32(const float *X0, const float *A, const float *B, size_t N, size_t MaxIters,
                                    float Tol) {
	std::vector<float> X(X0, X0 + N);
	std::vector<float> R(N);
	std::vector<float> P(N);
	std::vector<float> Ap(N);
	for(size_t I = 0; I < N; ++I) {
		float Ax = 0.f;
		for(size_t J = 0; J < N; ++J)
			Ax += MatAt(A, N, I, J) * X[J];
		R[I] = B[I] - Ax;
		P[I] = R[I];
	}
	float RsOld = 0.f;
	for(size_t I = 0; I < N; ++I)
		RsOld += R[I] * R[I];
	if(RsOld <= Tol * Tol)
		return X;
	const size_t ItCap = MaxIters == 0 ? N * 8 : MaxIters;
	for(size_t It = 0; It < ItCap; ++It) {
		for(size_t I = 0; I < N; ++I) {
			float Sum = 0.f;
			for(size_t J = 0; J < N; ++J)
				Sum += MatAt(A, N, I, J) * P[J];
			Ap[I] = Sum;
		}
		float PAp = 0.f;
		for(size_t I = 0; I < N; ++I)
			PAp += P[I] * Ap[I];
		if(std::fabs(PAp) < 1e-20f)
			break;
		const float Alpha = RsOld / PAp;
		for(size_t I = 0; I < N; ++I) {
			X[I] += Alpha * P[I];
			R[I] -= Alpha * Ap[I];
		}
		float RsNew = 0.f;
		for(size_t I = 0; I < N; ++I)
			RsNew += R[I] * R[I];
		if(std::sqrt(RsNew) <= Tol)
			break;
		const float Beta = RsNew / RsOld;
		for(size_t I = 0; I < N; ++I)
			P[I] = R[I] + Beta * P[I];
		RsOld = RsNew;
	}
	return X;
}

std::vector<float> PdePoisson1dGaussSeidelF32(const float *U, const float *F, size_t N) {
	if(N < 3)
		return {};
	std::vector<float> Out(U, U + N);
	const float Dx2 = 1.f;
	for(size_t I = 1; I + 1 < N; ++I) {
		const float NewU = 0.5f * (Out[I - 1] + Out[I + 1] + F[I] * Dx2);
		Out[I] = NewU;
	}
	return Out;
}

std::vector<float> PdePoisson1dSorF32(const float *U, const float *F, size_t N, float Omega) {
	if(N < 3)
		return {};
	std::vector<float> Out(U, U + N);
	const float Dx2 = 1.f;
	for(size_t I = 1; I + 1 < N; ++I) {
		const float Gs = 0.5f * (Out[I - 1] + Out[I + 1] + F[I] * Dx2);
		Out[I] = (1.f - Omega) * Out[I] + Omega * Gs;
	}
	return Out;
}

double RootNewtonStepScalar(double X, double Fx, double Dfx) {
	if(std::fabs(Dfx) < 1e-15)
		return X;
	return X - Fx / Dfx;
}

double RootSecantStepScalar(double X0, double X1, double F0, double F1) {
	const double Den = F1 - F0;
	if(std::fabs(Den) < 1e-15)
		return X1;
	return X1 - F1 * (X1 - X0) / Den;
}

double RootBisectStepScalar(double Lo, double Hi, double Flo, double Fhi) {
	if(Flo * Fhi > 0.0)
		return 0.5 * (Lo + Hi);
	return 0.5 * (Lo + Hi);
}

double RootHalleyStepScalar(double X, double Fx, double Dfx, double D2fx) {
	const double Den = Dfx * Dfx - 0.5 * Fx * D2fx;
	if(std::fabs(Den) < 1e-15)
		return X;
	return X - Fx * Dfx / Den;
}

std::vector<double> OdeRk3FromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &K1,
                                   const std::vector<double> &K2, const std::vector<double> &K3) {
	if(Y.empty() || Y.size() != K1.size() || Y.size() != K2.size() || Y.size() != K3.size() || Y.size() > MaxSolveLen)
		return {};
	return MathSciSimdUtil::ToF64(OdeRk3VecF32(MathSciSimdUtil::SeqToF32(Y).data(), MathSciSimdUtil::SeqToF32(K1).data(),
	                          MathSciSimdUtil::SeqToF32(K2).data(), MathSciSimdUtil::SeqToF32(K3).data(), Y.size(),
	                          static_cast<float>(Dt)));
}

std::vector<double> OdeAdamsBashforth2FromReal(const std::vector<double> &Y, double Dt,
                                               const std::vector<double> &KCurr, const std::vector<double> &KPrev) {
	if(Y.size() != KCurr.size() || Y.size() != KPrev.size() || Y.empty() || Y.size() > MaxSolveLen)
		return {};
	return MathSciSimdUtil::ToF64(OdeAdamsBashforth2VecF32(MathSciSimdUtil::SeqToF32(Y).data(),
	                                      MathSciSimdUtil::SeqToF32(KCurr).data(), MathSciSimdUtil::SeqToF32(KPrev).data(),
	                                      Y.size(), static_cast<float>(Dt)));
}

std::vector<double> OdeMarchFromReal(std::string_view Method, const std::vector<double> &Y, double Dt,
                                     const std::vector<double> &A, const std::vector<double> &B,
                                     const std::vector<double> &C, const std::vector<double> &D, size_t Steps) {
	if(Y.empty() || Steps == 0 || Steps > MaxSolveLen)
		return {};
	std::vector<double> Cur = Y;
	for(size_t S = 0; S < Steps; ++S) {
		Cur = OdeSolveFromReal(Method, Cur, Dt, A, B, C, D);
		if(Cur.empty())
			return {};
	}
	return Cur;
}

std::vector<double> SdeMarchFromReal(const std::vector<double> &Y, double Dt, const std::vector<double> &Drift,
                                     const std::vector<double> &Diffusion, const std::vector<double> &Z, size_t Steps) {
	if(Y.empty() || Steps == 0 || Steps > MaxSolveLen)
		return {};
	std::vector<double> Cur = Y;
	for(size_t S = 0; S < Steps; ++S) {
		Cur = SdeEulerFromReal(Cur, Dt, Drift, Diffusion, Z);
		if(Cur.empty())
			return {};
	}
	return Cur;
}

std::vector<double> LinearJacobiStepFromMat(const std::string &Mat, const std::vector<double> &X,
                                            const std::vector<double> &B) {
	const auto Mp = ParseSquareMatF32(Mat);
	if(!Mp || !LinArgsOk(Mp->first, X, B))
		return {};
	return MathSciSimdUtil::ToF64(
	    LinearJacobiStepF32(MathSciSimdUtil::SeqToF32(X).data(), Mp->second.data(), MathSciSimdUtil::SeqToF32(B).data(),
	                        Mp->first));
}

std::vector<double> LinearGaussSeidelStepFromMat(const std::string &Mat, const std::vector<double> &X,
                                                 const std::vector<double> &B) {
	const auto Mp = ParseSquareMatF32(Mat);
	if(!Mp || !LinArgsOk(Mp->first, X, B))
		return {};
	return MathSciSimdUtil::ToF64(LinearGaussSeidelStepF32(MathSciSimdUtil::SeqToF32(X).data(), Mp->second.data(),
	                                     MathSciSimdUtil::SeqToF32(B).data(), Mp->first));
}

std::vector<double> LinearSorStepFromMat(const std::string &Mat, const std::vector<double> &X,
                                         const std::vector<double> &B, double Omega) {
	const auto Mp = ParseSquareMatF32(Mat);
	if(!Mp || !LinArgsOk(Mp->first, X, B))
		return {};
	return MathSciSimdUtil::ToF64(LinearSorStepF32(MathSciSimdUtil::SeqToF32(X).data(), Mp->second.data(),
	                                MathSciSimdUtil::SeqToF32(B).data(), Mp->first, static_cast<float>(Omega)));
}

std::vector<double> LinearRichardsonStepFromMat(const std::string &Mat, const std::vector<double> &X,
                                                const std::vector<double> &B, double Alpha) {
	const auto Mp = ParseSquareMatF32(Mat);
	if(!Mp || !LinArgsOk(Mp->first, X, B))
		return {};
	return MathSciSimdUtil::ToF64(LinearRichardsonStepF32(MathSciSimdUtil::SeqToF32(X).data(), Mp->second.data(),
	                                      MathSciSimdUtil::SeqToF32(B).data(), Mp->first, static_cast<float>(Alpha)));
}

std::vector<double> LinearCgSolveFromMat(const std::string &Mat, const std::vector<double> &X0,
                                         const std::vector<double> &B, size_t MaxIters, double Tol) {
	const auto Mp = ParseSquareMatF32(Mat);
	if(!Mp || !LinArgsOk(Mp->first, X0, B))
		return {};
	const float UseTol = static_cast<float>(Tol <= 0.0 ? 1e-6 : Tol);
	return MathSciSimdUtil::ToF64(LinearCgSolveF32(MathSciSimdUtil::SeqToF32(X0).data(), Mp->second.data(),
	                              MathSciSimdUtil::SeqToF32(B).data(), Mp->first, MaxIters, UseTol));
}

std::vector<double> LinearSolveFromReal(std::string_view Method, const std::string &Mat, const std::vector<double> &X,
                                        const std::vector<double> &B, size_t MaxIters, double Tol, double Param) {
	const auto Mp = ParseSquareMatF32(Mat);
	if(!Mp || !LinArgsOk(Mp->first, X, B))
		return {};
	std::string M(Method);
	for(char &Ch : M)
		Ch = static_cast<char>(std::toupper(static_cast<unsigned char>(Ch)));
	const size_t N = Mp->first;
	const float *A = Mp->second.data();
	std::vector<float> Cur(MathSciSimdUtil::SeqToF32(X));
	const std::vector<float> Bf = MathSciSimdUtil::SeqToF32(B);
	const float UseTol = static_cast<float>(Tol <= 0.0 ? 1e-6 : Tol);
	const size_t ItCap = MaxIters == 0 ? N * 64 : MaxIters;
	if(M == "CG" || M == "CONJUGATE_GRADIENT")
		return MathSciSimdUtil::ToF64(LinearCgSolveF32(Cur.data(), A, Bf.data(), N, ItCap, UseTol));
	for(size_t It = 0; It < ItCap; ++It) {
		std::vector<float> Next;
		if(M == "JACOBI")
			Next = LinearJacobiStepF32(Cur.data(), A, Bf.data(), N);
		else if(M == "GAUSS_SEIDEL" || M == "GS")
			Next = LinearGaussSeidelStepF32(Cur.data(), A, Bf.data(), N);
		else if(M == "SOR")
			Next = LinearSorStepF32(Cur.data(), A, Bf.data(), N, static_cast<float>(Param <= 0.0 ? 1.2 : Param));
		else if(M == "RICHARDSON")
			Next = LinearRichardsonStepF32(Cur.data(), A, Bf.data(), N, static_cast<float>(Param <= 0.0 ? 0.1 : Param));
		else
			return {};
		Cur = std::move(Next);
		if(ResidualInfNorm(A, Bf.data(), Cur.data(), N) <= UseTol)
			break;
	}
	return MathSciSimdUtil::ToF64(Cur);
}

std::vector<double> PdePoissonGsStepFromReal(const std::vector<double> &U, const std::vector<double> &F) {
	if(U.size() != F.size() || U.empty() || U.size() > MaxSolveLen)
		return {};
	return MathSciSimdUtil::ToF64(PdePoisson1dGaussSeidelF32(MathSciSimdUtil::SeqToF32(U).data(),
	                                      MathSciSimdUtil::SeqToF32(F).data(), U.size()));
}

std::vector<double> PdePoissonSorStepFromReal(const std::vector<double> &U, const std::vector<double> &F,
                                              double Omega) {
	if(U.size() != F.size() || U.empty() || U.size() > MaxSolveLen)
		return {};
	return MathSciSimdUtil::ToF64(
	    PdePoisson1dSorF32(MathSciSimdUtil::SeqToF32(U).data(), MathSciSimdUtil::SeqToF32(F).data(), U.size(),
	                       static_cast<float>(Omega)));
}

std::vector<double> PdePoissonSolveFromReal(const std::vector<double> &U, const std::vector<double> &F, double Omega,
                                            size_t MaxIters, double Tol, std::string_view Method) {
	if(U.size() != F.size() || U.empty() || U.size() > MaxSolveLen)
		return {};
	std::string M(Method);
	for(char &Ch : M)
		Ch = static_cast<char>(std::toupper(static_cast<unsigned char>(Ch)));
	std::vector<float> Cur(MathSciSimdUtil::SeqToF32(U));
	const std::vector<float> Ff = MathSciSimdUtil::SeqToF32(F);
	const size_t N = U.size();
	const size_t ItCap = MaxIters == 0 ? N * 256 : MaxIters;
	const float UseTol = static_cast<float>(Tol <= 0.0 ? 1e-5 : Tol);
	for(size_t It = 0; It < ItCap; ++It) {
		std::vector<float> Prev = Cur;
		if(M == "SOR")
			Cur = PdePoisson1dSorF32(Cur.data(), Ff.data(), N, static_cast<float>(Omega <= 0.0 ? 1.0 : Omega));
		else if(M == "GAUSS_SEIDEL" || M == "GS")
			Cur = PdePoisson1dGaussSeidelF32(Cur.data(), Ff.data(), N);
		else
			Cur = PdePoisson1dJacobiF32(Cur.data(), Ff.data(), N, static_cast<float>(Omega <= 0.0 ? 1.0 : Omega));
		float MaxDelta = 0.f;
		for(size_t I = 1; I + 1 < N; ++I)
			MaxDelta = std::max(MaxDelta, std::fabs(Cur[I] - Prev[I]));
		if(MaxDelta <= UseTol)
			break;
	}
	return MathSciSimdUtil::ToF64(Cur);
}

std::vector<double> PdeHeatMarchFromReal(const std::vector<double> &U, double Alpha, double Dt, double Dx,
                                         size_t Steps) {
	if(U.empty() || Steps == 0 || Steps > MaxSolveLen)
		return {};
	std::vector<double> Cur = U;
	for(size_t S = 0; S < Steps; ++S) {
		Cur = PdeHeat1dFromReal(Cur, Alpha, Dt, Dx);
		if(Cur.empty())
			return {};
	}
	return Cur;
}

std::vector<double> PdeSolveFromReal(std::string_view Method, const std::vector<double> &U, const std::vector<double> &F,
                                     double Omega, size_t MaxIters, double Tol, double Alpha, double Dt, double Dx,
                                     size_t Steps) {
	std::string M(Method);
	for(char &Ch : M)
		Ch = static_cast<char>(std::toupper(static_cast<unsigned char>(Ch)));
	if(M == "POISSON" || M == "POISSON_JACOBI" || M == "JACOBI")
		return PdePoissonSolveFromReal(U, F, Omega, MaxIters, Tol, "JACOBI");
	if(M == "POISSON_GS" || M == "GAUSS_SEIDEL")
		return PdePoissonSolveFromReal(U, F, Omega, MaxIters, Tol, "GS");
	if(M == "POISSON_SOR" || M == "SOR")
		return PdePoissonSolveFromReal(U, F, Omega, MaxIters, Tol, "SOR");
	if(M == "HEAT_MARCH" || M == "HEAT")
		return PdeHeatMarchFromReal(U, Alpha, Dt, Dx, Steps);
	return {};
}

double RootNewtonStepFromReal(double X, double Fx, double Dfx) { return RootNewtonStepScalar(X, Fx, Dfx); }

double RootSecantStepFromReal(double X0, double X1, double F0, double F1) {
	return RootSecantStepScalar(X0, X1, F0, F1);
}

double RootBisectStepFromReal(double Lo, double Hi, double Flo, double Fhi) {
	return RootBisectStepScalar(Lo, Hi, Flo, Fhi);
}

double RootHalleyStepFromReal(double X, double Fx, double Dfx, double D2fx) {
	return RootHalleyStepScalar(X, Fx, Dfx, D2fx);
}

double RootSolveStepFromReal(std::string_view Method, double A, double B, double C, double D) {
	std::string M(Method);
	for(char &Ch : M)
		Ch = static_cast<char>(std::toupper(static_cast<unsigned char>(Ch)));
	if(M == "NEWTON" || M == "NEWTON_STEP")
		return RootNewtonStepScalar(A, B, C);
	if(M == "SECANT" || M == "SECANT_STEP")
		return RootSecantStepScalar(A, B, C, D);
	if(M == "BISECT" || M == "BISECTION")
		return RootBisectStepScalar(A, B, C, D);
	if(M == "HALLEY" || M == "HALLEY_STEP")
		return RootHalleyStepScalar(A, B, C, D);
	return A;
}

} // namespace MathSciSolves
} // namespace AstralDB
