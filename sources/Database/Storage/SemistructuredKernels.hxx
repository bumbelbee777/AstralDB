#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace AstralDB {

class JsonBatchKernel {
public:
	static void ExtractBatch(const char *const *JsonStrings, const std::size_t *Lengths, const char *Path, std::size_t N,
	                         std::uint8_t *OutputMask, std::string *OutputValues);

	static void MatchEqBatch(const char *const *JsonStrings, const std::size_t *Lengths, const char *Path,
	                         const char *Expected, std::size_t N, std::uint8_t *OutputMask);
};

class XmlBatchKernel {
public:
	static void ExtractBatch(const char *const *XmlStrings, const std::size_t *Lengths, const char *Xpath, std::size_t N,
	                         std::uint8_t *OutputMask, std::string *OutputValues);

	static void ValidBatch(const char *const *XmlStrings, const std::size_t *Lengths, std::size_t N,
	                       std::uint8_t *OutputMask);
};

class RegexBatchKernel {
public:
	static void MatchBatch(const char *const *Texts, const std::size_t *Lengths, const char *Pattern, std::size_t N,
	                       std::uint8_t *OutputMask);
};

class FtsBatchKernel {
public:
	static void MatchBatch(const std::uint32_t *KeywordMasks, std::uint32_t RequiredMask, std::size_t N,
	                       std::uint8_t *OutputMask);
};

class RankBatchKernel {
public:
	static void FillBatch(const float *Ranks, std::size_t N, float *Output);
};

class TopKBatchKernel {
public:
	static void InsertBatch(const float *Ranks, const std::uint64_t *Ids, std::size_t N, std::size_t K, bool Ascending,
	                        std::vector<std::pair<float, std::uint64_t>> &Heap);

	static void PartialSortBatch(const float *Ranks, const std::uint64_t *Ids, std::size_t N, std::size_t K,
	                             std::vector<std::pair<float, std::uint64_t>> &Output);
};

} // namespace AstralDB
