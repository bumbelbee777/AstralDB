#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace AstralDB {

enum class VectorMetric : int8_t { L2 = 0, Cosine = 1 };

/** Flat row-id + vector store with brute-force top-\a K (SIMD dot for cosine). */
class VectorIndex {
	VectorMetric Metric_ = VectorMetric::Cosine;
	std::vector<size_t> RowIds_;
	std::vector<std::vector<double>> Vectors_;
	std::vector<std::vector<float>> FloatVectors_;
	std::vector<float> Norms_;

public:
	void Clear();
	void SetMetric(VectorMetric M) { Metric_ = M; }
	VectorMetric Metric() const { return Metric_; }

	void BuildFromColumn(const std::vector<std::unordered_map<std::string, std::string>> &Rows,
	                     const std::string &Column);

	void UpsertRow(size_t RowId, const std::vector<double> &Vec);
	void RemoveRow(size_t RowId);

	/** Row ids ordered by best distance/similarity first (at most \a K). */
	std::vector<size_t> TopK(const std::vector<double> &Query, std::size_t K) const;
};

} // namespace AstralDB
