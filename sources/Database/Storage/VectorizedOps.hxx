#pragma once

#include <Database/Storage/ColumnFilterSimd.hxx>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace AstralDB {

inline constexpr std::size_t kDefaultVecBatchSize = 1024;

/** Tunable batch size (\c ASTRALDB_VEC_BATCH_SIZE, default 1024). */
std::size_t VectorBatchSizeFromEnv() noexcept;

enum class ColumnVectorKind : uint8_t { I64, String, Null };

/** Typed column payload for vectorized operators. */
struct ColumnVector {
	ColumnVectorKind Kind = ColumnVectorKind::Null;
	std::vector<int64_t> I64;
	std::vector<std::string> Strings;
	std::vector<std::size_t> RowIndices;
	std::size_t Size = 0;
};

/** Batch of column vectors processed together. */
struct VectorBatch {
	std::vector<std::unique_ptr<ColumnVector>> Columns;
	int Size = 0;
};

/** Base class for vectorized pipeline operators. */
class VectorizedOperator {
public:
	virtual ~VectorizedOperator() = default;
	virtual bool Execute(VectorBatch &In, VectorBatch &Out) = 0;
};

/** SIMD filter on an i64 column; writes matching positions into \p OutMask (1 = keep). */
class VectorizedFilter {
public:
	VectorizedFilter(const int64_t *Values, std::size_t Count, FilterCompareOp Op, int64_t Literal);

	void ExecuteMask(std::vector<uint8_t> &OutMask) const;
	void ExecuteIndices(std::vector<std::size_t> &OutIndices) const;

private:
	const int64_t *Values_;
	std::size_t Count_;
	FilterCompareOp Op_;
	int64_t Literal_;
};

/** Extract a subset of columns from a batch by index list. */
class VectorizedProject {
public:
	explicit VectorizedProject(std::vector<std::size_t> ColumnIndices);

	bool Execute(VectorBatch &In, VectorBatch &Out);

private:
	std::vector<std::size_t> ColumnIndices_;
};

/** Hash-probe keys in batches (SIMD hash + equality). */
struct HashProbeResult {
	std::vector<std::size_t> ProbeIndices;
	std::vector<std::size_t> BuildIndices;
};

class VectorizedHashProbe {
public:
	VectorizedHashProbe(const int64_t *ProbeKeys, std::size_t ProbeCount,
	                    const std::vector<std::vector<std::size_t>> &BuildBuckets, std::size_t BuildCount);

	void Execute(HashProbeResult &Out) const;

private:
	const int64_t *ProbeKeys_;
	std::size_t ProbeCount_;
	const std::vector<std::vector<std::size_t>> *BuildBuckets_;
	std::size_t BuildCount_;
};

/** Build i64 column batch from row indices + string column (parse to i64). */
void BuildI64ColumnBatch(const std::vector<std::string> &Cells, const std::vector<std::size_t> &RowIndices,
                         std::size_t Begin, std::size_t End, ColumnVector &Out);

} // namespace AstralDB
