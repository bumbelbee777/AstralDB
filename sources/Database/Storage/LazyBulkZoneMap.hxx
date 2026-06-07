#pragma once

#include <Database/Database.hxx>
#include <Database/Storage/ColumnZoneMap.hxx>

#include <cstdint>
#include <vector>

namespace AstralDB {

/** Zone maps for lazy bulk synthetic tables (formula min/max, no column storage). */
void RebuildLazyBulkZoneMaps(std::vector<RowGroupStats> &Out, const std::vector<Database::Column> &Schema,
                             std::size_t RowCount, int64_t BulkStartId, int64_t BulkStep);

} // namespace AstralDB
