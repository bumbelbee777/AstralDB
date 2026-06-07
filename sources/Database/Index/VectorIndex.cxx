#include <Database/Index/VectorIndex.hxx>

#include <Database/Types/AdvancedTypes.hxx>
#include <IO/SIMD.hxx>

#include <algorithm>
#include <cmath>
#include <limits>

namespace AstralDB {
namespace {

std::vector<float> ToFloatVector(const std::vector<double> &In) {
	std::vector<float> Out;
	Out.reserve(In.size());
	for(double X : In)
		Out.push_back(static_cast<float>(X));
	return Out;
}

float NormF32(const std::vector<float> &V) {
	if(V.empty())
		return 0.f;
	return std::sqrt(Simd::DotProductF32(V.data(), V.data(), V.size()));
}

double L2Distance(const std::vector<float> &A, const std::vector<float> &B) {
	if(A.size() != B.size())
		return std::numeric_limits<double>::infinity();
	if(A.empty())
		return std::numeric_limits<double>::infinity();
	const float DistSq = Simd::L2SquaredF32(A.data(), B.data(), A.size());
	return std::sqrt(static_cast<double>(DistSq));
}

double CosineDistance(const std::vector<float> &A, const std::vector<float> &B, float NormA, float NormB) {
	if(A.size() != B.size() || A.empty())
		return std::numeric_limits<double>::infinity();
	if(NormA == 0.f || NormB == 0.f)
		return std::numeric_limits<double>::infinity();
	const float Dot = Simd::DotProductF32(A.data(), B.data(), A.size());
	const double Sim = static_cast<double>(Dot) / (static_cast<double>(NormA) * static_cast<double>(NormB));
	return 1.0 - Sim;
}

} // namespace

void VectorIndex::Clear() {
	RowIds_.clear();
	Vectors_.clear();
	FloatVectors_.clear();
	Norms_.clear();
}

void VectorIndex::BuildFromColumn(const std::vector<std::unordered_map<std::string, std::string>> &Rows,
                                  const std::string &Column) {
	Clear();
	RowIds_.reserve(Rows.size());
	Vectors_.reserve(Rows.size());
	FloatVectors_.reserve(Rows.size());
	Norms_.reserve(Rows.size());
	for(size_t I = 0; I < Rows.size(); ++I) {
		auto It = Rows[I].find(Column);
		if(It == Rows[I].end())
			continue;
		const auto Vec = AdvancedTypes::ParseVectorCell(It->second);
		if(!Vec)
			continue;
		RowIds_.push_back(I);
		Vectors_.push_back(*Vec);
		FloatVectors_.push_back(ToFloatVector(*Vec));
		Norms_.push_back(NormF32(FloatVectors_.back()));
	}
}

void VectorIndex::UpsertRow(size_t RowId, const std::vector<double> &Vec) {
	for(size_t I = 0; I < RowIds_.size(); ++I) {
		if(RowIds_[I] == RowId) {
			Vectors_[I] = Vec;
			FloatVectors_[I] = ToFloatVector(Vec);
			Norms_[I] = NormF32(FloatVectors_[I]);
			return;
		}
	}
	RowIds_.push_back(RowId);
	Vectors_.push_back(Vec);
	FloatVectors_.push_back(ToFloatVector(Vec));
	Norms_.push_back(NormF32(FloatVectors_.back()));
}

void VectorIndex::RemoveRow(size_t RowId) {
	for(size_t I = 0; I < RowIds_.size(); ++I) {
		if(RowIds_[I] == RowId) {
			RowIds_.erase(RowIds_.begin() + static_cast<std::ptrdiff_t>(I));
			Vectors_.erase(Vectors_.begin() + static_cast<std::ptrdiff_t>(I));
			FloatVectors_.erase(FloatVectors_.begin() + static_cast<std::ptrdiff_t>(I));
			Norms_.erase(Norms_.begin() + static_cast<std::ptrdiff_t>(I));
			return;
		}
	}
}

std::vector<size_t> VectorIndex::TopK(const std::vector<double> &Query, std::size_t K) const {
	struct Scored {
		size_t RowId;
		double Score;
	};
	const std::vector<float> QueryF = ToFloatVector(Query);
	const float QueryNorm = NormF32(QueryF);
	std::vector<Scored> Ranked;
	Ranked.reserve(RowIds_.size());
	for(size_t I = 0; I < RowIds_.size(); ++I) {
		const double Dist = Metric_ == VectorMetric::Cosine ?
		                        CosineDistance(FloatVectors_[I], QueryF, Norms_[I], QueryNorm) :
		                        L2Distance(FloatVectors_[I], QueryF);
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
