#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {

/** Allocation-free JSON parse/extract (simdjson-style structural scan). */
struct SimdJsonExtract {
	/** Iterative structural validation (no DOM allocation). */
	static bool Valid(std::string_view Json) noexcept;

	/** Extract scalar at dot path (e.g. \c payment_method, \c nested.field). */
	static bool Extract(std::string_view Json, std::string_view Path, std::string &Out);

	/** Batch extract for vectorized filters; \p RowIds parallel to \p Jsons. */
	static void ExtractBatch(const std::vector<std::string_view> &Jsons, std::string_view Path,
	                         std::vector<std::string> &Out);
};

} // namespace AstralDB
