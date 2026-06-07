#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace AstralDB {
namespace OlapAggregateMicrokernels {

/** Fast parse of OLAP numeric cells (integer-first, then \c stod fallback). */
bool TryParseF64Cell(std::string_view Cell, double &Out) noexcept;

/** Accumulate parsed value into \p Acc (SIMD-friendly scalar path). */
void FusedParseAddF64(std::string_view Cell, double &Acc) noexcept;

/** Sum a contiguous double buffer (AVX2/NEON when available). */
double SumF64(const double *Values, std::size_t Count) noexcept;

/** Sum parsed numeric cells for GROUP BY / OLAP SUM paths. */
double SumCellsF64(const std::string *Cells, std::size_t Count) noexcept;

} // namespace OlapAggregateMicrokernels
} // namespace AstralDB
