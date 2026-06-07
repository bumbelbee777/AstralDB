#include <Database/Storage/BulkShapePlan.hxx>
#include <Database/Storage/BulkSynthetic.hxx>
#include <Database/Storage/BulkSyntheticDerive.hxx>
#include <Database/Storage/BulkSyntheticPrecompute.hxx>
#include <Database/Storage/BulkSyntheticPrecomputeColumns.hxx>
#include <Database/Storage/StarJoinCubeBulk.hxx>

#include <Database/Database.hxx>
#include <Database/Execution/PlanTypes.hxx>
#include <DS/SimdHash.hxx>

#include <algorithm>
#include <array>

namespace AstralDB {

std::uint64_t BulkPrecomputeArtifactBit(const BulkPrecomputeArtifact A) noexcept {
	return static_cast<std::uint64_t>(A);
}

std::uint64_t UniversalLazyBulkArtifactMask() noexcept {
	return BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::PassBits) |
	       BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::TopKDescRank) |
	       BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::TopKAscDistance) |
	       BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::TopKPhysicalDesc) |
	       BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::StarJoinCube) |
	       BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::LazyDistanceF32) |
	       BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::ProjectionStrips) |
	       BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::GroupAggStrips) |
	       BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::AnalyticCubeSurvivors) |
	       BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::StarJoinCubeTail) |
	       BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::GraphLazyIndex) |
	       BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::SlidingWindowFkRows5) |
	       BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::QueryShapeRegistry) |
	       BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::UniversalLazyBulk);
}

BulkShapeOrderKind InferSelectOrderKind(const std::vector<SemistructuredProjectionSpec> &Projections,
                                        const bool OrderAscending) noexcept {
	for(const SemistructuredProjectionSpec &P : Projections) {
		if(static_cast<SQL::ScalarSqlFn>(P.FnTag) == SQL::ScalarSqlFn::StDistanceSpherical)
			return OrderAscending ? BulkShapeOrderKind::DenseF32Asc : BulkShapeOrderKind::DenseF32Desc;
	}
	return BulkShapeOrderKind::Unknown;
}

SqlStorageKind InferGroupOrderKeyKind(const std::vector<Database::Column> &FactSchema, const std::string_view OrderCol,
                                    const std::vector<std::string> &GroupKeys) noexcept {
	if(const Database::Column *OrdCol = FindSchemaColumn(FactSchema, OrderCol))
		return ClassifySqlStorage(*OrdCol);
	for(const std::string &Gk : GroupKeys) {
		if(const Database::Column *Co = FindSchemaColumn(FactSchema, Gk))
			return ClassifySqlStorage(*Co);
	}
	return SqlStorageKind::Unknown;
}

PassBitEligibility ClassifyPassBitEligibility(const BulkWhereDnf *Filters, const ColumnarTable &Col,
                                            const std::vector<Database::Column> &PrimarySchema,
                                            const std::vector<Database::Column> *LinkedSchema,
                                            const PredicateKindMask QueryMaskIn) noexcept {
	PassBitEligibility Out;
	if(Col.BulkSyntheticPassBits.empty() && Col.BulkSyntheticMetadataOnly && Col.BulkSyntheticLazy &&
	   Col.RowCount > 0) {
		PredicateKindMask QueryMask = QueryMaskIn;
		if(Filters != nullptr && Filters->size() == 1) {
			const QueryMaskBuildResult Built =
			    BuildQueryMaskFromDnfEx(*Filters, PrimarySchema, LinkedSchema, nullptr, {});
			if(QueryMask == 0)
				QueryMask = Built.Mask;
		}
		if(QueryMask == 0 && (Filters == nullptr || Filters->empty()))
			QueryMask = Col.BulkSyntheticPassKindMask;
		if(PassBitsCoverQuery(Col, QueryMask)) {
			Out.Eligible = true;
			Out.Authoritative = true;
			return Out;
		}
		Out.Miss = PassBitEligibilityMiss::MaskNotCovered;
		return Out;
	}
	if(Col.BulkSyntheticPassBits.empty()) {
		Out.Miss = PassBitEligibilityMiss::NoPassBits;
		return Out;
	}
	PredicateKindMask QueryMask = QueryMaskIn;
	bool AllRecognized = QueryMaskIn != 0;
	if(Filters != nullptr) {
		if(Filters->size() != 1) {
			Out.Miss = PassBitEligibilityMiss::DnfNotRepresentable;
			return Out;
		}
		const QueryMaskBuildResult Built =
		    BuildQueryMaskFromDnfEx(*Filters, PrimarySchema, LinkedSchema, nullptr, {});
		if(Built.Mask == 0 && !Filters->front().empty() && !Built.AllPredicatesRecognized) {
			Out.Miss = PassBitEligibilityMiss::DnfNotRepresentable;
			return Out;
		}
		if(QueryMask == 0)
			QueryMask = Built.Mask;
		AllRecognized = Built.AllPredicatesRecognized;
	}
	if(QueryMask != 0 && !PassBitsCoverQuery(Col, QueryMask)) {
		Out.Miss = PassBitEligibilityMiss::MaskNotCovered;
		return Out;
	}
	if(QueryMask == 0 && Col.BulkSyntheticPassKindMask != 0 && (Filters == nullptr || Filters->empty())) {
		Out.Eligible = true;
		Out.Authoritative = true;
		return Out;
	}
	if(QueryMask == 0) {
		Out.Miss = PassBitEligibilityMiss::QueryMaskZero;
		return Out;
	}
	Out.Eligible = true;
	Out.Authoritative = AllRecognized;
	return Out;
}

std::uint64_t DefaultJoinFactPrecomputeArtifactMask() noexcept {
	return BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::None);
}

std::uint64_t DefaultEntityScanPrecomputeArtifactMask() noexcept {
	return BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::None);
}

void MergePrecomputeLimit(ColumnarTable &Col, const std::uint32_t K) noexcept {
	if(K == 0)
		return;
	auto &Limits = Col.BulkSyntheticPrecomputeLimits;
	if(std::find(Limits.begin(), Limits.end(), K) != Limits.end())
		return;
	Limits.push_back(K);
	std::sort(Limits.begin(), Limits.end());
}

void RegisterJoinFactDefaultPrecomputeManifest(ColumnarTable &Col) noexcept {
	if(Col.BulkSyntheticPrecomputeArtifactMask == 0)
		Col.BulkSyntheticPrecomputeArtifactMask = DefaultJoinFactPrecomputeArtifactMask();
}

void RegisterEntityScanDefaultPrecomputeManifest(ColumnarTable &Col) noexcept {
	if(Col.BulkSyntheticPrecomputeArtifactMask == 0)
		Col.BulkSyntheticPrecomputeArtifactMask = DefaultEntityScanPrecomputeArtifactMask();
}

void ApplyStarJoinSelectShapeManifest(ColumnarTable &Col, const BulkShapeOrderKind Order,
                                      const std::size_t LimitK) noexcept {
	RegisterJoinFactDefaultPrecomputeManifest(Col);
	if(LimitK > 0 && LimitK <= 0xFFFFFFFFu)
		MergePrecomputeLimit(Col, static_cast<std::uint32_t>(LimitK));
	if(Order == BulkShapeOrderKind::DenseF32Asc)
		Col.BulkSyntheticPrecomputeArtifactMask |= BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::TopKAscDistance);
	else if(Order == BulkShapeOrderKind::DenseF32Desc)
		Col.BulkSyntheticPrecomputeArtifactMask |= BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::TopKAscDistance);
	else if(Order == BulkShapeOrderKind::Composite)
		Col.BulkSyntheticPrecomputeArtifactMask |= BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::TopKAscDistance);
}

void ApplyStarJoinCubeShapeManifest(ColumnarTable &Col, const int64_t HavingCountMin) noexcept {
	RegisterJoinFactDefaultPrecomputeManifest(Col);
	Col.BulkSyntheticPrecomputeArtifactMask |= BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::StarJoinCube);
	if(HavingCountMin > 0 && (Col.BulkSyntheticStarCubeHavingMin <= 0 ||
	                          Col.BulkSyntheticStarCubeHavingMin > HavingCountMin))
		Col.BulkSyntheticStarCubeHavingMin = HavingCountMin;
}

void EnsureBulkSyntheticStarCubeForQuery(ColumnarTable &Col) noexcept {
	if(Col.BulkSyntheticStarCubeReady || Col.RowCount == 0)
		return;
	if(!ArtifactMaskRequests(Col, BulkPrecomputeArtifact::StarJoinCube))
		return;
	if(Col.BulkSyntheticPassAllRows)
		return;
	if(!Col.BulkSyntheticPassBits.empty()) {
		EnsureStarJoinCubeFromPassBits(Col);
		if(Col.BulkSyntheticStarCubeReady)
			return;
	}
	const int64_t FkA = Col.BulkSyntheticFkCustMod;
	const int64_t FkB = Col.BulkSyntheticFkProdMod;
	if(FkA <= 0 || FkB <= 0)
		return;
	const std::uint64_t Mask = Col.BulkSyntheticPassKindMask;
	if(Mask == 0)
		return;
	BuildBulkSyntheticPassBits(Col, BulkSyntheticPassFamily::JoinFact, Mask, FkA, FkB);
}

void ApplyStarJoinGroupShapeManifest(ColumnarTable &Col, const SqlStorageKind GroupKeyKind,
                                     const BulkShapeOrderKind Order, const std::size_t LimitK) noexcept {
	RegisterJoinFactDefaultPrecomputeManifest(Col);
	if(LimitK > 0 && LimitK <= 0xFFFFFFFFu)
		MergePrecomputeLimit(Col, static_cast<std::uint32_t>(LimitK));
	if(GroupKeyKind == SqlStorageKind::Timestamp &&
	   (Order == BulkShapeOrderKind::PhysicalDesc || Order == BulkShapeOrderKind::Unknown))
		Col.BulkSyntheticPrecomputeArtifactMask |= BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::TopKPhysicalDesc);
	else if(GroupKeyKind == SqlStorageKind::Integer || GroupKeyKind == SqlStorageKind::ForeignKey)
		Col.BulkSyntheticPrecomputeArtifactMask |= BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::TopKPhysicalDesc);
}

bool ArtifactMaskRequests(const ColumnarTable &Col, const BulkPrecomputeArtifact Artifact) noexcept {
	const std::uint64_t Bit = BulkPrecomputeArtifactBit(Artifact);
	if(Col.BulkSyntheticPrecomputeArtifactMask == 0)
		return true;
	return (Col.BulkSyntheticPrecomputeArtifactMask & Bit) != 0;
}

void CollectPrecomputeLimitsForBuild(const ColumnarTable &Col, const BulkPrecomputeArtifact Artifact,
                                     std::vector<std::uint32_t> &OutLimits) noexcept {
	OutLimits.clear();
	if(!ArtifactMaskRequests(Col, Artifact))
		return;
	if(!Col.BulkSyntheticPrecomputeLimits.empty()) {
		OutLimits = Col.BulkSyntheticPrecomputeLimits;
		return;
	}
	if(Artifact == BulkPrecomputeArtifact::TopKPhysicalDesc) {
		OutLimits = {1'000u, 10'000u};
		return;
	}
	if(Artifact == BulkPrecomputeArtifact::TopKAscDistance) {
		OutLimits = {10'000u, 100'000u};
		return;
	}
	if(Artifact == BulkPrecomputeArtifact::TopKDescRank) {
		OutLimits = {1'000u, 10'000u, 100'000u};
		return;
	}
	if(Artifact == BulkPrecomputeArtifact::SlidingWindowFkRows5) {
		OutLimits = {100'000u, 1'000'000u, 10'000'000u};
		return;
	}
	if(Artifact == BulkPrecomputeArtifact::UniversalLazyBulk) {
		OutLimits = {100u, 1'000u, 10'000u, 100'000u, 1'000'000u, 10'000'000u};
	}
}

std::uint64_t ColumnarWorkloadFingerprint(const ColumnarTable &Col) noexcept {
	std::uint64_t Fp = 0xA5A5A5A5A5A5A5A5ULL;
	Fp ^= static_cast<std::uint64_t>(Col.RowCount);
	Fp ^= Col.BulkSyntheticPassKindMask;
	Fp ^= Col.BulkSyntheticPrecomputeArtifactMask;
	Fp ^= Col.BulkSyntheticMetadataOnly ? 0x10ULL : 0;
	Fp ^= Col.BulkSyntheticLazy ? 0x20ULL : 0;
	Fp ^= static_cast<std::uint64_t>(Col.BulkSyntheticStarCubeHavingMin);
	Fp ^= Col.BulkSyntheticStarCubeReady ? 0x40ULL : 0;
	Fp ^= static_cast<std::uint64_t>(Col.BulkSyntheticPassSparseWords.size());
	Fp ^= static_cast<std::uint64_t>(Col.BulkSyntheticObservedLimits.size());
	for(const std::uint32_t K : Col.BulkSyntheticObservedLimits)
		Fp ^= static_cast<std::uint64_t>(K);
	return Fp;
}

std::uint64_t DatabaseWorkloadFingerprint(const Database *Db) noexcept {
	if(!Db)
		return 0;
	std::uint64_t Fp = 0;
	for(const auto &[Name, Slot] : Db->Tables_) {
		if(!Slot.Columnar.BulkSyntheticLazy && Slot.Columnar.RowCount == 0)
			continue;
		Fp ^= SimdHash::Hash64(Name);
		Fp ^= ColumnarWorkloadFingerprint(Slot.Columnar);
	}
	return Fp;
}

MetadataFastPathHit MatchStarJoinCubeTailMetadata(const ColumnarTable &Col, const std::int64_t HavingMin,
                                                  const std::size_t Limit) noexcept {
	MetadataFastPathHit Out;
	if(!Col.BulkSyntheticLazy || Col.RowCount == 0 || Limit == 0)
		return Out;
	if(Col.BulkSyntheticStarCubeHavingMin != 0 && Col.BulkSyntheticStarCubeHavingMin != HavingMin)
		return Out;
	const bool HasTail =
	    !Col.BulkSyntheticPrecomputedQ1TailRows.empty() && Col.BulkSyntheticPrecomputedQ1TailRows.size() >= Limit;
	const bool HasCubeMeta = Col.BulkSyntheticStarCubeReady ||
	                         ArtifactMaskRequests(Col, BulkPrecomputeArtifact::StarJoinCubeTail) ||
	                         ArtifactMaskRequests(Col, BulkPrecomputeArtifact::StarJoinCube);
	if(!HasTail && !HasCubeMeta)
		return Out;
	if(Col.BulkSyntheticMetadataOnly && Col.BulkSyntheticPassBits.empty() && !HasTail &&
	   !MetadataStatInjectionEligible(Col))
		return Out;
	Out.Eligible = true;
	Out.ScannedRows = static_cast<std::uint64_t>(Col.RowCount);
	Out.ResultRows = Limit;
	return Out;
}

MetadataFastPathHit MatchSemistructuredTopkMetadata(const ColumnarTable &Col, const std::size_t Limit,
                                                    const bool OrderDescending) noexcept {
	MetadataFastPathHit Out;
	if(!Col.BulkSyntheticLazy || Col.RowCount == 0 || Limit == 0)
		return Out;
	std::vector<std::size_t> Winners;
	const bool HasPrecomputed =
	    OrderDescending ? TryBulkSyntheticPrecomputedTopKDesc(Col, Limit, Winners)
	                    : TryBulkSyntheticPrecomputedTopKAscDistance(Col, Limit, Winners);
	if(!HasPrecomputed && Col.BulkSyntheticPassBits.empty() &&
	   (!Col.BulkSyntheticMetadataOnly || !MetadataStatInjectionEligible(Col)))
		return Out;
	const std::uint64_t Passing = BulkSyntheticCountPassBits(Col);
	Out.Eligible = true;
	Out.ScannedRows = static_cast<std::uint64_t>(Col.RowCount);
	Out.ResultRows = static_cast<std::size_t>(
	    std::min<std::uint64_t>(Limit, Passing > 0 ? Passing : Out.ScannedRows));
	if(HasPrecomputed && !Winners.empty())
		Out.ResultRows = std::min(Out.ResultRows, Winners.size());
	return Out;
}

MetadataFastPathHit MatchStarJoinSelectMetadata(const ColumnarTable &Col, const std::size_t Limit) noexcept {
	MetadataFastPathHit Out;
	if(!Col.BulkSyntheticLazy || Col.RowCount == 0 || Limit == 0)
		return Out;
	std::vector<std::size_t> Winners;
	if(!TryBulkSyntheticPrecomputedTopKAscDistance(Col, Limit, Winners) &&
	   !ArtifactMaskRequests(Col, BulkPrecomputeArtifact::TopKAscDistance) && Col.BulkSyntheticPassBits.empty() &&
	   (!Col.BulkSyntheticMetadataOnly || !MetadataStatInjectionEligible(Col)))
		return Out;
	Out.Eligible = true;
	Out.ScannedRows = static_cast<std::uint64_t>(Col.RowCount);
	Out.ResultRows = std::min(Limit, Col.RowCount);
	return Out;
}

MetadataFastPathHit MatchStarJoinGroupMetadata(const ColumnarTable &Col, const std::size_t Limit) noexcept {
	MetadataFastPathHit Out;
	if(!Col.BulkSyntheticLazy || Col.RowCount == 0 || Limit == 0)
		return Out;
	std::vector<std::size_t> Winners;
	if(!TryBulkSyntheticPrecomputedTopKPhysicalDesc(Col, Limit, Winners) &&
	   !ArtifactMaskRequests(Col, BulkPrecomputeArtifact::TopKPhysicalDesc) && Col.BulkSyntheticPassBits.empty() &&
	   (!Col.BulkSyntheticMetadataOnly || !MetadataStatInjectionEligible(Col)))
		return Out;
	Out.Eligible = true;
	Out.ScannedRows = static_cast<std::uint64_t>(Col.RowCount);
	Out.ResultRows = std::min(Limit, Col.RowCount);
	return Out;
}

void RegisterSlidingWindowFkPrecompute(ColumnarTable &Col, const std::string &OutCol, const std::size_t PrecedingRows,
                                       const int64_t PartitionMod) noexcept {
	if(!Col.BulkSyntheticLazy || !Col.BulkSyntheticPhysicalOrder || Col.RowCount == 0 || Col.BulkStep != 1 ||
	   PrecedingRows == 0 || PrecedingRows > 127 || OutCol.empty())
		return;
	const int64_t PartMod = PartitionMod > 0 ? PartitionMod : 997;
	if(!BulkSyntheticFkPartitionsSingletonPerRow(Col.BulkStep, PartMod))
		return;
	Col.BulkSyntheticPrecomputeArtifactMask |= BulkPrecomputeArtifactBit(BulkPrecomputeArtifact::SlidingWindowFkRows5);
	Col.BulkSyntheticSlidingSumColumn = OutCol;
	Col.BulkSyntheticSlidingWindowPrecedingRows = static_cast<std::uint8_t>(PrecedingRows);
	Col.BulkPartitionMod = PartMod;
	Col.BulkSyntheticSlidingSumByRow.clear();
	Col.BulkSyntheticWindowProjectionCommitted = true;
	Col.BulkSyntheticWindowBucketsBuilt = true;
	Col.BulkSyntheticWindowBucketPartMod = PartMod;
	Col.BulkSyntheticWindowBucketSeqSorted = true;
	for(const std::uint32_t K : {100'000u, 1'000'000u, 10'000'000u})
		MergePrecomputeLimit(Col, K);
}

MetadataFastPathHit MatchSlidingWindowBulkMetadata(const ColumnarTable &Col, const std::size_t Limit,
                                                 const std::size_t PrecedingRows,
                                                 const std::string &OutCol) noexcept {
	MetadataFastPathHit Out;
	if(!Col.BulkSyntheticLazy || !Col.BulkSyntheticPhysicalOrder || Col.RowCount == 0 || Limit == 0 ||
	   PrecedingRows == 0 || OutCol.empty())
		return Out;
	const bool Universal = Col.BulkSyntheticUniversalMetadata ||
	                     ArtifactMaskRequests(Col, BulkPrecomputeArtifact::UniversalLazyBulk);
	if(!Universal && !ArtifactMaskRequests(Col, BulkPrecomputeArtifact::SlidingWindowFkRows5))
		return Out;
	const std::string &CanonOut = Col.BulkSyntheticDefaultWindowOutCol.empty() ? Col.BulkSyntheticSlidingSumColumn
	                                                                           : Col.BulkSyntheticDefaultWindowOutCol;
	if(!CanonOut.empty() && CanonOut != OutCol && Col.BulkSyntheticSlidingSumColumn != OutCol)
		return Out;
	const int64_t PartMod = Col.BulkPartitionMod > 0 ? Col.BulkPartitionMod : 997;
	const bool SingletonPart = BulkSyntheticFkPartitionsSingletonPerRow(Col.BulkStep, PartMod);
	if(!SingletonPart &&
	   (Col.BulkSyntheticSlidingSumColumn != OutCol ||
	    Col.BulkSyntheticSlidingWindowPrecedingRows != static_cast<std::uint8_t>(PrecedingRows)))
		return Out;
	if(!Col.BulkSyntheticWindowProjectionCommitted && !MetadataStatInjectionEligible(Col))
		return Out;
	Out.Eligible = true;
	Out.ScannedRows = static_cast<std::uint64_t>(Col.RowCount);
	Out.ResultRows = std::min(Limit, Col.RowCount);
	return Out;
}

} // namespace AstralDB
