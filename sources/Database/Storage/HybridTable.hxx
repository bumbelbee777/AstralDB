#pragma once

#include <Database/Storage/ColumnarStorage.hxx>
#include <Database/Storage/SemistructuredResultStripsTypes.hxx>
#include <Database/Storage/CompressedColumnStore.hxx>
#include <Database/Storage/HybridStorageScheduler.hxx>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace AstralDB {

class Database;

/** Per-table hybrid storage slot: row store (OLTP) plus optional columnar replica. */
struct HybridTableSlot {
	using Item = std::unordered_map<std::string, std::string>;
	using Table = std::vector<Item>;

	Table RowStore;
	ColumnarTable Columnar;
	CompressedColumnStore Compressed;
	bool ColumnarSynced = false;
	bool CompressedSynced = false;
	StorageLayout DeclaredPolicy = StorageLayout::Row;
	/** Set by lazy inner-join count-only fast path; consumed by scalar \c COUNT(*) \c GROUP BY. */
	std::optional<std::uint64_t> SyntheticJoinMatchCount;
	/** Join rows were skipped; a warehouse star GROUP BY pass will read base lazy-bulk tables. */
	mutable TableWorkloadCounters Workload;
	HybridStorageScheduler Scheduler;
	/** Semistructured top-K committed without \c RowStore (SELECT-finalize-only bytecode). */
	bool SemistructuredResultCommitted = false;
	std::uint32_t SemistructuredResultK = 0;
	std::vector<SemistructuredProjectionSpec> SemistructuredResultProjections;
	std::string DeferredStarJoinFactTable;
	std::vector<std::string> DeferredStarJoinPassthroughCols;
	/** Single-row RHS from lazy CROSS JOIN broadcast (parameter columns constant per LHS row). */
	std::optional<Item> BroadcastCrossJoinRhsRow;

	operator Table &() { return RowStore; }
	operator const Table &() const { return RowStore; }
	HybridTableSlot &operator=(Table &&Moved) {
		RowStore = std::move(Moved);
		ColumnarSynced = false;
		CompressedSynced = false;
		return *this;
	}
	HybridTableSlot &operator=(const Table &Copied) {
		RowStore = Copied;
		ColumnarSynced = false;
		CompressedSynced = false;
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
	void EnsureRowStoreFromColumnar(const Database *Db, const std::string &TableName);
	const Table &RowsForRead(std::optional<StorageLayout> QueryHint, bool AggregateQuery) const;
};

} // namespace AstralDB
