#include <Database/Storage/ShapeWorkloadRegistry.hxx>

#include <Database/Storage/BulkShapePlan.hxx>
#include <Database/Storage/BulkSyntheticPrecomputeColumns.hxx>

#include <algorithm>
#include <cstdlib>

namespace AstralDB {

namespace {

[[nodiscard]] bool EnvTruthy(const char *Name) noexcept {
	const char *E = std::getenv(Name);
	return E != nullptr && E[0] != '0' && E[0] != '\0';
}

void MergeObservedLimit(ColumnarTable &Col, const std::uint32_t K) noexcept {
	if(K == 0)
		return;
	auto &Limits = Col.BulkSyntheticObservedLimits;
	if(std::find(Limits.begin(), Limits.end(), K) != Limits.end())
		return;
	if(Limits.size() >= Col.BulkSyntheticObservedLimitCap) {
		Limits.erase(Limits.begin());
	}
	Limits.push_back(K);
	std::sort(Limits.begin(), Limits.end());
	Limits.erase(std::unique(Limits.begin(), Limits.end()), Limits.end());
	MergePrecomputeLimit(Col, K);
}

void RecordFingerprint(ColumnarTable &Col, const QueryShapeFingerprint128 &Fp) noexcept {
	auto &Fps = Col.BulkSyntheticObservedShapeFps;
	for(const QueryShapeFingerprint128 &Existing : Fps) {
		if(Existing == Fp)
			return;
	}
	constexpr std::size_t kMaxShapeFps = 64;
	if(Fps.size() >= kMaxShapeFps)
		Fps.erase(Fps.begin());
	Fps.push_back(Fp);
}

} // namespace

bool ShapePrecomputeEnabled() noexcept { return EnvTruthy("ASTRALDB_SHAPE_PRECOMPUTE"); }

void RecordObservedQueryShape(ColumnarTable &Col, const BulkQueryShape &Shape,
                              const QueryShapeFingerprint128 &Fingerprint) noexcept {
	if(!Col.BulkSyntheticLazy || Col.RowCount == 0)
		return;
	if(Shape.HasLimit && Shape.Limit > 0 && Shape.Limit <= 0xFFFFFFFFu)
		MergeObservedLimit(Col, static_cast<std::uint32_t>(Shape.Limit));
	RecordFingerprint(Col, Fingerprint);
	if(ShapePrecomputeEnabled())
		SchedulePrecomputeForObservedLimits(Col);
}

void SchedulePrecomputeForObservedLimits(ColumnarTable &Col) noexcept {
	if(!Col.BulkSyntheticLazy || Col.BulkSyntheticObservedLimits.empty())
		return;
	if(!ArtifactMaskRequests(Col, BulkPrecomputeArtifact::TopKDescRank) &&
	   !ArtifactMaskRequests(Col, BulkPrecomputeArtifact::TopKPhysicalDesc) &&
	   !Col.BulkSyntheticUniversalMetadata)
		return;
	for(const std::uint32_t K : Col.BulkSyntheticObservedLimits) {
		if(K == 0)
			continue;
		std::vector<std::size_t> Winners;
		if(ArtifactMaskRequests(Col, BulkPrecomputeArtifact::TopKDescRank))
			(void)TryBulkSyntheticPrecomputedTopKDesc(Col, K, Winners);
		if(ArtifactMaskRequests(Col, BulkPrecomputeArtifact::TopKPhysicalDesc))
			(void)TryBulkSyntheticPrecomputedTopKPhysicalDesc(Col, K, Winners);
	}
}

} // namespace AstralDB
