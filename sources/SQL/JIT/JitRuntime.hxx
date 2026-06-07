#pragma once

#include <Database/Storage/ColumnFilterSimd.hxx>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace AstralDB {
namespace SQL {

/** Apply JIT or SIMD filter on a dense i64 batch; \p OutIndices maps into the batch. */
void FilterI64Batch(FilterCompareOp Op, int64_t Literal, const int64_t *Values, std::size_t Count,
                    std::vector<std::size_t> &OutIndices);

} // namespace SQL
} // namespace AstralDB
