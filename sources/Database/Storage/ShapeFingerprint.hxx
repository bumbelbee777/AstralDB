#pragma once

#include <cstdint>

namespace AstralDB {

struct QueryShapeFingerprint128 {
	std::uint64_t Lo = 0;
	std::uint64_t Hi = 0;
	bool operator==(const QueryShapeFingerprint128 &) const = default;
};

} // namespace AstralDB
