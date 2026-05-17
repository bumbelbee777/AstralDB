#include <Database/HybridStorageScheduler.hxx>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <sstream>

namespace AstralDB {

namespace {

constexpr float kLearnRate = 0.05f;
constexpr float kCostBlend = 0.15f;

float Clamp01(float X) {
	if(X < 0.f)
		return 0.f;
	if(X > 1.f)
		return 1.f;
	return X;
}

int LayoutClass(StorageLayout Layout) {
	switch(Layout) {
	case StorageLayout::Row:
		return 0;
	case StorageLayout::Columnar:
		return 1;
	case StorageLayout::Hybrid:
		return 2;
	default:
		return 0;
	}
}

} // namespace

HybridStorageScheduler::HybridStorageScheduler() {
	std::memset(Weights1_, 0, sizeof(Weights1_));
	std::memset(Bias1_, 0, sizeof(Bias1_));
	std::memset(Weights2_, 0, sizeof(Weights2_));
	std::memset(Bias2_, 0, sizeof(Bias2_));
	for(size_t I = 0; I < kStorageMlpInputDim * kStorageMlpHiddenDim; ++I)
		Weights1_[I] = 0.02f * static_cast<float>((I % 7) - 3);
	for(size_t H = 0; H < kStorageMlpHiddenDim; ++H)
		Bias1_[H] = 0.01f;
	for(size_t I = 0; I < kStorageMlpHiddenDim * kStorageMlpOutputDim; ++I)
		Weights2_[I] = 0.02f * static_cast<float>((I % 5) - 2);
}

float HybridStorageScheduler::Sigmoid(float X) {
	return 1.f / (1.f + std::exp(-X));
}

float HybridStorageScheduler::Relu(float X) {
	return X > 0.f ? X : 0.f;
}

void HybridStorageScheduler::Forward(const float Input[kStorageMlpInputDim], float Hidden[kStorageMlpHiddenDim],
                                    float Output[kStorageMlpOutputDim]) const {
	for(size_t H = 0; H < kStorageMlpHiddenDim; ++H) {
		float Sum = Bias1_[H];
		for(size_t I = 0; I < kStorageMlpInputDim; ++I)
			Sum += Input[I] * Weights1_[I * kStorageMlpHiddenDim + H];
		Hidden[H] = Relu(Sum);
	}
	for(size_t O = 0; O < kStorageMlpOutputDim; ++O) {
		float Sum = Bias2_[O];
		for(size_t H = 0; H < kStorageMlpHiddenDim; ++H)
			Sum += Hidden[H] * Weights2_[H * kStorageMlpOutputDim + O];
		Output[O] = Sum;
	}
	float MaxV = Output[0];
	for(size_t O = 1; O < kStorageMlpOutputDim; ++O)
		MaxV = std::max(MaxV, Output[O]);
	float ExpSum = 0.f;
	for(size_t O = 0; O < kStorageMlpOutputDim; ++O) {
		Output[O] = std::exp(Output[O] - MaxV);
		ExpSum += Output[O];
	}
	if(ExpSum > 0.f) {
		for(size_t O = 0; O < kStorageMlpOutputDim; ++O)
			Output[O] /= ExpSum;
	}
}

WorkloadFeatures HybridStorageScheduler::BuildFeatures(const TableWorkloadCounters &Counters) const {
	WorkloadFeatures F;
	const uint64_t TotalOps = Counters.ReadCount + Counters.WriteCount;
	if(TotalOps == 0)
		F.ReadWriteRatio = 0.5f;
	else
		F.ReadWriteRatio = static_cast<float>(Counters.ReadCount) / static_cast<float>(TotalOps);
	F.QueryPatternScore =
	    TotalOps == 0 ? 0.f
	                  : static_cast<float>(Counters.AggregateReadCount) / static_cast<float>(Counters.ReadCount + 1);
	F.DataSizeNorm = Clamp01(static_cast<float>(Counters.RowCountPeak) / 100000.f);
	F.CardinalityNorm = Clamp01(Counters.CardinalityEstimate);
	F.AccessFrequencyNorm = Clamp01(std::log1p(static_cast<float>(TotalOps)) / 12.f);
	return F;
}

StorageLayout HybridStorageScheduler::PredictLayout(const WorkloadFeatures &Features) const {
	float Input[kStorageMlpInputDim] = {Features.ReadWriteRatio, Features.QueryPatternScore, Features.DataSizeNorm,
	                                    Features.CardinalityNorm, Features.AccessFrequencyNorm};
	float Hidden[kStorageMlpHiddenDim];
	float Output[kStorageMlpOutputDim];
	Forward(Input, Hidden, Output);
	int Best = 0;
	for(int O = 1; O < static_cast<int>(kStorageMlpOutputDim); ++O)
		if(Output[O] > Output[Best])
			Best = O;
	switch(Best) {
	case 1:
		return StorageLayout::Columnar;
	case 2:
		return StorageLayout::Hybrid;
	default:
		return StorageLayout::Row;
	}
}

float HybridStorageScheduler::EstimateCost(StorageLayout Layout, const WorkloadFeatures &Features) const {
	const int Cls = LayoutClass(Layout);
	float Input[kStorageMlpInputDim] = {Features.ReadWriteRatio, Features.QueryPatternScore, Features.DataSizeNorm,
	                                    Features.CardinalityNorm, Features.AccessFrequencyNorm};
	float Hidden[kStorageMlpHiddenDim];
	float Output[kStorageMlpOutputDim];
	Forward(Input, Hidden, Output);
	const float P = Output[Cls];
	return (1.f - P) + Features.DataSizeNorm * 0.1f + (1.f - Features.ReadWriteRatio) * 0.05f;
}

void HybridStorageScheduler::TrainStep(const WorkloadFeatures &Features, int TargetClass, float ObservedCost) {
	float Input[kStorageMlpInputDim] = {Features.ReadWriteRatio, Features.QueryPatternScore, Features.DataSizeNorm,
	                                    Features.CardinalityNorm, Features.AccessFrequencyNorm};
	float Hidden[kStorageMlpHiddenDim];
	float Output[kStorageMlpOutputDim];
	Forward(Input, Hidden, Output);
	const float Err = ObservedCost * kCostBlend;
	for(size_t O = 0; O < kStorageMlpOutputDim; ++O) {
		const float Grad = (O == static_cast<size_t>(TargetClass) ? Output[O] - 1.f : Output[O]) * Err * kLearnRate;
		for(size_t H = 0; H < kStorageMlpHiddenDim; ++H)
			Weights2_[H * kStorageMlpOutputDim + O] -= Grad * Hidden[H];
		Bias2_[O] -= Grad;
	}
	for(size_t H = 0; H < kStorageMlpHiddenDim; ++H) {
		float Dh = 0.f;
		for(size_t O = 0; O < kStorageMlpOutputDim; ++O) {
			const float Grad = (O == static_cast<size_t>(TargetClass) ? Output[O] - 1.f : Output[O]) * Err * kLearnRate;
			Dh += Grad * Weights2_[H * kStorageMlpOutputDim + O];
		}
		if(Hidden[H] <= 0.f)
			continue;
		for(size_t I = 0; I < kStorageMlpInputDim; ++I)
			Weights1_[I * kStorageMlpHiddenDim + H] -= Dh * Input[I] * kLearnRate;
		Bias1_[H] -= Dh * kLearnRate;
	}
	++TrainSteps_;
}

void HybridStorageScheduler::ObserveOutcome(const WorkloadFeatures &Features, StorageLayout UsedLayout,
                                            float ObservedCost) {
	TrainStep(Features, LayoutClass(UsedLayout), ObservedCost);
}

std::string HybridStorageScheduler::SerializeWeights() const {
	std::ostringstream O;
	O << TrainSteps_ << ' ';
	for(float W : Weights1_)
		O << W << ' ';
	for(float B : Bias1_)
		O << B << ' ';
	for(float W : Weights2_)
		O << W << ' ';
	for(float B : Bias2_)
		O << B << ' ';
	return O.str();
}

bool HybridStorageScheduler::DeserializeWeights(std::string_view Blob) {
	std::istringstream WeightStream; WeightStream.str(std::string(Blob));
	if(!(WeightStream >> TrainSteps_))
		return false;
	for(float &W : Weights1_) {
		if(!(WeightStream >> W))
			return false;
	}
	for(float &B : Bias1_) {
		if(!(WeightStream >> B))
			return false;
	}
	for(float &W : Weights2_) {
		if(!(WeightStream >> W))
			return false;
	}
	for(float &B : Bias2_) {
		if(!(WeightStream >> B))
			return false;
	}
	return true;
}

StorageLayout StorageLayoutFromKeyword(std::string_view Keyword) {
	std::string U(Keyword);
	for(char &C : U)
		C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
	if(U == "ROW")
		return StorageLayout::Row;
	if(U == "COLUMNAR")
		return StorageLayout::Columnar;
	if(U == "HYBRID")
		return StorageLayout::Hybrid;
	if(U == "AUTO")
		return StorageLayout::Auto;
	return StorageLayout::Row;
}

const char *StorageLayoutKeyword(StorageLayout Layout) {
	switch(Layout) {
	case StorageLayout::Row:
		return "ROW";
	case StorageLayout::Columnar:
		return "COLUMNAR";
	case StorageLayout::Hybrid:
		return "HYBRID";
	case StorageLayout::Auto:
		return "AUTO";
	}
	return "ROW";
}

} // namespace AstralDB
