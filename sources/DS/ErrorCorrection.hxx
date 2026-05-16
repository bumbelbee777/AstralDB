#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {
namespace DS {

/** Byte-level single-error correction for WAL payloads: 4 data bytes + 2 GF(2^8) parity symbols per block. */
namespace Detail {

inline constexpr uint16_t kGfPoly = 0x11Du;

inline void GfBuildTables(std::array<uint8_t, 512> &Exp, std::array<uint8_t, 256> &Log) {
	uint16_t X = 1;
	for(int I = 0; I < 255; ++I) {
		Exp[static_cast<size_t>(I)] = static_cast<uint8_t>(X);
		Log[static_cast<size_t>(Exp[static_cast<size_t>(I)])] = static_cast<uint8_t>(I);
		X <<= 1;
		if(X & 0x100)
			X ^= kGfPoly;
	}
	for(int I = 255; I < 512; ++I)
		Exp[static_cast<size_t>(I)] = Exp[static_cast<size_t>(I - 255)];
}

inline std::pair<std::array<uint8_t, 512>, std::array<uint8_t, 256>> GfTables() {
	std::array<uint8_t, 512> Exp{};
	std::array<uint8_t, 256> Log{};
	Log[0] = 0;
	GfBuildTables(Exp, Log);
	return {Exp, Log};
}

inline const std::array<uint8_t, 512> &GfExp() {
	static const auto T = GfTables();
	return T.first;
}

inline const std::array<uint8_t, 256> &GfLog() {
	static const auto T = GfTables();
	return T.second;
}

inline uint8_t GfMul(uint8_t A, uint8_t B) {
	if(A == 0 || B == 0)
		return 0;
	const auto &Exp = GfExp();
	const auto &Log = GfLog();
	const int X = static_cast<int>(Log[static_cast<size_t>(A)]) +
	              static_cast<int>(Log[static_cast<size_t>(B)]);
	return Exp[static_cast<size_t>(X % 255)];
}

inline uint8_t GfInv(uint8_t A) {
	if(A == 0)
		return 0;
	const auto &Exp = GfExp();
	const auto &Log = GfLog();
	return Exp[static_cast<size_t>(255 - Log[static_cast<size_t>(A)])];
}

/** Parity (p4,p5) for data d0..d3 at positions 0..3; H·r=0 with r=(d0,d1,d2,d3,p4,p5). */
inline void EncodeBlock(const uint8_t *D, uint8_t &P0, uint8_t &P1) {
	const uint8_t S = static_cast<uint8_t>(D[0] ^ D[1] ^ D[2] ^ D[3]);
	const uint8_t W = static_cast<uint8_t>(D[0] ^ GfMul(2, D[1]) ^ GfMul(4, D[2]) ^ GfMul(8, D[3]));
	const uint8_t A = 16;
	const uint8_t B = 32;
	const uint8_t Det = static_cast<uint8_t>(A ^ B);
	const uint8_t InvDet = GfInv(Det);
	const uint8_t N0 = static_cast<uint8_t>(W ^ GfMul(B, S));
	P0 = GfMul(N0, InvDet);
	P1 = static_cast<uint8_t>(S ^ P0);
}

/** Correct up to one byte error in R[6]; returns false if uncorrectable. */
inline bool DecodeBlock(uint8_t *R) {
	uint8_t S0 = 0;
	uint8_t S1 = 0;
	const auto &Exp = GfExp();
	for(int I = 0; I < 6; ++I) {
		S0 ^= R[static_cast<size_t>(I)];
		S1 ^= GfMul(R[static_cast<size_t>(I)], Exp[static_cast<size_t>(I)]);
	}
	if(S0 == 0 && S1 == 0)
		return true;
	const uint8_t V = S0;
	if(V == 0)
		return false;
	const uint8_t PosEnc = GfMul(S1, GfInv(V));
	int ErrPos = -1;
	for(int I = 0; I < 6; ++I) {
		if(Exp[static_cast<size_t>(I)] == PosEnc) {
			ErrPos = I;
			break;
		}
	}
	if(ErrPos < 0)
		return false;
	R[static_cast<size_t>(ErrPos)] ^= V;
	S0 = 0;
	S1 = 0;
	for(int I = 0; I < 6; ++I) {
		S0 ^= R[static_cast<size_t>(I)];
		S1 ^= GfMul(R[static_cast<size_t>(I)], Exp[static_cast<size_t>(I)]);
	}
	return S0 == 0 && S1 == 0;
}

} // namespace Detail

struct ErrorCorrection {
	/** Layout: [length BE32][6-byte RS codewords… padded data to multiple of 4]. */
	static std::string Protect(std::string_view Payload) {
		std::string Out;
		const uint32_t Len = static_cast<uint32_t>(Payload.size());
		Out.resize(4 + ((Payload.size() + 3) / 4) * 6);
		Out[0] = static_cast<char>((Len >> 24) & 255);
		Out[1] = static_cast<char>((Len >> 16) & 255);
		Out[2] = static_cast<char>((Len >> 8) & 255);
		Out[3] = static_cast<char>(Len & 255);
		std::vector<uint8_t> Pad(Payload.begin(), Payload.end());
		while(Pad.size() % 4 != 0)
			Pad.push_back(0);
		std::size_t O = 4;
		for(std::size_t I = 0; I < Pad.size(); I += 4) {
			uint8_t D[4] = {Pad[I], Pad[I + 1], Pad[I + 2], Pad[I + 3]};
			uint8_t P0 = 0, P1 = 0;
			Detail::EncodeBlock(D, P0, P1);
			Out[O++] = static_cast<char>(D[0]);
			Out[O++] = static_cast<char>(D[1]);
			Out[O++] = static_cast<char>(D[2]);
			Out[O++] = static_cast<char>(D[3]);
			Out[O++] = static_cast<char>(P0);
			Out[O++] = static_cast<char>(P1);
		}
		return Out;
	}

	static std::optional<std::string> Recover(std::string_view Protected) {
		if(Protected.size() < 4)
			return std::nullopt;
		const auto Len = static_cast<uint32_t>(static_cast<unsigned char>(Protected[0]) << 24 |
		                                       static_cast<unsigned char>(Protected[1]) << 16 |
		                                       static_cast<unsigned char>(Protected[2]) << 8 |
		                                       static_cast<unsigned char>(Protected[3]));
		const std::size_t NeedBlocks = (static_cast<std::size_t>(Len) + 3) / 4;
		if(Protected.size() != 4 + NeedBlocks * 6)
			return std::nullopt;
		std::string Out;
		Out.reserve(Len);
		for(std::size_t B = 0; B < NeedBlocks; ++B) {
			const std::size_t Base = 4 + B * 6;
			uint8_t Block[6];
			for(int J = 0; J < 6; ++J)
				Block[J] = static_cast<uint8_t>(Protected[Base + static_cast<std::size_t>(J)]);
			if(!Detail::DecodeBlock(Block))
				return std::nullopt;
			for(int J = 0; J < 4; ++J) {
				if(Out.size() >= Len)
					break;
				Out.push_back(static_cast<char>(Block[J]));
			}
		}
		if(Out.size() != Len)
			return std::nullopt;
		return Out;
	}
};

} // namespace DS
} // namespace AstralDB
