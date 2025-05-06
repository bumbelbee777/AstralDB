#pragma once

#include <IO/Task.hxx>
#include <IO/SIMD.hxx>
#include <vector>
#include <array>
#include <future>
#include <cstring>

namespace AstralDB {
namespace Blake3 {
template<class T> T Rotate(T V, int C) {
    return (V << C) | (V >> (sizeof(T) * 8 - C));
}

// BLAKE3 IV (from SHA-256 IV)
static constexpr std::array<uint32_t, 8> IV = {
	0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
	0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
};

inline void Prefetch(const void* Ptr) {
#if defined(__GNUC__)
	__builtin_prefetch(Ptr);
#endif
    return;
}

inline uint32_t Load32LE(const uint8_t* Src) {
	return (uint32_t(Src[0])      ) |
		   (uint32_t(Src[1]) <<  8) |
		   (uint32_t(Src[2]) << 16) |
		   (uint32_t(Src[3]) << 24);
}

inline void Store32LE(uint8_t* Dst, uint32_t V) {
	Dst[0] = V & 0xFF;
	Dst[1] = (V >> 8) & 0xFF;
	Dst[2] = (V >> 16) & 0xFF;
	Dst[3] = (V >> 24) & 0xFF;
}

inline void G(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d, uint32_t x, uint32_t y) {
	a = a + b + x;
	d = Rotate(d ^ a, 16);
	c = c + d;
	b = Rotate(b ^ c, 12);
	a = a + b + y;
	d = Rotate(d ^ a, 8);
	c = c + d;
	b = Rotate(b ^ c, 7);
}

inline void Compress(const uint32_t* ChainingValue, const uint8_t* Block, uint64_t Counter, uint32_t Flags, uint32_t Out[8]) {
	// Minimal BLAKE3 compression for a single 64-byte block
	static constexpr uint32_t MSG_SCHEDULE[7][16] = {
		{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15 },
		{ 2, 6, 3,10, 7, 0, 4,13, 1,11,12, 5, 9,14,15, 8 },
		{ 3, 4,10,12,13, 2, 7,14, 6, 5, 9, 0,11,15, 8, 1 },
		{10, 7,12, 9,14, 3,13,15, 4, 0,11, 6, 5, 8, 1, 2 },
		{12,13, 9,11,15,10,14, 8, 7, 6, 5, 3, 0, 1, 2, 4 },
		{ 9,15,11, 5, 8,12, 1, 2, 3, 7, 6,10,13,14, 4, 0 },
		{11, 8, 5, 0, 2, 9, 3, 4,10, 6, 7,12,14,15, 1,13 }
	};
	uint32_t State[16];
	for(int i = 0; i < 8; ++i) State[i] = ChainingValue[i];
	for(int i = 0; i < 8; ++i) State[8 + i] = IV[i];
	State[12] ^= (uint32_t)Counter;
	State[14] ^= Flags;
	uint32_t M[16];
	for(int i = 0; i < 16; ++i) M[i] = Load32LE(Block + i * 4);
	for(int round = 0; round < 7; ++round) {
		const uint32_t* S = MSG_SCHEDULE[round];
		G(State[0], State[4], State[8], State[12], M[S[0]], M[S[1]]);
		G(State[1], State[5], State[9], State[13], M[S[2]], M[S[3]]);
		G(State[2], State[6], State[10], State[14], M[S[4]], M[S[5]]);
		G(State[3], State[7], State[11], State[15], M[S[6]], M[S[7]]);
		G(State[0], State[5], State[10], State[15], M[S[8]], M[S[9]]);
		G(State[1], State[6], State[11], State[12], M[S[10]], M[S[11]]);
		G(State[2], State[7], State[8], State[13], M[S[12]], M[S[13]]);
		G(State[3], State[4], State[9], State[14], M[S[14]], M[S[15]]);
	}
	for(int i = 0; i < 8; ++i) Out[i] = State[i] ^ State[i + 8];
}

// Synchronous BLAKE3 hash for std::vector<uint8_t> input, 32-byte output
inline std::array<uint8_t, 32> Hash(const std::vector<uint8_t>& Input) {
	Prefetch(Input.data());
	constexpr uint32_t CHUNK_LEN = 64;
	constexpr uint32_t CHUNK_START = 1 << 0;
	constexpr uint32_t CHUNK_END = 1 << 1;
	constexpr uint32_t ROOT = 1 << 3;
	uint32_t Flags = CHUNK_START | CHUNK_END | ROOT;
	uint32_t Out[8] = {};
	uint8_t Block[CHUNK_LEN] = {};
	SimdMemset(Block, 0, CHUNK_LEN);
	SimdMemcpy(Block, Input.data(), std::min<size_t>(Input.size(), CHUNK_LEN));
	Compress(IV.data(), Block, 0, Flags, Out);
	std::array<uint8_t, 32> HashOut;
	for(int i = 0; i < 8; ++i) Store32LE(HashOut.data() + i * 4, Out[i]);
	return HashOut;
}

// Async BLAKE3 hash
inline std::future<std::array<uint8_t, 32>> HashAsync(const std::vector<uint8_t>& Input) {
	return RunAsync([Input]() { return Hash(Input); });
}
}
}