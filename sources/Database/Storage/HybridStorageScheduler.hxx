#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace AstralDB {

enum class StorageLayout : unsigned { Row = 0, Columnar = 1, Hybrid = 2, Auto = 3 };

StorageLayout StorageLayoutFromKeyword(std::string_view Keyword);
const char *StorageLayoutKeyword(StorageLayout Layout);

struct TableWorkloadCounters {
	uint64_t ReadCount = 0;
	uint64_t WriteCount = 0;
	uint64_t AggregateReadCount = 0;
	std::size_t RowCountPeak = 0;
	float CardinalityEstimate = 0.f;
};

struct WorkloadFeatures {
	float ReadWriteRatio = 0.5f;
	float QueryPatternScore = 0.f;
	float DataSizeNorm = 0.f;
	float CardinalityNorm = 0.f;
	float AccessFrequencyNorm = 0.f;
};

/** Small MLP that learns row vs columnar vs hybrid layout from table access counters. */
class HybridStorageScheduler {
public:
	static constexpr std::size_t kStorageMlpInputDim = 5;
	static constexpr std::size_t kStorageMlpHiddenDim = 8;
	static constexpr std::size_t kStorageMlpOutputDim = 3;
	static constexpr std::size_t ColumnarGroupByRowThreshold = 32;

	HybridStorageScheduler();

	WorkloadFeatures BuildFeatures(const TableWorkloadCounters &Counters) const;
	StorageLayout PredictLayout(const WorkloadFeatures &Features) const;
	float EstimateCost(StorageLayout Layout, const WorkloadFeatures &Features) const;
	void ObserveOutcome(const WorkloadFeatures &Features, StorageLayout UsedLayout, float ObservedCost);
	std::string SerializeWeights() const;
	bool DeserializeWeights(std::string_view Blob);

	static bool PreferColumnarGroupBy(std::size_t RowCount, int64_t AggMode, std::size_t NumNumericAggs) {
		return RowCount >= ColumnarGroupByRowThreshold && AggMode == 3 && NumNumericAggs > 0;
	}

private:
	static float Sigmoid(float X);
	static float Relu(float X);
	void Forward(const float Input[kStorageMlpInputDim], float Hidden[kStorageMlpHiddenDim],
	             float Output[kStorageMlpOutputDim]) const;
	void TrainStep(const WorkloadFeatures &Features, int TargetClass, float ObservedCost);

	float Weights1_[kStorageMlpInputDim * kStorageMlpHiddenDim];
	float Bias1_[kStorageMlpHiddenDim];
	float Weights2_[kStorageMlpHiddenDim * kStorageMlpOutputDim];
	float Bias2_[kStorageMlpOutputDim];
	uint64_t TrainSteps_ = 0;
};

} // namespace AstralDB
