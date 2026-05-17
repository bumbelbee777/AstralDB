#include <Database/MathSciSignal.hxx>

#include <IO/SIMD.hxx>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

namespace AstralDB {
namespace MathSciSignal {
namespace {

constexpr size_t MaxTransformLen = 1u << 20;

size_t NextPow2(size_t N) {
	if(N <= 1)
		return 1;
	size_t P = 1;
	while(P < N)
		P <<= 1;
	return P;
}

bool IsPow2(size_t N) { return N > 0 && (N & (N - 1)) == 0; }

void BitReversePermute(float *Re, float *Im, size_t N) {
	for(size_t I = 0, J = 0; I < N; ++I) {
		if(I < J) {
			std::swap(Re[I], Re[J]);
			std::swap(Im[I], Im[J]);
		}
		size_t M = N >> 1;
		for(; M && J >= M; M >>= 1)
			J -= M;
		J += M;
	}
}

void FftButterflies(float *Re, float *Im, size_t N, bool Inverse) {
	const float Sign = Inverse ? 1.f : -1.f;
	for(size_t Len = 2; Len <= N; Len <<= 1) {
		const size_t Half = Len >> 1;
		const float Angle = Sign * 2.f * static_cast<float>(std::numbers::pi) / static_cast<float>(Len);
		const float Wr = std::cos(Angle);
		const float Wi = std::sin(Angle);
		for(size_t I = 0; I < N; I += Len) {
			float Cr = 1.f;
			float Ci = 0.f;
			for(size_t J = 0; J < Half; ++J) {
				const size_t U = I + J;
				const size_t V = U + Half;
				const float Tr = Re[V] * Cr - Im[V] * Ci;
				const float Ti = Re[V] * Ci + Im[V] * Cr;
				Re[V] = Re[U] - Tr;
				Im[V] = Im[U] - Ti;
				Re[U] += Tr;
				Im[U] += Ti;
				const float NextCr = Cr * Wr - Ci * Wi;
				Ci = Cr * Wi + Ci * Wr;
				Cr = NextCr;
			}
		}
	}
}

std::vector<float> ToF32(const std::vector<double> &In) {
	std::vector<float> Out(In.size());
	for(size_t I = 0; I < In.size(); ++I)
		Out[I] = static_cast<float>(In[I]);
	return Out;
}

std::vector<double> ToF64(const std::vector<float> &In) {
	std::vector<double> Out(In.size());
	for(size_t I = 0; I < In.size(); ++I)
		Out[I] = static_cast<double>(In[I]);
	return Out;
}

} // namespace

void FftInPlaceF32(float *Re, float *Im, size_t N) {
	if(N < 2 || !IsPow2(N) || N > MaxTransformLen)
		return;
	BitReversePermute(Re, Im, N);
	FftButterflies(Re, Im, N, false);
}

void IfftInPlaceF32(float *Re, float *Im, size_t N) {
	if(N < 2 || !IsPow2(N) || N > MaxTransformLen)
		return;
	BitReversePermute(Re, Im, N);
	FftButterflies(Re, Im, N, true);
	const float Inv = 1.f / static_cast<float>(N);
	Simd::ScaleF32(Re, Re, Inv, N);
	Simd::ScaleF32(Im, Im, Inv, N);
}

void Dct2F32(const float *In, float *Out, size_t N) {
	if(N == 0 || N > MaxTransformLen)
		return;
	const float Scale0 = std::sqrt(1.f / static_cast<float>(N));
	const float Scale = std::sqrt(2.f / static_cast<float>(N));
	for(size_t K = 0; K < N; ++K) {
		float Acc = 0.f;
		const float Ck = (K == 0) ? Scale0 : Scale;
		for(size_t I = 0; I < N; ++I) {
			const float Angle = static_cast<float>(std::numbers::pi) / static_cast<float>(N) *
			                    (static_cast<float>(I) + 0.5f) * static_cast<float>(K);
			Acc += In[I] * std::cos(Angle);
		}
		Out[K] = Ck * Acc;
	}
}

void Idct2F32(const float *In, float *Out, size_t N) {
	if(N == 0 || N > MaxTransformLen)
		return;
	const float Scale0 = std::sqrt(1.f / static_cast<float>(N));
	const float Scale = std::sqrt(2.f / static_cast<float>(N));
	for(size_t I = 0; I < N; ++I) {
		float Acc = In[0] * Scale0;
		for(size_t K = 1; K < N; ++K) {
			const float Angle = static_cast<float>(std::numbers::pi) / static_cast<float>(N) *
			                    (static_cast<float>(I) + 0.5f) * static_cast<float>(K);
			Acc += In[K] * Scale * std::cos(Angle);
		}
		Out[I] = Acc;
	}
}

std::vector<float> Conv1dFullF32(const float *A, size_t Na, const float *B, size_t Nb) {
	if(Na == 0 || Nb == 0)
		return {};
	const size_t OutLen = Na + Nb - 1;
	std::vector<float> Out(OutLen, 0.f);
	for(size_t I = 0; I < OutLen; ++I) {
		const size_t J0 = (I >= Nb - 1) ? I - (Nb - 1) : 0;
		const size_t J1 = std::min(I, Na - 1);
		float Acc = 0.f;
		for(size_t Ja = J0; Ja <= J1; ++Ja) {
			const size_t Jb = I - Ja;
			if(Jb < Nb)
				Acc += A[Ja] * B[Jb];
		}
		Out[I] = Acc;
	}
	return Out;
}

std::vector<float> Conv1dSameF32(const float *A, size_t Na, const float *B, size_t Nb) {
	if(Na == 0 || Nb == 0)
		return {};
	const size_t Pad = Nb / 2;
	std::vector<float> Padded(Na + Nb - 1, 0.f);
	for(size_t I = 0; I < Na; ++I)
		Padded[I + Pad] = A[I];
	const auto Full = Conv1dFullF32(Padded.data(), Padded.size(), B, Nb);
	std::vector<float> Out(Na, 0.f);
	for(size_t I = 0; I < Na; ++I)
		Out[I] = Full[I + Pad];
	return Out;
}

std::vector<float> Laplacian1dF32(const float *In, size_t N) {
	if(N == 0)
		return {};
	std::vector<float> Out(N, 0.f);
	for(size_t I = 0; I < N; ++I) {
		const float L = (I > 0) ? In[I - 1] : 0.f;
		const float R = (I + 1 < N) ? In[I + 1] : 0.f;
		Out[I] = L - 2.f * In[I] + R;
	}
	return Out;
}

std::vector<float> AdGradAddF32(const float *Upstream, size_t N) {
	std::vector<float> Out(N);
	std::memcpy(Out.data(), Upstream, N * sizeof(float));
	return Out;
}

std::vector<float> AdGradMulLhsF32(const float *A, const float *B, const float *Upstream, size_t N) {
	std::vector<float> Out(N);
	Simd::MulF32(Out.data(), B, Upstream, N);
	(void)A;
	return Out;
}

std::vector<float> AdGradMulRhsF32(const float *A, const float *B, const float *Upstream, size_t N) {
	std::vector<float> Out(N);
	Simd::MulF32(Out.data(), A, Upstream, N);
	(void)B;
	return Out;
}

std::vector<float> AdGradReluF32(const float *X, const float *Upstream, size_t N) {
	std::vector<float> Out(N);
	for(size_t I = 0; I < N; ++I)
		Out[I] = (X[I] > 0.f) ? Upstream[I] : 0.f;
	return Out;
}

std::vector<float> AdGradSigmoidF32(const float *Y, const float *Upstream, size_t N) {
	std::vector<float> Out(N);
	for(size_t I = 0; I < N; ++I) {
		const float Yi = Y[I];
		Out[I] = Upstream[I] * Yi * (1.f - Yi);
	}
	return Out;
}

std::vector<float> AdGradConv1dInputF32(const float *Input, size_t NIn, const float *Kernel, size_t Nk,
                                        const float *Upstream, size_t NUp) {
	(void)NUp;
	std::vector<float> Out(NIn, 0.f);
	const size_t FullLen = NIn + Nk - 1;
	if(FullLen == 0)
		return Out;
	std::vector<float> KRev(Nk);
	for(size_t I = 0; I < Nk; ++I)
		KRev[I] = Kernel[Nk - 1 - I];
	const auto Part = Conv1dFullF32(Upstream, FullLen, KRev.data(), Nk);
	for(size_t I = 0; I < NIn && I < Part.size(); ++I)
		Out[I] = Part[I];
	(void)Input;
	return Out;
}

std::vector<float> AdGradConv1dKernelF32(const float *Input, size_t NIn, const float *Kernel, size_t Nk,
                                           const float *Upstream, size_t NUp) {
	(void)Kernel;
	(void)NUp;
	std::vector<float> Out(Nk, 0.f);
	const size_t FullLen = NIn + Nk - 1;
	if(FullLen == 0)
		return Out;
	std::vector<float> UpPadded(FullLen, 0.f);
	const size_t Copy = std::min(NUp, FullLen);
	std::memcpy(UpPadded.data(), Upstream, Copy * sizeof(float));
	for(size_t K = 0; K < Nk; ++K) {
		float Acc = 0.f;
		for(size_t I = 0; I < NIn; ++I) {
			const size_t Idx = I + K;
			if(Idx < UpPadded.size())
				Acc += Input[I] * UpPadded[Idx];
		}
		Out[K] = Acc;
	}
	return Out;
}

std::vector<float> AdChainF32(const float *LocalGrad, const float *Upstream, size_t N) {
	std::vector<float> Out(N);
	Simd::MulF32(Out.data(), LocalGrad, Upstream, N);
	return Out;
}

std::vector<float> FftInterleavedReIm(const std::vector<double> &In) {
	if(In.empty() || In.size() > MaxTransformLen)
		return {};
	const size_t N = NextPow2(In.size());
	if(N < 2)
		return {};
	std::vector<float> Re(N, 0.f);
	std::vector<float> Im(N, 0.f);
	for(size_t I = 0; I < In.size(); ++I)
		Re[I] = static_cast<float>(In[I]);
	FftInPlaceF32(Re.data(), Im.data(), N);
	std::vector<float> Out(N * 2);
	for(size_t I = 0; I < N; ++I) {
		Out[I * 2] = Re[I];
		Out[I * 2 + 1] = Im[I];
	}
	return Out;
}

std::vector<double> IfftRealFromInterleaved(const std::vector<double> &Interleaved) {
	if(Interleaved.size() < 4 || (Interleaved.size() % 2) != 0)
		return {};
	const size_t N = Interleaved.size() / 2;
	if(!IsPow2(N) || N > MaxTransformLen)
		return {};
	std::vector<float> Re(N);
	std::vector<float> Im(N);
	for(size_t I = 0; I < N; ++I) {
		Re[I] = static_cast<float>(Interleaved[I * 2]);
		Im[I] = static_cast<float>(Interleaved[I * 2 + 1]);
	}
	IfftInPlaceF32(Re.data(), Im.data(), N);
	return ToF64(Re);
}

std::vector<double> Dct2FromReal(const std::vector<double> &In) {
	if(In.empty() || In.size() > MaxTransformLen)
		return {};
	const auto F = ToF32(In);
	std::vector<float> Out(In.size());
	Dct2F32(F.data(), Out.data(), In.size());
	return ToF64(Out);
}

std::vector<double> Idct2FromReal(const std::vector<double> &In) {
	if(In.empty() || In.size() > MaxTransformLen)
		return {};
	const auto F = ToF32(In);
	std::vector<float> Out(In.size());
	Idct2F32(F.data(), Out.data(), In.size());
	return ToF64(Out);
}

std::vector<double> Conv1dFullFromReal(const std::vector<double> &A, const std::vector<double> &B) {
	if(A.empty() || B.empty())
		return {};
	const auto Af = ToF32(A);
	const auto Bf = ToF32(B);
	return ToF64(Conv1dFullF32(Af.data(), Af.size(), Bf.data(), Bf.size()));
}

std::vector<double> Conv1dSameFromReal(const std::vector<double> &A, const std::vector<double> &B) {
	if(A.empty() || B.empty())
		return {};
	const auto Af = ToF32(A);
	const auto Bf = ToF32(B);
	return ToF64(Conv1dSameF32(Af.data(), Af.size(), Bf.data(), Bf.size()));
}

std::vector<double> Laplacian1dFromReal(const std::vector<double> &In) {
	if(In.empty())
		return {};
	return ToF64(Laplacian1dF32(ToF32(In).data(), In.size()));
}

} // namespace MathSciSignal
} // namespace AstralDB
