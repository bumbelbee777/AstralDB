#pragma once

#include <Database/ColumnarStorage.hxx>
#include <Database/HybridStorageScheduler.hxx>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace AstralDB {

/** Per-table hybrid storage slot: row store (OLTP) plus optional columnar replica. */
struct HybridTableSlot {
	using Item = std::unordered_map<std::string, std::string>;
	using Table = std::vector<Item>;

	Table RowStore;
	ColumnarTable Columnar;
	bool ColumnarSynced = false;
	StorageLayout DeclaredPolicy = StorageLayout::Row;
	mutable TableWorkloadCounters Workload;
	HybridStorageScheduler Scheduler;

	operator Table &() { return RowStore; }
	operator const Table &() const { return RowStore; }
	HybridTableSlot &operator=(Table &&Moved) {
		RowStore = std::move(Moved);
		ColumnarSynced = false;
		return *this;
	}
	HybridTableSlot &operator=(const Table &Copied) {
		RowStore = Copied;
		ColumnarSynced = false;
		return *this;
	}

	void SetDeclaredPolicy(StorageLayout Policy);
	StorageLayout EffectiveLayout(std::optional<StorageLayout> QueryHint, bool AggregateQuery) const;
	void RecordWrite();
	void RecordRead(bool AggregateQuery) const;
	void RebuildColumnarFromRows();
	void SyncColumnarAfterRowMutation();
	/** When bulk used columnar-only append, build row store for VM scans. */
	void EnsureRowStoreFromColumnar();
	const Table &RowsForRead(std::optional<StorageLayout> QueryHint, bool AggregateQuery) const;
};

} // namespace AstralDB
