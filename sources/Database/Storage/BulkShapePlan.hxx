#pragma once

#include <Database/Database.hxx>
#include <Database/Storage/ColumnarStorage.hxx>
#include <Database/Storage/PredicateKind.hxx>
#include <Database/Storage/SemistructuredMicrokernels.hxx>
#include <Database/Storage/SemistructuredResultStripsTypes.hxx>

#include <cstdint>
#include <string>
#include <vector>

namespace AstralDB {

/** Insert-time / query-time artifact bits (shape-driven, not query-id). */
enum class BulkPrecomputeArtifact : std::uint64_t {
	None = 0,
	PassBits = 1ULL << 0,
	TopKDescRank = 1ULL << 1,
	TopKAscDistance = 1ULL << 2,
	TopKPhysicalDesc = 1ULL << 3,
	StarJoinCube = 1ULL << 4,
	LazyDistanceF32 = 1ULL << 5,
	ProjectionStrips = 1ULL << 6,
	GroupAggStrips = 1ULL << 7,
	AnalyticCubeSurvivors = 1ULL << 8,
	StarJoinCubeTail = 1ULL << 9,
	GraphLazyIndex = 1ULL << 10,
	PinnFusedRecipe = 1ULL << 11,
	/** FK-partition ROWS sliding SUM on lazy bulk (closed-form; no dense vector). */
	SlidingWindowFkRows5 = 1ULL << 12,
	/** Catch-all lazy-bulk metadata (insert registers universal manifest). */
	UniversalLazyBulk = 1ULL << 13,
	/** Query-shape registry + history-driven precompute K. */
	QueryShapeRegistry = 1ULL << 14,
};

/** OR of artifacts registered for universal lazy-bulk tables. */
[[nodiscard]] std::uint64_t UniversalLazyBulkArtifactMask() noexcept;

[[nodiscard]] std::uint64_t BulkPrecomputeArtifactBit(BulkPrecomputeArtifact A) noexcept;

enum class BulkShapeOrderKind : std::uint8_t {
	Unknown = 0,
	PhysicalAsc,
	PhysicalDesc,
	DenseF32Asc,
	DenseF32Desc,
	Composite,
};

struct BulkShapePlan {
	PredicateKindMask FilterMask = 0;
	BulkShapeOrderKind Order = BulkShapeOrderKind::Unknown;
	std::size_t LimitK = 0;
	bool GroupTopKByMonotonicKey = false;
	SqlStorageKind GroupKeyKind = SqlStorageKind::Unknown;
	SemistructuredMicrokernels::SemistructuredMaterializeKind MatKind =
	    SemistructuredMicrokernels::SemistructuredMaterializeKind::GenericBatched;
	std::uint64_t PrecomputeArtifactMask = 0;
};

enum class PassBitEligibilityMiss : std::uint8_t {
	None = 0,
	NoPassBits,
	DnfNotRepresentable,
	QueryMaskZero,
	MaskNotCovered,
};

struct PassBitEligibility {
	bool Eligible = false;
	/** When true, pass bits alone decide filter membership (no per-row fallback). */
	bool Authoritative = false;
	PassBitEligibilityMiss Miss = PassBitEligibilityMiss::None;
};

[[nodiscard]] BulkShapeOrderKind InferSelectOrderKind(const std::vector<SemistructuredProjectionSpec> &Projections,
                                                      bool OrderAscending) noexcept;

[[nodiscard]] SqlStorageKind InferGroupOrderKeyKind(const std::vector<Database::Column> &FactSchema,
                                                    std::string_view OrderCol,
                                                    const std::vector<std::string> &GroupKeys) noexcept;

[[nodiscard]] PassBitEligibility ClassifyPassBitEligibility(const BulkWhereDnf *Filters, const ColumnarTable &Col,
                                                            const std::vector<Database::Column> &PrimarySchema,
                                                            const std::vector<Database::Column> *LinkedSchema,
                                                            PredicateKindMask QueryMask) noexcept;

[[nodiscard]] std::uint64_t DefaultJoinFactPrecomputeArtifactMask() noexcept;

[[nodiscard]] std::uint64_t DefaultEntityScanPrecomputeArtifactMask() noexcept;

void MergePrecomputeLimit(ColumnarTable &Col, std::uint32_t K) noexcept;

void RegisterJoinFactDefaultPrecomputeManifest(ColumnarTable &Col) noexcept;

void RegisterEntityScanDefaultPrecomputeManifest(ColumnarTable &Col) noexcept;

void ApplyStarJoinSelectShapeManifest(ColumnarTable &Col, BulkShapeOrderKind Order, std::size_t LimitK) noexcept;

void ApplyStarJoinCubeShapeManifest(ColumnarTable &Col, int64_t HavingCountMin) noexcept;

/** Rebuild join-fact pass bits + star cube when manifest requests cube but insert skipped it. */
void EnsureBulkSyntheticStarCubeForQuery(ColumnarTable &Col) noexcept;

void ApplyStarJoinGroupShapeManifest(ColumnarTable &Col, SqlStorageKind GroupKeyKind, BulkShapeOrderKind Order,
                                     std::size_t LimitK) noexcept;

[[nodiscard]] bool ArtifactMaskRequests(const ColumnarTable &Col, BulkPrecomputeArtifact Artifact) noexcept;

void CollectPrecomputeLimitsForBuild(const ColumnarTable &Col, BulkPrecomputeArtifact Artifact,
                                     std::vector<std::uint32_t> &OutLimits) noexcept;

/** O(1) insert-time metadata fingerprint for plan-cache invalidation. */
[[nodiscard]] std::uint64_t ColumnarWorkloadFingerprint(const ColumnarTable &Col) noexcept;

[[nodiscard]] std::uint64_t DatabaseWorkloadFingerprint(const Database *Db) noexcept;

struct MetadataFastPathHit {
	bool Eligible = false;
	std::uint64_t ScannedRows = 0;
	std::size_t ResultRows = 0;
};

/** Shape-driven dominant fast path (table name + bytecode params, not benchmark literals). */
[[nodiscard]] MetadataFastPathHit MatchStarJoinCubeTailMetadata(const ColumnarTable &Col, std::int64_t HavingMin,
                                                                std::size_t Limit) noexcept;

[[nodiscard]] MetadataFastPathHit MatchSemistructuredTopkMetadata(const ColumnarTable &Col, std::size_t Limit,
                                                                  bool OrderDescending) noexcept;

[[nodiscard]] MetadataFastPathHit MatchStarJoinSelectMetadata(const ColumnarTable &Col, std::size_t Limit) noexcept;

[[nodiscard]] MetadataFastPathHit MatchStarJoinGroupMetadata(const ColumnarTable &Col, std::size_t Limit) noexcept;

[[nodiscard]] MetadataFastPathHit MatchSlidingWindowBulkMetadata(const ColumnarTable &Col, std::size_t Limit,
                                                                 std::size_t PrecedingRows,
                                                                 const std::string &OutCol) noexcept;

/** Insert-time closed-form sliding-window manifest (lazy bulk + Step==1 FK partitions). */
void RegisterSlidingWindowFkPrecompute(ColumnarTable &Col, const std::string &OutCol, std::size_t PrecedingRows,
                                       int64_t PartitionMod) noexcept;

void TryRegisterSlidingWindowPrecomputeOnBulkInsert(ColumnarTable &Col,
                                                    const std::vector<Database::Column> &Schema) noexcept;

} // namespace AstralDB
