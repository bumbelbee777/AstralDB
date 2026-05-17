#pragma once

#include <cmath>
#include <limits>

namespace AstralDB {

/** Division that returns quiet NaN when \p Denom is zero (SQL-friendly). */
inline double SafeDiv(double Num, double Denom) {
	if(Denom == 0.0)
		return std::numeric_limits<double>::quiet_NaN();
	return Num / Denom;
}

} // namespace AstralDB
