#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace AstralDB {

/** Bit-sliced coordinates for low-cardinality CUBE dimensions. */
class BitSliceIndex {
public:
	void Build(const std::vector<std::vector<std::string>> &DimValues);
	[[nodiscard]] std::size_t Encode(const std::vector<std::size_t> &DimIndices) const;
	[[nodiscard]] std::size_t DimCount() const noexcept { return Slices_.size(); }

private:
	std::vector<unsigned> BitsPerDim_;
	std::vector<unsigned> Shift_;
	std::vector<std::size_t> Cardinality_;
	std::vector<std::vector<std::size_t>> Slices_;
};

} // namespace AstralDB
