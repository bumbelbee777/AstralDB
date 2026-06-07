#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace AstralDB {

/** Welford online variance for STDDEV_POP / STDDEV_SAMP. */
struct WelfordState {
	double Mean_ = 0.0;
	double M2_ = 0.0;
	int64_t Count_ = 0;

	void Add(double X) {
		++Count_;
		const double Delta = X - Mean_;
		Mean_ += Delta / static_cast<double>(Count_);
		const double Delta2 = X - Mean_;
		M2_ += Delta * Delta2;
	}

	void Merge(const WelfordState &Other) {
		if(Other.Count_ == 0)
			return;
		if(Count_ == 0) {
			*this = Other;
			return;
		}
		const int64_t Total = Count_ + Other.Count_;
		const double Delta = Other.Mean_ - Mean_;
		Mean_ = Mean_ + Delta * static_cast<double>(Other.Count_) / static_cast<double>(Total);
		M2_ = M2_ + Other.M2_ + Delta * Delta * static_cast<double>(Count_ * Other.Count_) / static_cast<double>(Total);
		Count_ = Total;
	}

	double StdDevPop() const {
		if(Count_ <= 0)
			return 0.0;
		return std::sqrt(M2_ / static_cast<double>(Count_));
	}

	double StdDevSamp() const {
		if(Count_ <= 1)
			return 0.0;
		return std::sqrt(M2_ / static_cast<double>(Count_ - 1));
	}
};

/** Simplified mergeable t-digest for APPROX_QUANTILE / MEDIAN. */
struct TDigestState {
	static constexpr std::size_t kMaxCentroids = 128;
	struct Centroid {
		double Mean = 0.0;
		double Weight = 0.0;
	};
	std::vector<Centroid> Centroids_;
	double TotalWeight_ = 0.0;

	void Add(double X, double Weight = 1.0) {
		if(Weight <= 0.0)
			return;
		TotalWeight_ += Weight;
		if(Centroids_.empty()) {
			Centroids_.push_back({X, Weight});
			return;
		}
		for(auto &C : Centroids_) {
			if(std::abs(C.Mean - X) < 1e-9) {
				const double W = C.Weight + Weight;
				C.Mean = (C.Mean * C.Weight + X * Weight) / W;
				C.Weight = W;
				CompressIfNeeded();
				return;
			}
		}
		Centroids_.push_back({X, Weight});
		CompressIfNeeded();
	}

	void Merge(const TDigestState &Other) {
		for(const auto &C : Other.Centroids_)
			Add(C.Mean, C.Weight);
	}

	double Quantile(double Q) const {
		if(Centroids_.empty() || TotalWeight_ <= 0.0)
			return 0.0;
		Q = std::clamp(Q, 0.0, 1.0);
		auto Sorted = Centroids_;
		std::sort(Sorted.begin(), Sorted.end(),
		          [](const Centroid &A, const Centroid &B) { return A.Mean < B.Mean; });
		const double Target = Q * TotalWeight_;
		double Acc = 0.0;
		for(const auto &C : Sorted) {
			Acc += C.Weight;
			if(Acc >= Target)
				return C.Mean;
		}
		return Sorted.back().Mean;
	}

	double Median() const { return Quantile(0.5); }

private:
	void CompressIfNeeded() {
		if(Centroids_.size() <= kMaxCentroids)
			return;
		std::sort(Centroids_.begin(), Centroids_.end(),
		          [](const Centroid &A, const Centroid &B) { return A.Mean < B.Mean; });
		std::vector<Centroid> Out;
		Out.reserve(kMaxCentroids / 2);
		double BucketW = TotalWeight_ / static_cast<double>(kMaxCentroids / 2);
		double CurMean = 0.0;
		double CurW = 0.0;
		for(const auto &C : Centroids_) {
			if(CurW + C.Weight <= BucketW || Out.empty()) {
				const double W = CurW + C.Weight;
				CurMean = CurW > 0.0 ? (CurMean * CurW + C.Mean * C.Weight) / W : C.Mean;
				CurW = W;
			} else {
				Out.push_back({CurMean, CurW});
				CurMean = C.Mean;
				CurW = C.Weight;
			}
		}
		if(CurW > 0.0)
			Out.push_back({CurMean, CurW});
		Centroids_ = std::move(Out);
	}
};

/** Frequency sketch for MODE. */
struct FreqSketchState {
	std::unordered_map<std::string, int64_t> Counts_;
	std::string BestValue_;
	int64_t BestCount_ = 0;

	void Add(const std::string &Value) {
		const int64_t N = ++Counts_[Value];
		if(N > BestCount_ || (N == BestCount_ && Value < BestValue_)) {
			BestCount_ = N;
			BestValue_ = Value;
		}
	}

	void Merge(const FreqSketchState &Other) {
		for(const auto &[K, V] : Other.Counts_) {
			const int64_t N = Counts_[K] + V;
			Counts_[K] = N;
			if(N > BestCount_ || (N == BestCount_ && K < BestValue_)) {
				BestCount_ = N;
				BestValue_ = K;
			}
		}
	}

	const std::string &Mode() const { return BestValue_; }
};

} // namespace AstralDB
