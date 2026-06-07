#pragma once



#include <Database/Storage/BulkSyntheticSemistructured.hxx>

#include <Database/Storage/ColumnarStorage.hxx>

#include <Database/Storage/SemistructuredResultStripsTypes.hxx>

#include <Database/Database.hxx>

#include <Database/Execution/PlanTypes.hxx>



#include <cstdint>

#include <string>

#include <string_view>

#include <vector>



namespace AstralDB {



[[nodiscard]] constexpr std::uint64_t SemistructuredCellStripKeyPk() noexcept { return 0xA57FA11B001ULL; }



[[nodiscard]] std::uint64_t SemistructuredCellStripKey(SQL::ScalarSqlFn Fn, std::string_view Aux) noexcept;



/** Projection fingerprint from fn + args only (SELECT aliases ignored). */

[[nodiscard]] std::uint64_t SemistructuredProjectionSemanticsFingerprint(

    const std::vector<SemistructuredProjectionSpec> &Projections, const Database::Column *Pk) noexcept;



[[nodiscard]] std::uint64_t SemistructuredProjectionLayoutFingerprint(

    const std::vector<SemistructuredProjectionSpec> &Projections,

    const Database::Column *Pk) noexcept;



[[nodiscard]] bool ProjectionsMatchStripPack(

    const BulkSyntheticProjectionStripPack &Pack,

    const std::vector<SemistructuredProjectionSpec> &Projections, const Database::Column *Pk) noexcept;



[[nodiscard]] bool ProjectionStripPackBuildable(const std::vector<SemistructuredProjectionSpec> &Projections,

                                                const Database::Column *Pk, bool HasDenseRank) noexcept;



/** Build or return cached strip pack for this projection layout + winner order (keyed by fingerprint). */

[[nodiscard]] bool EnsureProjectionStripPack(ColumnarTable &Col,

                                              const std::vector<SemistructuredProjectionSpec> &Projections,

                                              const std::vector<Database::Column> &Schema,

                                              const std::vector<std::size_t> &WinnerRowIndices,

                                              const BulkSyntheticProjectionStripPack *&OutPack) noexcept;



bool MaterializeRowStoreFromProjectionStrips(const BulkSyntheticProjectionStripPack &Pack, std::size_t TableRowCount,

                                             RowTable &Out) noexcept;



/** Cached strip zip when available; otherwise one-pass LUT → row maps (no strip arena). */

bool MaterializeWinnersLutRowStore(ColumnarTable &Col, const std::vector<SemistructuredProjectionSpec> &Projections,

                                   const std::vector<Database::Column> &Schema,

                                   const std::vector<std::size_t> &WinnerRowIndices, RowTable &Out) noexcept;



/**

 * Commit semistructured top-K on columnar storage without building \c RowStore (SELECT-finalize tail only).

 * \p OutColumnNames captures output column order for deferred materialization.

 */

[[nodiscard]] bool TryCommitSemistructuredColumnarResult(

    ColumnarTable &Col, const std::vector<SemistructuredProjectionSpec> &Projections,

    const std::vector<Database::Column> &Schema, std::uint32_t K) noexcept;



/** Materialize deferred semistructured result into \p Out when columnar commit was used. */

struct HybridTableSlot;

[[nodiscard]] bool EnsureSemistructuredRowStoreMaterialized(HybridTableSlot &Slot,

                                                            const std::vector<Database::Column> &Schema,

                                                            RowTable &Out) noexcept;



} // namespace AstralDB

