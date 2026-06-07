#pragma once

#include <Database/Storage/ColumnarStorage.hxx>
#include <Database/Database.hxx>

#include <string>
#include <string_view>

namespace AstralDB {

[[nodiscard]] std::string BulkSyntheticPrecomputeColumnKey(std::string_view BaseColumn,
                                                           std::string_view Facet) noexcept;

/** Populate rank f32 at bulk load (pass-bit top-K precompute runs in \c FinishBulkSyntheticPrecomputeColumns). */
void SeedBulkSyntheticPrecomputeColumns(ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                                        std::size_t StartRowIndex = 0) noexcept;

/** After \c BuildBulkSyntheticPassBits: group rank stats + DESC top-K + insert-time winner cell strips. */
void FinishBulkSyntheticPrecomputeColumns(ColumnarTable &Col,
                                          const std::vector<Database::Column> *Schema = nullptr) noexcept;

[[nodiscard]] bool TryReadPrecomputeRank(const ColumnarTable &Col, std::string_view BaseColumn,
                                         std::size_t RowIndex, double &Out) noexcept;

/** Per-group max rank on pass-bit rows (enables bucket top-K collect pruning). */
void BuildBulkSyntheticPassGroupMaxRank(ColumnarTable &Col) noexcept;

/** Precompute DESC top-K winner row indices for common LIMIT values at bulk load. */
void BuildBulkSyntheticPrecomputedTopKDesc(ColumnarTable &Col) noexcept;

void BuildBulkSyntheticPrecomputedTopKDescForK(ColumnarTable &Col, std::uint32_t K) noexcept;

/** Entity-scan: build missing DESC top-K map for query LIMIT (insert keeps pass bits only). */
void EnsureEntityScanTopKDescForQuery(ColumnarTable &Col, std::size_t LimitK) noexcept;

/** JoinFact: ASC top-K by synthetic shipping distance on pass-bit rows. */
void BuildBulkSyntheticPrecomputedTopKAscDistance(ColumnarTable &Col) noexcept;

void BuildBulkSyntheticPrecomputedTopKAscDistanceForK(ColumnarTable &Col, std::uint32_t K) noexcept;
void BuildBulkSyntheticPrecomputedTopKPhysicalDescForK(ColumnarTable &Col, std::uint32_t K) noexcept;

[[nodiscard]] bool TryBulkSyntheticPrecomputedTopKAscDistance(const ColumnarTable &Col, std::size_t K,
                                                              std::vector<std::size_t> &Out) noexcept;

void BuildBulkSyntheticPrecomputedTopKPhysicalDesc(ColumnarTable &Col) noexcept;

[[nodiscard]] bool TryBulkSyntheticPrecomputedTopKPhysicalDesc(const ColumnarTable &Col, std::size_t K,
                                                               std::vector<std::size_t> &Out) noexcept;

/** O(1) winner list when insert-time top-K for \a K was precomputed. */
[[nodiscard]] bool TryBulkSyntheticPrecomputedTopKDesc(const ColumnarTable &Col, std::size_t K,
                                                       std::vector<std::size_t> &Out) noexcept;

/** Merge query LIMIT into manifest and build any missing insert-time top-K maps. */
void EnsureBulkSyntheticPrecomputeForQuery(ColumnarTable &Col, std::size_t LimitK, bool WantAscDistance,
                                           bool WantPhysicalDesc) noexcept;

[[nodiscard]] bool UseStarJoinColumnarCommitEnv() noexcept;

} // namespace AstralDB
