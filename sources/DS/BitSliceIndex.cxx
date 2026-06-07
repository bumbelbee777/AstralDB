#include <DS/BitSliceIndex.hxx>

#include <algorithm>
#include <cmath>

namespace AstralDB {

void BitSliceIndex::Build(const std::vector<std::vector<std::string>> &DimValues) {
	Slices_.clear();
	BitsPerDim_.clear();
	Shift_.clear();
	Cardinality_.clear();
	unsigned TotalBits = 0;
	for(const auto &Dim : DimValues) {
		const std::size_t Card = Dim.size();
		Cardinality_.push_back(Card);
		unsigned Bits = Card <= 1 ? 0u : static_cast<unsigned>(std::ceil(std::log2(static_cast<double>(Card))));
		BitsPerDim_.push_back(Bits);
		Shift_.push_back(TotalBits);
		TotalBits += Bits;
		std::vector<std::size_t> Map(Card);
		for(std::size_t I = 0; I < Card; ++I)
			Map[I] = I;
		Slices_.push_back(std::move(Map));
	}
}

std::size_t BitSliceIndex::Encode(const std::vector<std::size_t> &DimIndices) const {
	std::size_t Code = 0;
	for(std::size_t D = 0; D < DimIndices.size() && D < BitsPerDim_.size(); ++D) {
		const std::size_t Idx = DimIndices[D];
		if(Idx < Cardinality_[D])
			Code |= (Idx << Shift_[D]);
	}
	return Code;
}

} // namespace AstralDB
