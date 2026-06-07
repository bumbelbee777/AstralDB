#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace AstralDB::MathSciListCache {

/** LRU-cached parse of \c L[n]:… numeric list cells (bounded; shared across MathSci modules). */
const std::vector<double> *LookupDoubles(std::string_view Cell);

/** Parse without inserting into the shared cache (for one-off literals). */
std::optional<std::vector<double>> ParseDoublesUncached(std::string_view Cell);

void ClearCacheForTests();

} // namespace AstralDB::MathSciListCache
