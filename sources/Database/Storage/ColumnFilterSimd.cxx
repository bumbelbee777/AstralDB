#include <Database/Storage/ColumnFilterSimd.hxx>
#include <Database/Storage/BulkSynthetic.hxx>
#include <Database/Storage/BulkSyntheticSemistructured.hxx>
#include <Database/Storage/PassBitWalk.hxx>

#include <thread>
#include <Database/Storage/ColumnZoneMap.hxx>
#include <Database/Storage/PassBitWalk.hxx>
#include <Database/Storage/SimdTiling.hxx>
#include <IO/Job.hxx>
#include <IO/SIMD.hxx>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <future>
#include <limits>
#include <queue>
#include <vector>

#if defined(__AVX512F__)
#include <immintrin.h>
#elif defined(__AVX2__)
#include <immintrin.h>
#endif
#if defined(__ARM_FEATURE_SVE)
#include <arm_sve.h>
#endif
#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace AstralDB {

void ParseColumnToI64(const std::vector<std::string> &Cells, std::vector<int64_t> &Out) {
	Out.resize(Cells.size());
	for(std::size_t I = 0; I < Cells.size(); ++I) {
		char *End = nullptr;
		const long long V = std::strtoll(Cells[I].c_str(), &End, 10);
		Out[I] = (End != Cells[I].c_str() && *End == '\0') ? static_cast<int64_t>(V) : 0;
	}
}

namespace {

bool ScalarFilterHit(int64_t Value, FilterCompareOp Op, int64_t Literal) noexcept {
	switch(Op) {
	case FilterCompareOp::Eq:
		return Value == Literal;
	case FilterCompareOp::Ne:
		return Value != Literal;
	case FilterCompareOp::Gt:
		return Value > Literal;
	case FilterCompareOp::Ge:
		return Value >= Literal;
	case FilterCompareOp::Lt:
		return Value < Literal;
	case FilterCompareOp::Le:
		return Value <= Literal;
	}
	return false;
}

void FilterI64ScalarTail(const int64_t *Values, std::size_t Start, std::size_t Count, FilterCompareOp Op,
                         int64_t Literal, std::vector<std::size_t> &OutIndices) {
	for(std::size_t I = Start; I < Count; ++I) {
		if(ScalarFilterHit(Values[I], Op, Literal))
			OutIndices.push_back(I);
	}
}

#if defined(__AVX512F__)
void AppendMask8(std::size_t Offset, __mmask8 Mask, std::vector<std::size_t> &Out) {
	const unsigned Bits = static_cast<unsigned>(Mask);
	for(int B = 0; B < 8; ++B) {
		if((Bits >> B) & 1U)
			Out.push_back(Offset + static_cast<std::size_t>(B));
	}
}

__mmask8 CompareMaskI64x8(__m512i V, __m512i Need, FilterCompareOp Op) {
	switch(Op) {
	case FilterCompareOp::Eq:
		return _mm512_cmpeq_epi64_mask(V, Need);
	case FilterCompareOp::Ne:
		return _mm512_cmpneq_epi64_mask(V, Need);
	case FilterCompareOp::Gt:
		return _mm512_cmpgt_epi64_mask(V, Need);
	case FilterCompareOp::Ge:
		return _kor_mask8(_mm512_cmpeq_epi64_mask(V, Need), _mm512_cmpgt_epi64_mask(V, Need));
	case FilterCompareOp::Lt:
		return _mm512_cmplt_epi64_mask(V, Need);
	case FilterCompareOp::Le:
		return _kor_mask8(_mm512_cmpeq_epi64_mask(V, Need), _mm512_cmplt_epi64_mask(V, Need));
	default:
		return 0;
	}
}

void FilterI64Avx512(const int64_t *Values, std::size_t Count, FilterCompareOp Op, int64_t Literal,
                     std::vector<std::size_t> &OutIndices) {
	const __m512i Need = _mm512_set1_epi64(Literal);
	std::size_t I = 0;
	for(; I + 8 <= Count; I += 8) {
		const __m512i V = _mm512_loadu_si512(reinterpret_cast<const __m512i *>(Values + I));
		AppendMask8(I, CompareMaskI64x8(V, Need, Op), OutIndices);
	}
	FilterI64ScalarTail(Values, I, Count, Op, Literal, OutIndices);
}
#endif

#if defined(__AVX2__)
__m256i LoadI64x4(const int64_t *P) {
	return _mm256_set_epi64x(P[3], P[2], P[1], P[0]);
}

void AppendMask4(std::size_t Offset, __m256i Mask, std::vector<std::size_t> &Out) {
	const int Bits = _mm256_movemask_pd(_mm256_castsi256_pd(Mask));
	for(int B = 0; B < 4; ++B) {
		if((Bits >> B) & 1)
			Out.push_back(Offset + static_cast<std::size_t>(B));
	}
}

void FilterI64Avx2Kernel(const int64_t *Values, std::size_t Count, FilterCompareOp Op, int64_t Literal,
                         std::vector<std::size_t> &OutIndices) {
	const __m256i Need = _mm256_set1_epi64x(Literal);
	std::size_t I = 0;
	for(; I + 4 <= Count; I += 4) {
		const __m256i V = LoadI64x4(Values + I);
		__m256i M;
		switch(Op) {
		case FilterCompareOp::Eq:
			M = _mm256_cmpeq_epi64(V, Need);
			break;
		case FilterCompareOp::Ne:
			M = _mm256_xor_si256(_mm256_cmpeq_epi64(V, Need), _mm256_set1_epi64x(-1));
			break;
		case FilterCompareOp::Gt:
			M = _mm256_cmpgt_epi64(V, Need);
			break;
		case FilterCompareOp::Ge: {
			const __m256i Eq = _mm256_cmpeq_epi64(V, Need);
			const __m256i Gt = _mm256_cmpgt_epi64(V, Need);
			M = _mm256_or_si256(Eq, Gt);
		} break;
		case FilterCompareOp::Lt:
			M = _mm256_cmpgt_epi64(Need, V);
			break;
		case FilterCompareOp::Le: {
			const __m256i Eq = _mm256_cmpeq_epi64(V, Need);
			const __m256i Lt = _mm256_cmpgt_epi64(Need, V);
			M = _mm256_or_si256(Eq, Lt);
		} break;
		default:
			M = _mm256_setzero_si256();
			break;
		}
		AppendMask4(I, M, OutIndices);
	}
	FilterI64ScalarTail(Values, I, Count, Op, Literal, OutIndices);
}
#endif

#if defined(__ARM_FEATURE_SVE)
svbool_t ComparePgSveI64(svint64_t V, svint64_t Need, FilterCompareOp Op) {
	switch(Op) {
	case FilterCompareOp::Eq:
		return svcmpeq_s64(svptrue_b64(), V, Need);
	case FilterCompareOp::Ne:
		return svcmpne_s64(svptrue_b64(), V, Need);
	case FilterCompareOp::Gt:
		return svcmpgt_s64(svptrue_b64(), V, Need);
	case FilterCompareOp::Ge:
		return svcmpge_s64(svptrue_b64(), V, Need);
	case FilterCompareOp::Lt:
		return svcmplt_s64(svptrue_b64(), V, Need);
	case FilterCompareOp::Le:
		return svcmple_s64(svptrue_b64(), V, Need);
	default:
		return svdup_b64(false);
	}
}

void FilterI64Sve(const int64_t *Values, std::size_t Count, FilterCompareOp Op, int64_t Literal,
                  std::vector<std::size_t> &OutIndices) {
	alignas(64) int64_t HitBuf[16];
	const svint64_t Need = svdup_s64(Literal);
	const std::size_t LaneCount = static_cast<std::size_t>(svcntd());
	std::size_t I = 0;
	while(I < Count) {
		const svbool_t Pg = svwhilelt_b64(I, Count);
		const svint64_t V = svld1_s64(Pg, Values + I);
		const svbool_t Hit = ComparePgSveI64(V, Need, Op);
		const svint64_t HitLane = svsel_s64(Hit, svdup_s64(1), svdup_s64(0));
		svst1_s64(Pg, HitBuf, HitLane);
		for(std::size_t Lane = 0; Lane < LaneCount && I + Lane < Count; ++Lane) {
			if(HitBuf[Lane] != 0)
				OutIndices.push_back(I + Lane);
		}
		I += LaneCount;
	}
}
#endif

#if defined(__ARM_NEON) || defined(__aarch64__)
void AppendMaskNeon2(std::size_t Offset, uint64x2_t Mask, std::vector<std::size_t> &Out) {
	if(vgetq_lane_u64(Mask, 0) != 0)
		Out.push_back(Offset);
	if(vgetq_lane_u64(Mask, 1) != 0)
		Out.push_back(Offset + 1);
}

uint64x2_t CompareMaskNeonI64(int64x2_t V, int64x2_t Need, FilterCompareOp Op) {
	switch(Op) {
	case FilterCompareOp::Eq:
		return vreinterpretq_u64_s64(vceqq_s64(V, Need));
	case FilterCompareOp::Ne:
		return vreinterpretq_u64_s64(veorq_s64(vceqq_s64(V, Need), vdupq_n_s64(-1)));
	case FilterCompareOp::Gt:
		return vreinterpretq_u64_s64(vcgtq_s64(V, Need));
	case FilterCompareOp::Ge:
		return vreinterpretq_u64_s64(vcgeq_s64(V, Need));
	case FilterCompareOp::Lt:
		return vreinterpretq_u64_s64(vcltq_s64(V, Need));
	case FilterCompareOp::Le:
		return vreinterpretq_u64_s64(vcleq_s64(V, Need));
	default:
		return vdupq_n_u64(0);
	}
}

void FilterI64Neon(const int64_t *Values, std::size_t Count, FilterCompareOp Op, int64_t Literal,
                   std::vector<std::size_t> &OutIndices) {
	const int64x2_t Need = vdupq_n_s64(Literal);
	std::size_t I = 0;
	for(; I + 2 <= Count; I += 2) {
		const int64x2_t V = vld1q_s64(Values + I);
		AppendMaskNeon2(I, CompareMaskNeonI64(V, Need, Op), OutIndices);
	}
	FilterI64ScalarTail(Values, I, Count, Op, Literal, OutIndices);
}
#endif

} // namespace

void FilterI64Simd(const int64_t *Values, std::size_t Count, FilterCompareOp Op, int64_t Literal,
                   std::vector<std::size_t> &OutIndices) {
	OutIndices.clear();
	if(!Values || Count == 0)
		return;
	OutIndices.reserve(Count / 8 + 8);
#if defined(__AVX512F__)
	FilterI64Avx512(Values, Count, Op, Literal, OutIndices);
#elif defined(__ARM_FEATURE_SVE)
	FilterI64Sve(Values, Count, Op, Literal, OutIndices);
#elif defined(__AVX2__)
	FilterI64Avx2Kernel(Values, Count, Op, Literal, OutIndices);
#elif defined(__ARM_NEON) || defined(__aarch64__)
	FilterI64Neon(Values, Count, Op, Literal, OutIndices);
#else
	FilterI64ScalarTail(Values, 0, Count, Op, Literal, OutIndices);
#endif
}

int64_t SumI64Avx2(const int64_t *Values, const std::size_t Count) {
	return Simd::SumI64(Values, Count);
}

int64_t MinI64Avx2(const int64_t *Values, const std::size_t Count) {
	return Simd::MinI64(Values, Count);
}

int64_t MaxI64Avx2(const int64_t *Values, const std::size_t Count) {
	return Simd::MaxI64(Values, Count);
}

namespace {

void AppendSetBitsFromWord(std::uint64_t Word, std::size_t BaseRow, std::size_t RowCount,
                         std::vector<std::size_t> &Out) {
	while(Word != 0) {
		const unsigned Bit = static_cast<unsigned>(std::countr_zero(Word));
		const std::size_t Row = BaseRow + Bit;
		if(Row < RowCount)
			Out.push_back(Row);
		Word &= Word - 1;
	}
}

PassBitWordWalk PassBitWordWalkFromParams(const PassBitTopKParams &Params) noexcept {
	PassBitWordWalk Walk;
	Walk.Bits = Params.Bits;
	Walk.RowCount = Params.RowCount;
	Walk.SparseWords = Params.SparsePassWords;
	Walk.SparseWordCount = Params.SparsePassWordCount;
	Walk.GroupCounts = Params.PassGroupCounts;
	Walk.GroupCount = Params.PassGroupCount;
	return Walk;
}

} // namespace

void ScanPassBitsIndices(const std::uint64_t *Bits, const std::size_t RowCount, std::vector<std::size_t> &OutIndices) {
	if(!Bits || RowCount == 0)
		return;
	OutIndices.reserve(OutIndices.size() + RowCount / 16);
	const std::size_t Words = (RowCount + 63) / 64;
	for(std::size_t W = 0; W < Words; ++W) {
		const std::uint64_t Word = PassBitWordMaskedAt(Bits, W, RowCount);
		if(Word != 0)
			AppendSetBitsFromWord(Word, W * 64, RowCount, OutIndices);
	}
}

void ScanPassBitsIndicesSparse(const std::uint64_t *Bits, const std::uint32_t *SparseWords, const std::size_t SparseWordCount,
                               const std::size_t RowCount, std::vector<std::size_t> &OutIndices) {
	if(!Bits || !SparseWords || SparseWordCount == 0 || RowCount == 0)
		return;
	OutIndices.reserve(OutIndices.size() + SparseWordCount * 4);
	for(std::size_t I = 0; I < SparseWordCount; ++I) {
		const std::size_t W = SparseWords[I];
		AppendSetBitsFromWord(PassBitWordMaskedAt(Bits, W, RowCount), W * 64, RowCount, OutIndices);
	}
}

void ScanPassBitsGroupWalk(const std::uint64_t *Bits, const std::uint32_t *GroupPassCounts,
                           const std::size_t GroupCount, const std::size_t RowCount,
                           std::vector<std::size_t> &OutIndices) {
	if(!Bits || !GroupPassCounts || GroupCount == 0 || RowCount == 0)
		return;
	const std::size_t Words = (RowCount + 63) / 64;
	constexpr std::size_t WordsPerGroup = kColumnRowGroupSize / 64;
	OutIndices.reserve(OutIndices.size() + RowCount / 16);
	for(std::size_t G = 0; G < GroupCount; ++G) {
		if(GroupPassCounts[G] == 0)
			continue;
		const std::size_t W0 = G * WordsPerGroup;
		const std::size_t W1 = std::min(Words, (G + 1) * WordsPerGroup);
		for(std::size_t W = W0; W < W1; ++W) {
			const std::uint64_t Word = PassBitWordMaskedAt(Bits, W, RowCount);
			if(Word != 0)
				AppendSetBitsFromWord(Word, W * 64, RowCount, OutIndices);
		}
	}
}

namespace {

/** Finer rank histogram (keys in \c [0,1]) keeps cut/partial buckets small at 100M scale. */
constexpr int kRankBuckets = 4096;

inline int RankF32ToBucket(const float Key) noexcept {
	if(!std::isfinite(Key))
		return 0;
	int M = static_cast<int>(Key * static_cast<float>(kRankBuckets - 1) + 0.5f);
	if(M < 0)
		M = 0;
	else if(M >= kRankBuckets)
		M = kRankBuckets - 1;
	return M;
}

/** DESC top-K with dense f32 keys: histogram pass, then selective bucket collect (default). */
void SelectTopKFromPassBitsBucketDesc(const PassBitTopKParams &Params, std::vector<std::size_t> &OutSortedRowIndices) {
	OutSortedRowIndices.clear();
	if(!Params.Bits || Params.RowCount == 0 || Params.K == 0 || Params.Keys == nullptr)
		return;
	const float *const Keys = Params.Keys;
	const std::size_t Keep = Params.K;
	std::array<std::uint32_t, kRankBuckets> Counts{};
	const PassBitWordWalk Walk = PassBitWordWalkFromParams(Params);
	auto ScanPassRows = [&](const auto &OnRow) {
		ForEachNonemptyPassWord(Walk, [&](const std::size_t W) {
			std::uint64_t Word = PassBitWordMaskedAt(Walk.Bits, W, Walk.RowCount);
			const std::size_t Base = W * 64;
			while(Word != 0) {
				const unsigned Bit = static_cast<unsigned>(std::countr_zero(Word));
				const std::size_t Row = Base + Bit;
				if(Row < Walk.RowCount)
					OnRow(Row);
				Word &= Word - 1;
			}
		});
	};
	ScanPassRows([&](const std::size_t Row) {
#if defined(__GNUC__) || defined(__clang__)
		if(const std::size_t Pf = Row + 64; Pf < Walk.RowCount)
			__builtin_prefetch(Keys + Pf, 0, 0);
#endif
		++Counts[static_cast<std::size_t>(RankF32ToBucket(Keys[Row]))];
	});
	std::size_t Need = Keep;
	int CutBucket = -1;
	for(int M = kRankBuckets - 1; M >= 0; --M) {
		const std::uint32_t C = Counts[static_cast<std::size_t>(M)];
		if(C == 0)
			continue;
		if(C >= Need) {
			CutBucket = M;
			break;
		}
		Need -= C;
	}
	if(CutBucket < 0) {
		PassBitTopKHeap<std::uint32_t> Fallback;
		Fallback.Reset(Keep, false);
		ScanPassRows([&](const std::size_t Row) {
			Fallback.Consider(Keys[Row], static_cast<std::uint32_t>(Row));
		});
		std::vector<typename PassBitTopKHeap<std::uint32_t>::Entry> Sorted;
		Fallback.ExtractSorted(Sorted);
		OutSortedRowIndices.reserve(Sorted.size());
		for(const auto &E : Sorted)
			OutSortedRowIndices.push_back(E.Row);
		return;
	}
	const std::size_t RowsFromHigh = Keep - Need;
	const std::size_t TakeFromCut = Need;
	OutSortedRowIndices.reserve(Keep);
	int PartialHighBucket = -1;
	std::size_t PartialHighNeed = 0;
	std::array<bool, kRankBuckets> CollectFullBucket{};
	CollectFullBucket.fill(false);
	if(RowsFromHigh > 0) {
		std::size_t Left = RowsFromHigh;
		for(int M = kRankBuckets - 1; M > CutBucket; --M) {
			const std::uint32_t C = Counts[static_cast<std::size_t>(M)];
			if(C == 0)
				continue;
			if(C <= Left) {
				CollectFullBucket[static_cast<std::size_t>(M)] = true;
				Left -= C;
			} else {
				PartialHighBucket = M;
				PartialHighNeed = Left;
				Left = 0;
				break;
			}
		}
	}
	PassBitTopKHeap<std::uint32_t> CutHeap;
	PassBitTopKHeap<std::uint32_t> PartialHighHeap;
	if(TakeFromCut > 0)
		CutHeap.Reset(TakeFromCut, false);
	if(PartialHighBucket >= 0 && PartialHighNeed > 0)
		PartialHighHeap.Reset(PartialHighNeed, false);
	const std::size_t RowCap = Walk.RowCount;
	const float HighBucketMinKey =
	    CutBucket > 0 ? static_cast<float>(CutBucket) / static_cast<float>(kRankBuckets) : 0.f;
	const bool UseGroupPrune = Params.PassGroupMaxRank != nullptr && Params.PassGroupCounts != nullptr &&
	                         Params.PassGroupCount > 0 && CutBucket > 0;
	const auto ScanCollectRow = [&](const std::size_t Row) {
		const int M = RankF32ToBucket(Keys[Row]);
		if(M > CutBucket) {
			if(M == PartialHighBucket)
				PartialHighHeap.Consider(Keys[Row], static_cast<std::uint32_t>(Row));
			else if(CollectFullBucket[static_cast<std::size_t>(M)])
				OutSortedRowIndices.push_back(Row);
		} else if(M == CutBucket && TakeFromCut > 0) {
#if defined(__GNUC__) || defined(__clang__)
			if(const std::size_t Pf = Row + 64; Pf < RowCap)
				__builtin_prefetch(Keys + Pf, 0, 1);
#endif
			CutHeap.Consider(Keys[Row], static_cast<std::uint32_t>(Row));
		}
	};
	if(UseGroupPrune) {
		constexpr std::size_t WordsPerGroup = kColumnRowGroupSize / 64;
		const std::size_t Words = (Walk.RowCount + 63) / 64;
		for(std::size_t G = 0; G < Params.PassGroupCount; ++G) {
			if(Params.PassGroupCounts[G] == 0)
				continue;
			if(Params.PassGroupMaxRank[G] < HighBucketMinKey)
				continue;
			const std::size_t W0 = G * WordsPerGroup;
			const std::size_t W1 = std::min(Words, (G + 1) * WordsPerGroup);
			for(std::size_t W = W0; W < W1; ++W) {
				std::uint64_t Word = PassBitWordMaskedAt(Walk.Bits, W, Walk.RowCount);
				const std::size_t Base = W * 64;
				while(Word != 0) {
					const unsigned Bit = static_cast<unsigned>(std::countr_zero(Word));
					const std::size_t Row = Base + Bit;
					if(Row < Walk.RowCount)
						ScanCollectRow(Row);
					Word &= Word - 1;
				}
			}
		}
	} else {
		ScanPassRows(ScanCollectRow);
	}
	if(PartialHighBucket >= 0 && OutSortedRowIndices.size() < RowsFromHigh) {
		std::vector<typename PassBitTopKHeap<std::uint32_t>::Entry> HighWinners;
		PartialHighHeap.ExtractSorted(HighWinners);
		for(const auto &E : HighWinners)
			OutSortedRowIndices.push_back(E.Row);
	}
	if(TakeFromCut > 0) {
		std::vector<typename PassBitTopKHeap<std::uint32_t>::Entry> CutWinners;
		CutHeap.ExtractSorted(CutWinners);
		for(const auto &E : CutWinners)
			OutSortedRowIndices.push_back(E.Row);
	}
	if(OutSortedRowIndices.size() > Keep)
		OutSortedRowIndices.resize(Keep);
}

/** ASC top-K with dense f32 keys (e.g. haversine distance meters): histogram from low buckets. */
void SelectTopKFromPassBitsBucketAsc(const PassBitTopKParams &Params, std::vector<std::size_t> &OutSortedRowIndices) {
	OutSortedRowIndices.clear();
	if(!Params.Bits || Params.RowCount == 0 || Params.K == 0 || Params.Keys == nullptr)
		return;
	const float *const Keys = Params.Keys;
	const std::size_t Keep = Params.K;
	constexpr float kMaxDistanceMeters = 20037508.f;
	const auto KeyToBucket = [&](const float Key) -> int {
		if(!std::isfinite(Key) || Key <= 0.f)
			return 0;
		int M = static_cast<int>((Key / kMaxDistanceMeters) * static_cast<float>(kRankBuckets - 1));
		if(M < 0)
			M = 0;
		else if(M >= kRankBuckets)
			M = kRankBuckets - 1;
		return M;
	};
	std::array<std::uint32_t, kRankBuckets> Counts{};
	const PassBitWordWalk Walk = PassBitWordWalkFromParams(Params);
	auto ScanPassRows = [&](const auto &OnRow) {
		ForEachNonemptyPassWord(Walk, [&](const std::size_t W) {
			std::uint64_t Word = PassBitWordMaskedAt(Walk.Bits, W, Walk.RowCount);
			const std::size_t Base = W * 64;
			while(Word != 0) {
				const unsigned Bit = static_cast<unsigned>(std::countr_zero(Word));
				const std::size_t Row = Base + Bit;
				if(Row < Walk.RowCount)
					OnRow(Row);
				Word &= Word - 1;
			}
		});
	};
	ScanPassRows([&](const std::size_t Row) {
#if defined(__GNUC__) || defined(__clang__)
		if(const std::size_t Pf = Row + 64; Pf < Walk.RowCount)
			__builtin_prefetch(Keys + Pf, 0, 0);
#endif
		++Counts[static_cast<std::size_t>(KeyToBucket(Keys[Row]))];
	});
	std::size_t Need = Keep;
	int CutBucket = -1;
	for(int M = 0; M < kRankBuckets; ++M) {
		const std::uint32_t C = Counts[static_cast<std::size_t>(M)];
		if(C == 0)
			continue;
		if(C >= Need) {
			CutBucket = M;
			break;
		}
		Need -= C;
	}
	if(CutBucket < 0) {
		PassBitTopKHeap<std::uint32_t> Fallback;
		Fallback.Reset(Keep, true);
		ScanPassRows([&](const std::size_t Row) {
			Fallback.Consider(Keys[Row], static_cast<std::uint32_t>(Row));
		});
		std::vector<typename PassBitTopKHeap<std::uint32_t>::Entry> Sorted;
		Fallback.ExtractSorted(Sorted);
		OutSortedRowIndices.reserve(Sorted.size());
		for(const auto &E : Sorted)
			OutSortedRowIndices.push_back(E.Row);
		return;
	}
	const std::size_t RowsFromLow = Keep - Need;
	const std::size_t TakeFromCut = Need;
	OutSortedRowIndices.reserve(Keep);
	int PartialLowBucket = -1;
	std::size_t PartialLowNeed = 0;
	std::array<bool, kRankBuckets> CollectFullBucket{};
	CollectFullBucket.fill(false);
	if(RowsFromLow > 0) {
		std::size_t Left = RowsFromLow;
		for(int M = 0; M < CutBucket; ++M) {
			const std::uint32_t C = Counts[static_cast<std::size_t>(M)];
			if(C == 0)
				continue;
			if(C <= Left) {
				CollectFullBucket[static_cast<std::size_t>(M)] = true;
				Left -= C;
			} else {
				PartialLowBucket = M;
				PartialLowNeed = Left;
				Left = 0;
				break;
			}
		}
	}
	PassBitTopKHeap<std::uint32_t> CutHeap;
	PassBitTopKHeap<std::uint32_t> PartialLowHeap;
	if(TakeFromCut > 0)
		CutHeap.Reset(TakeFromCut, true);
	if(PartialLowBucket >= 0 && PartialLowNeed > 0)
		PartialLowHeap.Reset(PartialLowNeed, true);
	const auto ScanCollectRow = [&](const std::size_t Row) {
		const int M = KeyToBucket(Keys[Row]);
		if(M < CutBucket) {
			if(M == PartialLowBucket)
				PartialLowHeap.Consider(Keys[Row], static_cast<std::uint32_t>(Row));
			else if(CollectFullBucket[static_cast<std::size_t>(M)])
				OutSortedRowIndices.push_back(Row);
		} else if(M == CutBucket && TakeFromCut > 0)
			CutHeap.Consider(Keys[Row], static_cast<std::uint32_t>(Row));
	};
	ScanPassRows(ScanCollectRow);
	if(PartialLowBucket >= 0 && OutSortedRowIndices.size() < RowsFromLow) {
		std::vector<typename PassBitTopKHeap<std::uint32_t>::Entry> LowWinners;
		PartialLowHeap.ExtractSorted(LowWinners);
		for(const auto &E : LowWinners)
			OutSortedRowIndices.push_back(E.Row);
	}
	if(TakeFromCut > 0) {
		std::vector<typename PassBitTopKHeap<std::uint32_t>::Entry> CutWinners;
		CutHeap.ExtractSorted(CutWinners);
		for(const auto &E : CutWinners)
			OutSortedRowIndices.push_back(E.Row);
	}
	if(OutSortedRowIndices.size() > Keep)
		OutSortedRowIndices.resize(Keep);
}

} // namespace

void SelectTopKFromPassBitsHeap(const PassBitTopKParams &Params, std::vector<std::size_t> &OutSortedRowIndices) {
	OutSortedRowIndices.clear();
	if(!Params.Bits || Params.RowCount == 0 || Params.K == 0 || Params.Keys == nullptr)
		return;
	const float *const Keys = Params.Keys;
	const std::size_t Keep = Params.K;
	const PassBitWordWalk Walk = PassBitWordWalkFromParams(Params);
	PassBitTopKHeap<std::uint32_t> Heap;
	Heap.Reset(Keep, Params.Ascending);
	ForEachNonemptyPassWord(Walk, [&](const std::size_t W) {
		std::uint64_t Word = PassBitWordMaskedAt(Walk.Bits, W, Walk.RowCount);
		const std::size_t Base = W * 64;
		while(Word != 0) {
			const unsigned Bit = static_cast<unsigned>(std::countr_zero(Word));
			const std::size_t Row = Base + Bit;
			if(Row < Walk.RowCount)
				Heap.Consider(Keys[Row], static_cast<std::uint32_t>(Row));
			Word &= Word - 1;
		}
	});
	std::vector<typename PassBitTopKHeap<std::uint32_t>::Entry> Winners;
	Heap.ExtractSorted(Winners);
	OutSortedRowIndices.reserve(Winners.size());
	for(const auto &E : Winners)
		OutSortedRowIndices.push_back(E.Row);
}

void SelectTopKPhysicalDescReverseWalk(const PassBitTopKParams &Params,
                                       std::vector<std::size_t> &OutSortedRowIndices) {
	OutSortedRowIndices.clear();
	if(!Params.Bits || Params.RowCount == 0 || Params.K == 0)
		return;
	const std::size_t Keep = Params.K;
	const PassBitWordWalk Walk = PassBitWordWalkFromParams(Params);
	const auto CollectWordDesc = [&](const std::size_t W, std::uint64_t Word) {
		const std::size_t Base = W * 64;
		const std::size_t Valid =
		    Base < Params.RowCount ? std::min<std::size_t>(64, Params.RowCount - Base) : 0;
		if(Valid == 0)
			return;
		if(Valid < 64)
			Word &= (1ULL << Valid) - 1;
		for(int Bit = static_cast<int>(Valid) - 1; Bit >= 0 && OutSortedRowIndices.size() < Keep; --Bit) {
			if((Word >> static_cast<unsigned>(Bit)) & 1ULL) {
				const std::size_t Row = Base + static_cast<std::size_t>(Bit);
				OutSortedRowIndices.push_back(Row);
			}
		}
	};
	if(Walk.SparseWords != nullptr && Walk.SparseWordCount > 0) {
		for(std::size_t I = Walk.SparseWordCount; I-- > 0 && OutSortedRowIndices.size() < Keep;) {
			const std::size_t W = Walk.SparseWords[I];
			CollectWordDesc(W, PassBitWordMaskedAt(Walk.Bits, W, Walk.RowCount));
		}
		return;
	}
	const std::size_t Words = (Params.RowCount + 63) / 64;
	for(std::size_t W = Words; W-- > 0 && OutSortedRowIndices.size() < Keep;) {
		const std::uint64_t Word = PassBitWordMaskedAt(Walk.Bits, W, Walk.RowCount);
		if(Word != 0)
			CollectWordDesc(W, Word);
	}
}

void SelectTopKFromPassBits(const PassBitTopKParams &Params, std::vector<std::size_t> &OutSortedRowIndices) {
	OutSortedRowIndices.clear();
	if(!Params.Bits || Params.RowCount == 0 || Params.K == 0)
		return;
	if(!Params.Ascending && Params.Keys != nullptr) {
		SelectTopKFromPassBitsBucketDesc(Params, OutSortedRowIndices);
		return;
	}
	if(Params.Ascending && Params.Keys != nullptr) {
		SelectTopKFromPassBitsBucketAsc(Params, OutSortedRowIndices);
		return;
	}
	const bool ComputedRank = Params.Keys == nullptr && Params.PhysicalRankStep != 0;
	if(!Params.Ascending && ComputedRank) {
		SelectTopKPhysicalDescReverseWalk(Params, OutSortedRowIndices);
		return;
	}
	const std::size_t Keep = Params.K;
	const PassBitWordWalk Walk = PassBitWordWalkFromParams(Params);
	const bool ComputedRankTail = Params.Keys == nullptr && Params.PhysicalRankStep != 0;
	const auto KeyAt = [&](const std::size_t RowIndex) -> float {
		if(ComputedRankTail)
			return BulkSyntheticBioRankF32Row(Params.PhysicalRankStart +
			                                static_cast<int64_t>(RowIndex) * Params.PhysicalRankStep);
		if(Params.KeyFn)
			return Params.KeyFn(RowIndex);
		return 0.f;
	};
	PassBitTopKHeap<std::size_t> Heap;
	Heap.Reset(Keep, Params.Ascending);
	ForEachNonemptyPassWord(Walk, [&](const std::size_t W) {
		std::uint64_t Word = PassBitWordMaskedAt(Walk.Bits, W, Walk.RowCount);
		const std::size_t Base = W * 64;
		while(Word != 0) {
			const unsigned Bit = static_cast<unsigned>(std::countr_zero(Word));
			const std::size_t Row = Base + Bit;
			if(Row < Walk.RowCount)
				Heap.Consider(KeyAt(Row), Row);
			Word &= Word - 1;
		}
	});
	std::vector<typename PassBitTopKHeap<std::size_t>::Entry> Winners;
	Heap.ExtractSorted(Winners);
	OutSortedRowIndices.reserve(Winners.size());
	for(const auto &E : Winners)
		OutSortedRowIndices.push_back(E.Row);
}

void ScanPassBitsGroup(const std::uint64_t *Bits, const std::size_t GroupStart, const std::size_t GroupEnd,
                       const std::size_t RowCount, std::vector<std::size_t> &OutIndices) {
	if(!Bits || GroupEnd <= GroupStart)
		return;
	const std::size_t End = std::min(GroupEnd, RowCount);
	const std::size_t StartWord = GroupStart / 64;
	const std::size_t EndWord = (End + 63) / 64;
	for(std::size_t W = StartWord; W < EndWord; ++W) {
		std::uint64_t Word = Bits[W];
		if(Word == 0)
			continue;
		const std::size_t Base = W * 64;
		if(Base < GroupStart) {
			const std::size_t Skip = GroupStart - Base;
			Word &= ~((1ULL << Skip) - 1);
		}
		if(Base + 64 > End) {
			const std::size_t Keep = End - Base;
			if(Keep < 64)
				Word &= (1ULL << Keep) - 1;
		}
		AppendSetBitsFromWord(Word, Base, RowCount, OutIndices);
	}
}

namespace {

void AccumulateWordPrecomputed(std::uint64_t Word, std::size_t BaseRow, std::size_t RowCount, const std::uint8_t *SlotByRow,
                               const double *AmountByRow, WarehouseLutAggSlot *Slots) {
	while(Word != 0) {
		const unsigned Bit = static_cast<unsigned>(std::countr_zero(Word));
		const std::size_t Oi = BaseRow + Bit;
		if(Oi < RowCount) {
			const std::uint8_t S = SlotByRow[Oi];
			WarehouseLutAggSlot &G = Slots[S];
			++G.Cnt;
			G.Sum += AmountByRow[Oi];
		}
		Word &= Word - 1;
	}
}

void ScanPassGroupWords(const std::uint64_t *Bits, std::size_t GroupStart, std::size_t GroupEnd, std::size_t RowCount,
                        const std::uint8_t *SlotByRow, const double *AmountByRow, WarehouseLutAggSlot *Slots) {
	const std::size_t End = std::min(GroupEnd, RowCount);
	const std::size_t StartWord = GroupStart / 64;
	const std::size_t EndWord = (End + 63) / 64;
#if defined(__AVX2__)
	std::size_t W = StartWord;
	for(; W + 4 <= EndWord; W += 4) {
		const __m256i V = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(Bits + W));
		alignas(32) std::uint64_t Lane[4];
		_mm256_store_si256(reinterpret_cast<__m256i *>(Lane), V);
		for(int L = 0; L < 4; ++L) {
			if(Lane[L] == 0)
				continue;
			std::uint64_t Word = Lane[L];
			const std::size_t Base = (W + static_cast<std::size_t>(L)) * 64;
			if(Base < GroupStart) {
				const std::size_t Skip = GroupStart - Base;
				Word &= ~((1ULL << Skip) - 1);
			}
			if(Base + 64 > End) {
				const std::size_t Keep = End - Base;
				if(Keep < 64)
					Word &= (1ULL << Keep) - 1;
			}
			AccumulateWordPrecomputed(Word, Base, RowCount, SlotByRow, AmountByRow, Slots);
		}
	}
	for(; W < EndWord; ++W) {
		std::uint64_t Word = Bits[W];
		if(Word == 0)
			continue;
		const std::size_t Base = W * 64;
		if(Base < GroupStart) {
			const std::size_t Skip = GroupStart - Base;
			Word &= ~((1ULL << Skip) - 1);
		}
		if(Base + 64 > End) {
			const std::size_t Keep = End - Base;
			if(Keep < 64)
				Word &= (1ULL << Keep) - 1;
		}
		AccumulateWordPrecomputed(Word, Base, RowCount, SlotByRow, AmountByRow, Slots);
	}
#else
	for(std::size_t W = StartWord; W < EndWord; ++W) {
		std::uint64_t Word = Bits[W];
		if(Word == 0)
			continue;
		const std::size_t Base = W * 64;
		if(Base < GroupStart) {
			const std::size_t Skip = GroupStart - Base;
			Word &= ~((1ULL << Skip) - 1);
		}
		if(Base + 64 > End) {
			const std::size_t Keep = End - Base;
			if(Keep < 64)
				Word &= (1ULL << Keep) - 1;
		}
		AccumulateWordPrecomputed(Word, Base, RowCount, SlotByRow, AmountByRow, Slots);
	}
#endif
}

} // namespace

void FusedPassBitWarehouseLutAgg(const std::uint64_t *Bits, const std::uint32_t *GroupPassCounts,
                                 const std::size_t RowCount, const std::size_t GroupCount, const std::uint8_t *SlotByRow,
                                 const double *AmountByRow, WarehouseLutAggSlot *Slots, const std::size_t SlotCount) {
	if(!Bits || !SlotByRow || !AmountByRow || !Slots || RowCount == 0 || SlotCount == 0)
		return;
	if(GroupPassCounts && GroupCount > 0) {
		for(std::size_t Gi = 0; Gi < GroupCount; ++Gi) {
			if(GroupPassCounts[Gi] == 0)
				continue;
			const std::size_t GroupStart = Gi * kColumnRowGroupSize;
			const std::size_t GroupEnd = GroupStart + kColumnRowGroupSize;
			ScanPassGroupWords(Bits, GroupStart, GroupEnd, RowCount, SlotByRow, AmountByRow, Slots);
		}
		return;
	}
	const std::size_t Words = (RowCount + 63) / 64;
#if defined(__AVX2__)
	std::size_t W = 0;
	for(; W + 4 <= Words; W += 4) {
		const __m256i V = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(Bits + W));
		alignas(32) std::uint64_t Lane[4];
		_mm256_store_si256(reinterpret_cast<__m256i *>(Lane), V);
		for(int L = 0; L < 4; ++L) {
			if(Lane[L] != 0)
				AccumulateWordPrecomputed(Lane[L], (W + static_cast<std::size_t>(L)) * 64, RowCount, SlotByRow,
				                          AmountByRow, Slots);
		}
	}
	for(; W < Words; ++W) {
		if(Bits[W] != 0)
			AccumulateWordPrecomputed(Bits[W], W * 64, RowCount, SlotByRow, AmountByRow, Slots);
	}
#else
	for(std::size_t W = 0; W < Words; ++W) {
		if(Bits[W] != 0)
			AccumulateWordPrecomputed(Bits[W], W * 64, RowCount, SlotByRow, AmountByRow, Slots);
	}
#endif
}

void FusedPassBitWarehouseLutAggParallel(const std::uint64_t *Bits, const std::uint32_t *GroupPassCounts,
                                         const std::size_t RowCount, const std::size_t GroupCount,
                                         const std::uint8_t *SlotByRow, const double *AmountByRow,
                                         WarehouseLutAggSlot *Slots, const std::size_t SlotCount) {
	if(!Bits || !SlotByRow || !AmountByRow || !Slots || RowCount == 0 || SlotCount == 0)
		return;
	if(!GroupPassCounts || GroupCount == 0 || !JobSystem::Instance().IsRunning() || RowCount < 500'000) {
		FusedPassBitWarehouseLutAgg(Bits, GroupPassCounts, RowCount, GroupCount, SlotByRow, AmountByRow, Slots,
		                            SlotCount);
		return;
	}
	std::vector<std::size_t> ActiveGroups;
	ActiveGroups.reserve(GroupCount);
	for(std::size_t Gi = 0; Gi < GroupCount; ++Gi) {
		if(GroupPassCounts[Gi] != 0)
			ActiveGroups.push_back(Gi);
	}
	if(ActiveGroups.empty())
		return;
	std::vector<std::future<std::array<WarehouseLutAggSlot, BulkSyntheticWarehouseLutSlots>>> Futs;
	Futs.reserve(ActiveGroups.size());
	for(const std::size_t Gi : ActiveGroups) {
		Futs.push_back(JobSystem::Instance().SubmitAsync(
		    [Bits, GroupPassCounts, RowCount, GroupCount, SlotByRow, AmountByRow, Gi]() {
			    (void)GroupPassCounts;
			    (void)GroupCount;
			    std::array<WarehouseLutAggSlot, BulkSyntheticWarehouseLutSlots> Local{};
			    const std::size_t GroupStart = Gi * kColumnRowGroupSize;
			    const std::size_t GroupEnd = GroupStart + kColumnRowGroupSize;
			    ScanPassGroupWords(Bits, GroupStart, GroupEnd, RowCount, SlotByRow, AmountByRow, Local.data());
			    return Local;
		    }));
	}
	for(auto &F : Futs) {
		const auto Local = F.get();
		for(std::size_t I = 0; I < SlotCount && I < Local.size(); ++I) {
			Slots[I].Cnt += Local[I].Cnt;
			Slots[I].Sum += Local[I].Sum;
		}
	}
}

void FusedPassRowDirectAgg(const std::uint8_t *PassSlots, const double *PassAmounts, const std::size_t PassCount,
                           WarehouseLutAggSlot *Slots, const std::size_t SlotCount) {
	if(!PassSlots || !PassAmounts || !Slots || PassCount == 0 || SlotCount == 0)
		return;
	constexpr std::size_t kElemBytes = sizeof(std::uint8_t) + sizeof(double);
	const TiledCachePlan Plan = SimdTiling::ActivePlan(WorkloadClass::OlapScan, kElemBytes);
	const std::size_t Panel = SimdTiling::L1PanelElements(Plan, kElemBytes);
	SimdTileSession TileSession;
	SimdTiling::BeginTileScan(PassCount, WorkloadClass::OlapScan, kElemBytes, TileSession);
	for(std::size_t Begin = 0; Begin < PassCount; Begin += Panel) {
		const std::size_t End = (std::min)(Begin + Panel, PassCount);
		SimdTiling::AdvanceTilePanel(TileSession, Begin, End);
		if(TileSession.Armed) {
			SimdTiling::PrefetchStreamAhead(TileSession, PassAmounts, End, sizeof(double), End - Begin);
			if(TileSession.Prefetch.Armed)
				Superfetch::AdvanceColumnScan(TileSession.Prefetch, PassAmounts + Begin, Begin);
		}
		alignas(64) std::array<WarehouseLutAggSlot, BulkSyntheticWarehouseLutSlots> PanelSlots{};
		const std::size_t PanelCount = End - Begin;
		std::size_t I = 0;
#if defined(__AVX2__)
		const std::size_t Unroll = static_cast<std::size_t>(Plan.Unroll);
		for(; I + Unroll <= PanelCount; I += Unroll) {
			for(std::size_t J = 0; J < Unroll; ++J) {
				const std::size_t Idx = Begin + I + J;
				const std::uint8_t S = PassSlots[Idx];
				WarehouseLutAggSlot &G = PanelSlots[S];
				++G.Cnt;
				G.Sum += PassAmounts[Idx];
			}
		}
#endif
		for(; I < PanelCount; ++I) {
			const std::size_t Idx = Begin + I;
			const std::uint8_t S = PassSlots[Idx];
			WarehouseLutAggSlot &G = PanelSlots[S];
			++G.Cnt;
			G.Sum += PassAmounts[Idx];
		}
		for(std::size_t Si = 0; Si < SlotCount && Si < PanelSlots.size(); ++Si) {
			if(PanelSlots[Si].Cnt == 0)
				continue;
			Slots[Si].Cnt += PanelSlots[Si].Cnt;
			Slots[Si].Sum += PanelSlots[Si].Sum;
		}
	}
}

void FusedPassRowDirectAggParallel(const std::uint8_t *PassSlots, const double *PassAmounts, const std::size_t PassCount,
                                   WarehouseLutAggSlot *Slots, const std::size_t SlotCount) {
	if(!PassSlots || !PassAmounts || !Slots || PassCount == 0 || SlotCount == 0)
		return;
	if(!JobSystem::Instance().IsRunning() || PassCount < 64'000) {
		FusedPassRowDirectAgg(PassSlots, PassAmounts, PassCount, Slots, SlotCount);
		return;
	}
	const unsigned Workers = std::max(1u, JobWorkerCountFromEnv());
	const std::size_t Chunk = (PassCount + Workers - 1) / Workers;
	std::vector<std::future<std::array<WarehouseLutAggSlot, BulkSyntheticWarehouseLutSlots>>> Futs;
	Futs.reserve(Workers);
	for(unsigned W = 0; W < Workers; ++W) {
		const std::size_t Begin = static_cast<std::size_t>(W) * Chunk;
		if(Begin >= PassCount)
			break;
		const std::size_t End = std::min(Begin + Chunk, PassCount);
		Futs.push_back(JobSystem::Instance().SubmitAsync([PassSlots, PassAmounts, Begin, End]() {
			std::array<WarehouseLutAggSlot, BulkSyntheticWarehouseLutSlots> Local{};
			FusedPassRowDirectAgg(PassSlots + Begin, PassAmounts + Begin, End - Begin, Local.data(),
			                      BulkSyntheticWarehouseLutSlots);
			return Local;
		}));
	}
	for(auto &F : Futs) {
		const auto Local = F.get();
		for(std::size_t I = 0; I < SlotCount && I < Local.size(); ++I) {
			Slots[I].Cnt += Local[I].Cnt;
			Slots[I].Sum += Local[I].Sum;
		}
	}
}

} // namespace AstralDB
