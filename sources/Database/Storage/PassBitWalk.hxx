#pragma once

#include <Database/Storage/ColumnarStorage.hxx>
#include <Database/Storage/ColumnZoneMap.hxx>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace AstralDB {

/** O(passing) pass-bit traversal: sparse word list, then non-zero groups, then non-zero words. */
struct PassBitWordWalk {
	const std::uint64_t *Bits = nullptr;
	std::size_t RowCount = 0;
	const std::uint32_t *SparseWords = nullptr;
	std::size_t SparseWordCount = 0;
	const std::uint32_t *GroupCounts = nullptr;
	std::size_t GroupCount = 0;

	[[nodiscard]] static PassBitWordWalk FromColumn(const ColumnarTable &Col) noexcept;
};

inline std::uint64_t PassBitWordMaskedAt(const std::uint64_t *Bits, const std::size_t WordIndex,
                                         const std::size_t RowCount) noexcept {
	const std::uint64_t Word = Bits[WordIndex];
	const std::size_t Base = WordIndex * 64;
	if(Base + 64 <= RowCount)
		return Word;
	const std::size_t Valid = RowCount - Base;
	if(Valid >= 64)
		return Word;
	return Word & ((1ULL << Valid) - 1);
}

template <typename Fn>
inline void ForEachNonemptyPassWord(const PassBitWordWalk &Walk, Fn &&FnWord) {
	if(Walk.SparseWords != nullptr && Walk.SparseWordCount > 0) {
		for(std::size_t I = 0; I < Walk.SparseWordCount; ++I)
			FnWord(Walk.SparseWords[I]);
		return;
	}
	const std::size_t Words = Walk.RowCount > 0 ? (Walk.RowCount + 63) / 64 : 0;
	if(Walk.Bits != nullptr && Walk.GroupCounts != nullptr && Walk.GroupCount > 0) {
		constexpr std::size_t WordsPerGroup = kColumnRowGroupSize / 64;
		for(std::size_t G = 0; G < Walk.GroupCount; ++G) {
			if(Walk.GroupCounts[G] == 0)
				continue;
			const std::size_t W0 = G * WordsPerGroup;
			const std::size_t W1 = std::min(Words, (G + 1) * WordsPerGroup);
			for(std::size_t W = W0; W < W1; ++W) {
				if(Walk.Bits[W] != 0)
					FnWord(W);
			}
		}
		return;
	}
	if(!Walk.Bits || Words == 0)
		return;
	for(std::size_t W = 0; W < Words; ++W) {
		if(Walk.Bits[W] != 0)
			FnWord(W);
	}
}

/** Fixed-size heap for pass-bit top-K (faster than \c std::priority_queue). */
template <typename RowT = std::uint32_t>
struct PassBitTopKHeap {
	struct Entry {
		float Key = 0.f;
		RowT Row = 0;
	};

	std::vector<Entry> Data;
	std::size_t Cap = 0;
	bool Ascending = false;

	void Reset(const std::size_t K, const bool Asc) noexcept {
		Cap = K;
		Ascending = Asc;
		Data.clear();
		Data.reserve(K);
	}

	void Consider(const float Key, const RowT Row) noexcept {
		if(Ascending) {
			if(Data.size() < Cap) {
				Data.push_back({Key, Row});
				std::push_heap(Data.begin(), Data.end(), MaxKeyCmp{});
				return;
			}
			if(Key >= Data.front().Key)
				return;
			std::pop_heap(Data.begin(), Data.end(), MaxKeyCmp{});
			Data.back() = {Key, Row};
			std::push_heap(Data.begin(), Data.end(), MaxKeyCmp{});
			return;
		}
		if(Data.size() < Cap) {
			Data.push_back({Key, Row});
			std::push_heap(Data.begin(), Data.end(), MinKeyCmp{});
			return;
		}
		if(Key <= Data.front().Key)
			return;
		std::pop_heap(Data.begin(), Data.end(), MinKeyCmp{});
		Data.back() = {Key, Row};
		std::push_heap(Data.begin(), Data.end(), MinKeyCmp{});
	}

	void ExtractSorted(std::vector<Entry> &Out) {
		Out = std::move(Data);
		const auto Less = [this](const Entry &A, const Entry &B) {
			return Ascending ? (A.Key < B.Key) : (A.Key > B.Key);
		};
		std::sort(Out.begin(), Out.end(), Less);
	}

private:
	struct MinKeyCmp {
		bool operator()(const Entry &A, const Entry &B) const noexcept { return A.Key > B.Key; }
	};
	struct MaxKeyCmp {
		bool operator()(const Entry &A, const Entry &B) const noexcept { return A.Key < B.Key; }
	};
};

} // namespace AstralDB
