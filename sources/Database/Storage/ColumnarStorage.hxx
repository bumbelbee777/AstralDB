#pragma once

#include <Database/Storage/ColumnZoneMap.hxx>
#include <Database/Storage/ShapeFingerprint.hxx>
#include <Database/Storage/SemistructuredResultStripsTypes.hxx>
#include <DS/FormatDoubleSimd.hxx>

#include <array>
#include <cstddef>
#include <cstdint>
#include <future>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace AstralDB {
namespace SQL {
struct Instruction;
}

using RowItem = std::unordered_map<std::string, std::string>;
using RowTable = std::vector<RowItem>;
using BulkWhereDnfBranch = std::vector<std::tuple<std::string, std::string, std::string>>;
using BulkWhereDnf = std::vector<BulkWhereDnfBranch>;
using FilterPredicateTriple = std::tuple<std::string, std::string, std::string>;

/** Literal column overlay for lazy-bulk UPDATE (evaluated on cell read, last match wins). */
struct BulkSyntheticLiteralPatch {
	BulkWhereDnf Where;
	std::string Column;
	std::string Value;
};

/** Workload family for insert-time pass-bit precompute (warehouse star vs entity scan). */
enum class BulkSyntheticPassFamily : std::uint8_t {
	None = 0,
	JoinFact = 1,
	EntityScan = 2,
};

/** Column-major replica of a row table for analytics scans. */
struct ColumnarTable {
	std::unordered_map<std::string, std::vector<std::string>> Columns;
	std::size_t RowCount = 0;
	/** Contiguous INSERT BULK rows in physical order (id = BulkStartId + index * BulkStep). */
	bool BulkSyntheticPhysicalOrder = false;
	int64_t BulkStartId = 1;
	int64_t BulkStep = 1;
	/** Non-zero when bulk rows use schema FK modulus for partition-friendly windows. */
	int64_t BulkPartitionMod = 0;
	/** Row count only; cell values generated on demand from bulk metadata + schema. */
	bool BulkSyntheticLazy = false;
	/** Stacked lazy WHERE filters; each DNF must hold (AND across the stack). */
	std::vector<BulkWhereDnf> BulkSyntheticWhereDnfs;
	/** Lazy-bulk DELETE tombstones (one bit per row index). */
	std::vector<std::uint64_t> BulkSyntheticDeleteBits;
	/** Tombstone count (avoids O(n) recount in metadata paths). */
	std::uint32_t BulkSyntheticDeletedCount = 0;
	/** Lazy-bulk literal UPDATE patches (applied on cell read). */
	std::vector<BulkSyntheticLiteralPatch> BulkSyntheticLiteralPatches;
	/** Per 64K-row group min/max for scan pruning. */
	std::vector<RowGroupStats> RowGroups;
	/** Insert-time AND of composable synthetic warehouse predicates (one bit per row). */
	std::vector<std::uint64_t> BulkSyntheticPassBits;
	/** Bitmask of predicate kinds folded into \c BulkSyntheticPassBits. */
	std::uint64_t BulkSyntheticPassKindMask = 0;
	/** Workload family used to build/evaluate \c BulkSyntheticPassBits. */
	BulkSyntheticPassFamily BulkSyntheticPassFamilyTag = BulkSyntheticPassFamily::None;
	/** Rows passing the precomputed filter per 64K row group. */
	std::vector<std::uint32_t> BulkSyntheticPassGroupCounts;
	/** Indices of non-zero words in \c BulkSyntheticPassBits (insert-time sparse index). */
	std::vector<std::uint32_t> BulkSyntheticPassSparseWords;
	/** Insert-time FK LUT: country index by cust_id (size = BulkSyntheticFkCustMod). */
	std::vector<std::uint8_t> BulkSyntheticCountryLut;
	/** Insert-time FK LUT: category index by prod_id (size = BulkSyntheticFkProdMod). */
	std::vector<std::uint8_t> BulkSyntheticCategoryLut;
	int64_t BulkSyntheticFkCustMod = 0;
	int64_t BulkSyntheticFkProdMod = 0;
	/** Insert-time join GROUP BY hash slot per fact row index. */
	std::vector<std::uint8_t> BulkSyntheticJoinGroupSlotByRow;
	/** Insert-time \c amount per row index for fused SUM without row-id decode. */
	std::vector<double> BulkSyntheticAmountByRow;
	/** Dense pass-row list: warehouse slot and amount for rows set in \c BulkSyntheticPassBits. */
	std::vector<std::uint8_t> BulkSyntheticPassSlots;
	std::vector<double> BulkSyntheticPassAmounts;
	/** Row index aligned with \c BulkSyntheticPassSlots / \c BulkSyntheticPassAmounts. */
	std::vector<std::uint32_t> BulkSyntheticPassRowIndex;
	/** Q1 CUBE(4) insert-time aggregates per grouping-set mask (query emits without scanning rows). */
	std::array<std::vector<std::pair<std::int64_t, double>>, 16> BulkSyntheticStarCubeDense;
	std::array<std::unordered_map<std::uint32_t, std::pair<std::int64_t, double>>, 16> BulkSyntheticStarCubeSparse;
	std::array<bool, 16> BulkSyntheticStarCubeMaskDense{};
	bool BulkSyntheticStarCubeReady = false;
	/** Non-zero CUBE slots (mask, flat slot) passing HAVING; cnt/sum kept fresh at insert time. */
	struct StarCubeSurvivorEntry {
		std::uint8_t Mask = 0;
		std::uint32_t Slot = 0;
		std::int64_t Cnt = 0;
		double Sum = 0;
	};
	std::vector<StarCubeSurvivorEntry> BulkSyntheticStarCubeSurvivors;
	/** Dedup keys for \c BulkSyntheticStarCubeSurvivors (mask<<32|slot). */
	std::unordered_set<std::uint64_t> BulkSyntheticStarCubeSurvivorKeys;
	/** mask<<32|slot -> index in \c BulkSyntheticStarCubeSurvivors for O(1) cnt/sum refresh. */
	std::unordered_map<std::uint64_t, std::size_t> BulkSyntheticStarCubeSurvivorIndex;
	/** Min COUNT(*) for survivor index pruning (set from STAR_JOIN_CUBE_BULK HAVING operand). */
	std::int64_t BulkSyntheticStarCubeHavingMin = 101;
	/** Sliding SUM() OVER output without per-row string materialization (\c sum_* window cols). */
	std::string BulkSyntheticSlidingSumColumn;
	std::vector<double> BulkSyntheticSlidingSumByRow;
	/** Precomputed ROWS-frame width when \c BulkPrecomputeArtifact::SlidingWindowFkRows5 is set. */
	std::uint8_t BulkSyntheticSlidingWindowPrecedingRows = 0;
	/** Panel-formatted decimals (contiguous arena); keyed by column name. */
	std::unordered_map<std::string, FormatDoubleSimd::FormattedDoubleColumn> FormattedColumns;
	/** Set when LIMIT/SELECT finalize committed row count without building \c RowStore. */
	bool BulkSyntheticWindowProjectionCommitted = false;
	/** Row indices after columnar ORDER BY on lazy bulk (LIMIT materializes this order). */
	std::vector<std::size_t> BulkSyntheticSortedRowIndices;
	/** Lazy f32 TEXT_RANK (and similar) keys indexed by row (filled on first fused semistructured scan). */
	std::vector<float> BulkSyntheticLazyRankF32;
	/** Rows in the latest semistructured top-K (one bit per row index). */
	std::vector<std::uint64_t> BulkSyntheticTopKBits;
	/** Per 64K row group: max TEXT_RANK among rows set in \c BulkSyntheticPassBits. */
	std::vector<float> BulkSyntheticPassGroupMaxRankF32;
	/** Insert-time DESC top-K row indices by limit K (pass bits + \c BulkSyntheticLazyRankF32). */
	std::unordered_map<std::uint32_t, std::vector<std::size_t>> BulkSyntheticPrecomputedTopKDesc;
	/** Insert-time ASC top-K by spherical distance (JoinFact star-select workloads). */
	std::vector<float> BulkSyntheticLazyDistanceF32;
	std::unordered_map<std::uint32_t, std::vector<std::size_t>> BulkSyntheticPrecomputedTopKAscDistance;
	/** Insert-time DESC top-K by physical row order on JoinFact pass bits (monotonic date GROUP BY). */
	std::unordered_map<std::uint32_t, std::vector<std::size_t>> BulkSyntheticPrecomputedTopKPhysicalDesc;
	/** Query-layout projection strips (arena columns, keyed by projection fingerprint). */
	std::unordered_map<std::uint64_t, BulkSyntheticProjectionStripPack> BulkSyntheticPrecomputedStripPacks;
	/** Insert-time winner cell strips (LUT/SIMD panels at bulk load, keyed by LIMIT K). */
	std::unordered_map<std::uint32_t, BulkSyntheticWinnerCellStrips> BulkSyntheticPrecomputedWinnerCellStrips;
	/** Insert-time semantic strip zip (column order = warehouse discover; keyed by LIMIT K). */
	std::unordered_map<std::uint32_t, BulkSyntheticProjectionStripPack> BulkSyntheticPrecomputedSemanticStripPacks;
	/** Expected semantics fingerprint per precomputed LIMIT K. */
	std::unordered_map<std::uint32_t, std::uint64_t> BulkSyntheticPrecomputedSemanticsFpByK;
	/** Shape-driven insert-time artifact mask (\c BulkPrecomputeArtifact bits). */
	std::uint64_t BulkSyntheticPrecomputeArtifactMask = 0;
	/** When set, query metadata matchers accept any registered lazy-bulk shape (mask 0 = all artifacts). */
	bool BulkSyntheticUniversalMetadata = false;
	/** Bitmask of \c SqlStorageKind values present at insert (schema profile). */
	std::uint32_t BulkSyntheticSchemaKindMask = 0;
	/** Default SUM() OVER output column from schema (e.g. sum_amount). */
	std::string BulkSyntheticDefaultWindowOutCol;
	/** Pass bits represent every row (skip bit tests; still materialize bits for coverage checks). */
	bool BulkSyntheticPassAllRows = false;
	/** O(1) insert: predicates derived from rowId formulas; no pass-bit bitmap built. */
	bool BulkSyntheticMetadataOnly = false;
	/** Closed-form pass rate in permille (1000 = 100%) for metadata-only eligibility. */
	std::uint32_t BulkSyntheticPassRatePermille = 1000;
	/** Precomputed Q1 outer-group answer rows (LIMIT K). */
	std::vector<RowItem> BulkSyntheticPrecomputedQ1TailRows;
	/** Background JoinFact cube+tail build (metadata-first insert). */
	std::shared_ptr<std::future<void>> BulkSyntheticJoinFactPrecomputeJob;
	/** LIMIT K values to precompute (merged from DDL hints + lowered bytecode). */
	std::vector<std::uint32_t> BulkSyntheticPrecomputeLimits;
	/** Sorted unique LIMIT K observed from query history. */
	std::vector<std::uint32_t> BulkSyntheticObservedLimits;
	/** Recent distinct query-shape fingerprints (cap 64). */
	std::vector<QueryShapeFingerprint128> BulkSyntheticObservedShapeFps;
	std::uint32_t BulkSyntheticObservedLimitCap = 32;
	/** Cached FK mods / pass mask from CREATE (avoid re-walk each BULK). */
	int64_t BulkSyntheticCachedFkModA = 0;
	int64_t BulkSyntheticCachedFkModB = 0;
	std::uint64_t BulkSyntheticCachedPassKindMask = 0;
	/** Shared PARTITION BY buckets for lazy bulk window passes (rebuilt when partition/order keys change). */
	bool BulkSyntheticWindowBucketsBuilt = false;
	/** FK buckets filled by ascending row index scan (order_date monotone with index). */
	bool BulkSyntheticWindowBucketSeqSorted = false;
	bool BulkSyntheticFusedCustDateCoreDone = false;
	/** Cached cust_id/order_date window metrics (running sum, ma7, ma30, lag-1). */
	struct BulkSyntheticCustDateWindowCache {
		std::vector<double> Running;
		std::vector<double> Ma7;
		std::vector<double> Ma30;
		std::vector<double> Lag1;
	};
	std::optional<BulkSyntheticCustDateWindowCache> CustDateWindowCache;
	bool BulkSyntheticWindowBucketPerRow = false;
	bool BulkSyntheticWindowBucketOrderAsc = true;
	int64_t BulkSyntheticWindowBucketPartMod = 0;
	std::string BulkSyntheticWindowBucketPartCol;
	std::string BulkSyntheticWindowBucketOrderCol;
	std::vector<std::vector<std::size_t>> BulkSyntheticWindowBuckets;
	/** Window outputs keyed by result column (dense per row index). */
	std::unordered_map<std::string, std::vector<double>> BulkSyntheticWindowDbl;

	void RebuildFromRows(const RowTable &Rows);
	RowTable MaterializeAllRows() const;
};

/** Column-oriented GROUP BY fast path for SUM/MIN/MAX/AVG + optional COUNT(*). */
struct ColumnarGroupBy {
	static bool TryRun(RowTable &Tbl, const SQL::Instruction &Inst, const std::vector<std::string> &ActiveKeys);
	static bool TryRunFromColumnar(ColumnarTable &Col, RowTable &Tbl, const SQL::Instruction &Inst,
	                               const std::vector<std::string> &ActiveKeys);
};

/** O(n) sliding SUM for \c ROWS BETWEEN fixed preceding and \c CURRENT ROW on columnar storage. */
bool TrySlidingSumRowsFrame(ColumnarTable &Col, const std::string &PartCol, const std::string &OrderCol,
                            const std::string &SrcCol, const std::string &OutCol, std::size_t PrecedingRows,
                            bool OrderAscending, bool SkipOutputStore = false);

bool TryCumulativeSumRowsFrame(ColumnarTable &Col, const std::string &PartCol, const std::string &OrderCol,
                               const std::string &SrcCol, const std::string &OutCol, bool OrderAscending);

bool TrySlidingAvgRowsFrame(ColumnarTable &Col, const std::string &PartCol, const std::string &OrderCol,
                            const std::string &SrcCol, const std::string &OutCol, std::size_t PrecedingRows,
                            bool OrderAscending);

bool TryLagRowsFrame(ColumnarTable &Col, const std::string &PartCol, const std::string &OrderCol,
                     const std::string &SrcCol, const std::string &OutCol, std::size_t LagOffset, bool OrderAscending);

bool TryRankRowsFrame(ColumnarTable &Col, const std::string &PartCol, const std::string &OrderCol,
                      const std::string &OutCol, bool OrderAscending);

/** Resolve grouped-aggregate column refs for window fast paths (e.g. \c amount → \c total_amount). */
[[nodiscard]] std::string ResolveGroupedAggColumnRef(const ColumnarTable &Col, const std::string &Name);

/** Hash join on columnar storage without materializing input row stores. */
bool TryColumnarInnerJoinEquality(const ColumnarTable &Left, const ColumnarTable &Right, const std::string &LeftCol,
                                  const std::string &RightCol, RowTable &Result);

/** Append \p Count synthetic rows; \p CellAt produces each column value. */
void AppendBulkSyntheticColumnar(ColumnarTable &Col, const std::vector<std::string> &ColNames,
                                 const std::function<std::string(int64_t RowId, std::size_t ColIndex)> &CellAt,
                                 int64_t Count, int64_t StartId, int64_t Step);

/** O(1) logical bulk: sets row count and bulk metadata without materializing column vectors. */
void MarkBulkSyntheticLazy(ColumnarTable &Col, int64_t Count, int64_t StartId, int64_t Step);

BulkWhereDnf ToBulkWhereDnf(const std::vector<std::vector<FilterPredicateTriple>> &Branches);

/** Physical-order lazy bulk: map \c id lower/upper bounds to row-index half-open \c [Begin, End). */
[[nodiscard]] bool TryLazyBulkIdInterval(const BulkWhereDnf &Dnf, const ColumnarTable &Col, std::size_t &Begin,
                                         std::size_t &End) noexcept;

[[nodiscard]] bool BulkSyntheticRowIsDeleted(const ColumnarTable &Col, std::size_t RowIndex) noexcept;

} // namespace AstralDB
