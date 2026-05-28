#pragma once

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace AstralDB {
namespace MathSciSimdUtil {

inline std::vector<float> SeqToF32(const std::vector<double> &In) {
	std::vector<float> Out(In.size());
	for(std::size_t I = 0; I < In.size(); ++I)
		Out[I] = static_cast<float>(In[I]);
	return Out;
}

inline std::vector<double> ToF64(const std::vector<float> &In) {
	std::vector<double> Out(In.size());
	for(std::size_t I = 0; I < In.size(); ++I)
		Out[I] = static_cast<double>(In[I]);
	return Out;
}

/** Fast \c L[n]:… cell from pre-parsed doubles (avoids string streams on hot paths). */
inline std::string FormatListCellFromDoubles(const std::vector<double> &V) {
	std::string O;
	O.reserve(V.size() * 14 + 16);
	O += "L[";
	O += std::to_string(V.size());
	O += "]:";
	char Buf[40];
	for(std::size_t I = 0; I < V.size(); ++I) {
		if(I)
			O += ',';
		std::snprintf(Buf, sizeof(Buf), "%.8g", V[I]);
		O += Buf;
	}
	return O;
}

} // namespace MathSciSimdUtil
} // namespace AstralDB
