#pragma once

#include <DS/HashTable.hxx>
#include <string>
#include <cstdint>

namespace AstralDB {
namespace DS {
static constexpr int MAX_SLIDING_WINDOW_SIZE = 64 * 1024; // 64 kB

inline std::string LZ4Compress(const std::string &Data) {
	if(Data.empty()) return "";
	constexpr int MinMatch = 4;
	std::string Output;
	HashTable<uint32_t, int> Hashes;
	int I = 0;
	while(I < static_cast<int>(Data.size())) {
		int BestLen = 0, BestOffset = 0;
		if(I + MinMatch <= static_cast<int>(Data.size())) {
			uint32_t Hash = 0;
			for(int k = 0; k < MinMatch; ++k)
				Hash = (Hash * 257) + static_cast<uint8_t>(Data[I + k]);
			auto Match = Hashes.Get(Hash);
			if(Match && I - *Match <= MAX_SLIDING_WINDOW_SIZE) {
				int J = *Match;
				int Len = 0;
				while(I + Len < static_cast<int>(Data.size()) && Data[J + Len] == Data[I + Len] && Len < 255) ++Len;
				if(Len >= MinMatch) {
					BestLen = Len;
					BestOffset = I - J;
				}
			}
			Hashes.Insert(Hash, I);
		}
		if(BestLen >= MinMatch) {
			Output.push_back(0); // Marker for match
			Output.push_back(static_cast<char>(BestLen));
			Output.push_back(static_cast<char>(BestOffset >> 8));
			Output.push_back(static_cast<char>(BestOffset & 0xFF));
			I += BestLen;
		} else {
			Output.push_back(Data[I]);
			++I;
		}
	}
	return Output;
}

inline std::string LZ4Decompress(const std::string &Data) {
	if(Data.empty()) return "";
	std::string Output;
	for(size_t I = 0; I < Data.size();) {
		if(Data[I] == 0 && I + 3 < Data.size()) {
			int Len = static_cast<uint8_t>(Data[I + 1]);
			int Offset = (static_cast<uint8_t>(Data[I + 2]) << 8) | static_cast<uint8_t>(Data[I + 3]);
			if(Offset > 0 && Output.size() >= static_cast<size_t>(Offset)) {
				size_t Start = Output.size() - Offset;
				for(int k = 0; k < Len; ++k)
					Output.push_back(Output[Start + k]);
				I += 4;
				continue;
			}
		}
		Output.push_back(Data[I]);
		++I;
	}
	return Output;
}
}
}