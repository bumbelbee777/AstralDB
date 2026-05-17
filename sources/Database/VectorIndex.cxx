#include <Database/VectorIndex.hxx>

#include <Database/AdvancedTypes.hxx>
#include <IO/SIMD.hxx>

#include <algorithm>
#include <cmath>
#include <limits>

namespace AstralDB {
namespace {

double L2Distance(const std::vector<double> &A, const std::vector<double> &B) {
	if(A.size() != B.size())
		return std::numeric_limits<double>::infinity();
	double Acc = 0.0;
	for(size_t I = 0; I < A.size(); ++I) {
		const double D = A[I] - B[I];
		Acc += D * D;
	}
	return std::sqrt(Acc);
}

double CosineDistance(const std::vector<double> &A, const std::vector<double> &B) {
	if(A.size() != B.size() || A.empty())
		return std::numeric_limits<double>::infinity();
	std::vector<float> Af(A.begin(), A.end());
	std::vector<float> Bf(B.begin(), B.end());
	const float Dot = Simd::DotProductF32(Af.data(), Bf.data(), Af.size());
	double Na = 0.0;
	double Nb = 0.0;
	for(double X : A)
		Na += X * X;
	for(double X : B)
		Nb += X * X;
	if(Na == 0.0 || Nb == 0.0)
		return std::numeric_limits<double>::infinity();
	const double Sim = static_cast<double>(Dot) / (std::sqrt(Na) * std::sqrt(Nb));
	return 1.0 - Sim;
}

} // namespace

void VectorIndex::Clear() {
	RowIds_.clear();
	Vectors_.clear();
}

void VectorIndex::BuildFromColumn(const std::vector<std::unordered_map<std::string, std::string>> &Rows,
                                  const std::string &Column) {
	Clear();
	RowIds_.reserve(Rows.size());
	Vectors_.reserve(Rows.size());
	for(size_t I = 0; I < Rows.size(); ++I) {
		auto It = Rows[I].find(Column);
		if(It == Rows[I].end())
			continue;
		const auto Vec = AdvancedTypes::ParseVectorCell(It->second);
		if(!Vec)
			continue;
		RowIds_.push_back(I);
		Vectors_.push_back(*Vec);
	}
}

void VectorIndex::UpsertRow(size_t RowId, const std::vector<double> &Vec) {
	for(size_t I = 0; I < RowIds_.size(); ++I) {
		if(RowIds_[I] == RowId) {
			Vectors_[I] = Vec;
			return;
		}
	}
	RowIds_.push_back(RowId);
	Vectors_.push_back(Vec);
}

void VectorIndex::RemoveRow(size_t RowId) {
	for(size_t I = 0; I < RowIds_.size(); ++I) {
		if(RowIds_[I] == RowId) {
			RowIds_.erase(RowIds_.begin() + static_cast<std::ptrdiff_t>(I));
			Vectors_.erase(Vectors_.begin() + static_cast<std::ptrdiff_t>(I));
			return;
		}
	}
}

std::vector<size_t> VectorIndex::TopK(const std::vector<double> &Query, std::size_t K) const {
	struct Scored {
		size_t RowId;
		double Score;
	};
	std::vector<Scored> Ranked;
	Ranked.reserve(RowIds_.size());
	for(size_t I = 0; I < RowIds_.size(); ++I) {
		const double Dist = Metric_ == VectorMetric::Cosine ? CosineDistance(Vectors_[I], Query) :
		                                                      L2Distance(Vectors_[I], Query);
		Ranked.push_back({RowIds_[I], Dist});
	}
	const std::size_t Take = std::min(K, Ranked.size());
	std::partial_sort(Ranked.begin(), Ranked.begin() + static_cast<std::ptrdiff_t>(Take), Ranked.end(),
	                  [](const Scored &A, const Scored &B) { return A.Score < B.Score; });
	std::vector<size_t> Out;
	Out.reserve(Take);
	for(std::size_t I = 0; I < Take; ++I)
		Out.push_back(Ranked[I].RowId);
	return Out;
}

} // namespace AstralDB
