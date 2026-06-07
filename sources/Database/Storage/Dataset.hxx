#pragma once

#include <Database/Storage/ColumnarStorage.hxx>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace AstralDB {

enum class DatasetKind : std::uint8_t { TableRef = 0, BulkFixture = 1 };

struct DatasetEntry {

	using Item = std::unordered_map<std::string, std::string>;

	using Table = std::vector<Item>;

	DatasetKind Kind = DatasetKind::TableRef;

	std::string SourceTable;

	Table SnapshotRows;

	/** When true, \c SnapshotColumnar holds a lazy-bulk snapshot (no \c SnapshotRows materialization). */
	bool SnapshotLazyColumnar = false;

	ColumnarTable SnapshotColumnar;

	int64_t BulkCount = 0;

	int64_t BulkStart = 1;

	int64_t BulkStep = 1;

	/** Monotonic version id within a named dataset lineage (starts at 1). */

	int64_t VersionId = 1;

	/** Epoch milliseconds when this version was registered. */

	int64_t CreatedAtMs = 0;

};



/** All versions of a named dataset; latest is \c Versions.back() . */

struct DatasetCatalog {

	std::vector<DatasetEntry> Versions;

};



} // namespace AstralDB

