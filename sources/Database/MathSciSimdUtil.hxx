#pragma once

#include <cstddef>
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

} // namespace MathSciSimdUtil
} // namespace AstralDB
