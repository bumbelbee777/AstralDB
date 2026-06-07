#pragma once

#include <Database/Storage/ColumnarStorage.hxx>
#include <Database/Storage/ColumnarLazyBulk.hxx>
#include <Database/Database.hxx>
#include <Database/Storage/ColumnFilterSimd.hxx>

#include <cstddef>
#include <string>
#include <vector>

namespace AstralDB {

/** Refresh zone maps after column append (incremental chunk). */
void RefreshColumnarZoneMaps(ColumnarTable &Col);

/** Single-column predicate scan with zone-map pruning + SIMD batches. */
[[nodiscard]] std::vector<std::size_t> FilterColumnarRows(const ColumnarTable &Col, const std::string &Column,
                                                          FilterCompareOp Op, const std::string &Literal);

/** Apply DNF filter on columnar storage; returns true if result written to \p OutRows. */
[[nodiscard]] bool TryFilterColumnarDnf(const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                                        const std::vector<std::vector<FilterPredicateTriple>> &Branches,
                                        RowTable &OutRows);

/** Compact columnar columns to \p Active row indices (mutates \p Col). */
void CompactColumnarByIndices(ColumnarTable &Col, const std::vector<std::size_t> &Active);

/** Apply DNF filter in-place on columnar storage (no row-store materialization). */
[[nodiscard]] bool TryFilterColumnarDnfInPlace(
    ColumnarTable &Col, const std::vector<std::vector<FilterPredicateTriple>> &Branches);

} // namespace AstralDB
