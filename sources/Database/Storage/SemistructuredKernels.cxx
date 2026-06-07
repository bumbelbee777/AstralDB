#include <Database/Storage/SemistructuredKernels.hxx>

#include <Database/Storage/BulkSynthetic.hxx>
#include <Database/Storage/PassBitWalk.hxx>
#include <Database/Storage/BulkSyntheticSemistructured.hxx>
#include <DS/Regex.hxx>

using AstralDB::DS::Regex::Compiler;
using AstralDB::DS::Regex::Program;
using AstralDB::DS::Regex::Search;
#include <DS/SimdJsonExtract.hxx>
#include <DS/SimdXmlExtract.hxx>

#include <algorithm>
#include <cstring>
#include <queue>

#include <IO/SIMD.hxx>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace AstralDB {

namespace {

bool SqlTruthLiteral(std::string_view S) noexcept {
	return S == "1" || S == "true" || S == "TRUE" || S == "t" || S == "yes";
}

std::size_t FirstPatternChar(std::string_view Pattern) noexcept {
	for(char C : Pattern) {
		if(C == '^' || C == '$' || C == '(' || C == '[' || C == '.' || C == '\\')
			continue;
		return static_cast<std::size_t>(C);
	}
	return static_cast<std::size_t>(Pattern.empty() ? '\0' : Pattern[0]);
}

#if defined(__AVX2__)
bool TextContainsCharAvx2(std::string_view Text, char Needle) noexcept {
	const __m256i N = _mm256_set1_epi8(Needle);
	std::size_t I = 0;
	for(; I + 32 <= Text.size(); I += 32) {
		const __m256i V = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(Text.data() + I));
		const int Mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(V, N));
		if(Mask != 0)
			return true;
	}
	for(; I < Text.size(); ++I) {
		if(Text[I] == Needle)
			return true;
	}
	return false;
}
#elif defined(__ARM_NEON) || defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
bool TextContainsCharNeon(std::string_view Text, char Needle) noexcept {
	const uint8x16_t N = vdupq_n_u8(static_cast<uint8_t>(Needle));
	std::size_t I = 0;
	for(; I + 16 <= Text.size(); I += 16) {
		const uint8x16_t V = vld1q_u8(reinterpret_cast<const uint8_t *>(Text.data() + I));
		if(Simd::NeonMovemaskEq(vceqq_u8(V, N)) != 0)
			return true;
	}
	for(; I < Text.size(); ++I) {
		if(Text[I] == Needle)
			return true;
	}
	return false;
}
#endif

} // namespace

void JsonBatchKernel::ExtractBatch(const char *const *JsonStrings, const std::size_t *Lengths, const char *Path,
                                   const std::size_t N, std::uint8_t *OutputMask, std::string *OutputValues) {
	const std::string_view PathView(Path);
	for(std::size_t I = 0; I < N; ++I) {
		const std::string_view Json(JsonStrings[I], Lengths[I]);
		std::string Out;
		const bool Ok = SimdJsonExtract::Extract(Json, PathView, Out);
		OutputMask[I] = Ok ? 1 : 0;
		if(OutputValues)
			OutputValues[I] = std::move(Out);
	}
}

void JsonBatchKernel::MatchEqBatch(const char *const *JsonStrings, const std::size_t *Lengths, const char *Path,
                                   const char *Expected, const std::size_t N, std::uint8_t *OutputMask) {
	const std::string_view PathView(Path);
	const std::string_view Want(Expected);
	const bool WantTruth = SqlTruthLiteral(Want);
	for(std::size_t I = 0; I < N; ++I) {
		const std::string_view Json(JsonStrings[I], Lengths[I]);
		std::string Got;
		if(!SimdJsonExtract::Extract(Json, PathView, Got)) {
			OutputMask[I] = 0;
			continue;
		}
		const bool Hit = Got == Want || (WantTruth && SqlTruthLiteral(Got));
		OutputMask[I] = Hit ? 1 : 0;
	}
}

void XmlBatchKernel::ExtractBatch(const char *const *XmlStrings, const std::size_t *Lengths, const char *Xpath,
                                  const std::size_t N, std::uint8_t *OutputMask, std::string *OutputValues) {
	const std::string_view PathView(Xpath);
	for(std::size_t I = 0; I < N; ++I) {
		const std::string_view Xml(XmlStrings[I], Lengths[I]);
		std::string Out;
		const bool Ok = SimdXmlExtract::Extract(Xml, PathView, Out);
		OutputMask[I] = Ok ? 1 : 0;
		if(OutputValues)
			OutputValues[I] = std::move(Out);
	}
}

void XmlBatchKernel::ValidBatch(const char *const *XmlStrings, const std::size_t *Lengths, const std::size_t N,
                                std::uint8_t *OutputMask) {
	for(std::size_t I = 0; I < N; ++I) {
		const std::string_view Xml(XmlStrings[I], Lengths[I]);
		OutputMask[I] = SimdXmlExtract::Valid(Xml) ? 1 : 0;
	}
}

void RegexBatchKernel::MatchBatch(const char *const *Texts, const std::size_t *Lengths, const char *Pattern,
                                  const std::size_t N, std::uint8_t *OutputMask) {
	const std::string Pat(Pattern);
	const std::size_t First = FirstPatternChar(Pat);
	const std::optional<Program> Re = Compiler::Compile(Pat);
	if(!Re)
		return;
	for(std::size_t I = 0; I < N; ++I) {
		const std::string_view Text(Texts[I], Lengths[I]);
#if defined(__AVX2__)
		if(First < 256 && !TextContainsCharAvx2(Text, static_cast<char>(First))) {
			OutputMask[I] = 0;
			continue;
		}
#elif defined(__ARM_NEON) || defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
		if(First < 256 && !TextContainsCharNeon(Text, static_cast<char>(First))) {
			OutputMask[I] = 0;
			continue;
		}
#else
		if(First < 256 && Text.find(static_cast<char>(First)) == std::string_view::npos) {
			OutputMask[I] = 0;
			continue;
		}
#endif
		OutputMask[I] = Search(*Re, Text) ? 1 : 0;
	}
}

void FtsBatchKernel::MatchBatch(const std::uint32_t *KeywordMasks, const std::uint32_t RequiredMask, const std::size_t N,
                                std::uint8_t *OutputMask) {
	std::size_t I = 0;
#if defined(__AVX2__)
	const __m256i Req = _mm256_set1_epi32(static_cast<int>(RequiredMask));
	for(; I + 8 <= N; I += 8) {
		const __m256i Row = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(KeywordMasks + I));
		const __m256i Hit = _mm256_and_si256(Row, Req);
		const __m256i Eq = _mm256_cmpeq_epi32(Hit, Req);
		const int Mask = _mm256_movemask_ps(_mm256_castsi256_ps(Eq));
		for(unsigned J = 0; J < 8; ++J)
			OutputMask[I + J] = (Mask & (1 << J)) ? 1 : 0;
	}
#endif
	for(; I < N; ++I)
		OutputMask[I] = (KeywordMasks[I] & RequiredMask) == RequiredMask ? 1 : 0;
}

void RankBatchKernel::FillBatch(const float *Ranks, const std::size_t N, float *Output) {
	std::size_t I = 0;
#if defined(__AVX2__)
	for(; I + 8 <= N; I += 8)
		_mm256_storeu_ps(Output + I, _mm256_loadu_ps(Ranks + I));
#endif
	for(; I < N; ++I)
		Output[I] = Ranks[I];
}

void TopKBatchKernel::InsertBatch(const float *Ranks, const std::uint64_t *Ids, const std::size_t N, const std::size_t K,
                                  const bool Ascending, std::vector<std::pair<float, std::uint64_t>> &Heap) {
	if(K == 0)
		return;
	PassBitTopKHeap<std::uint64_t> H;
	H.Reset(K, Ascending);
	for(const auto &E : Heap)
		H.Consider(E.first, E.second);
	for(std::size_t I = 0; I < N; ++I)
		H.Consider(Ranks[I], Ids[I]);
	std::vector<typename PassBitTopKHeap<std::uint64_t>::Entry> Sorted;
	H.ExtractSorted(Sorted);
	Heap.clear();
	Heap.reserve(Sorted.size());
	for(const auto &E : Sorted)
		Heap.emplace_back(E.Key, E.Row);
}

void TopKBatchKernel::PartialSortBatch(const float *Ranks, const std::uint64_t *Ids, const std::size_t N,
                                       const std::size_t K, std::vector<std::pair<float, std::uint64_t>> &Output) {
	Output.clear();
	if(N == 0 || K == 0)
		return;
	const std::size_t Keep = std::min(K, N);
	std::vector<std::size_t> Ord(N);
	for(std::size_t I = 0; I < N; ++I)
		Ord[I] = I;
	const auto Less = [&](std::size_t A, std::size_t B) { return Ranks[A] > Ranks[B]; };
	if(N > Keep)
		std::partial_sort(Ord.begin(), Ord.begin() + static_cast<std::ptrdiff_t>(Keep), Ord.end(), Less);
	else
		std::sort(Ord.begin(), Ord.end(), Less);
	Output.reserve(Keep);
	for(std::size_t I = 0; I < Keep; ++I)
		Output.emplace_back(Ranks[Ord[I]], Ids[Ord[I]]);
}

} // namespace AstralDB
