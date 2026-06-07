#include <Database/Storage/HybridTable.hxx>

#include <Database/Storage/SemistructuredResultStrips.hxx>
#include <Database/Storage/ColumnarLazyBulk.hxx>
#include <Database/Database.hxx>
#include <algorithm>
#include <unordered_set>

namespace AstralDB {

namespace {

float EstimateCardinalityFromRows(const HybridTableSlot::Table &RowStore) {
	if(RowStore.empty())
		return 0.f;
	std::unordered_map<std::string, std::unordered_set<std::string>> Distinct;
	for(const auto &Row : RowStore) {
		for(const auto &[Col, Val] : Row)
			Distinct[Col].insert(Val);
	}
	float Sum = 0.f;
	for(const auto &[Col, Set] : Distinct) {
		(void)Col;
		Sum += static_cast<float>(Set.size());
	}
	return Sum / static_cast<float>(Distinct.empty() ? 1 : Distinct.size()) / static_cast<float>(RowStore.size());
}

} // namespace

void HybridTableSlot::SetDeclaredPolicy(StorageLayout Policy) {
	DeclaredPolicy = Policy;
	if(Policy == StorageLayout::Columnar || Policy == StorageLayout::Hybrid)
		RebuildColumnarFromRows();
}

StorageLayout HybridTableSlot::EffectiveLayout(std::optional<StorageLayout> QueryHint,
                                               bool AggregateQuery) const {
	if(QueryHint.has_value() && *QueryHint != StorageLayout::Auto)
		return *QueryHint;
	switch(DeclaredPolicy) {
	case StorageLayout::Row:
		return StorageLayout::Row;
	case StorageLayout::Columnar:
		return StorageLayout::Columnar;
	case StorageLayout::Hybrid:
		if(AggregateQuery)
			return StorageLayout::Columnar;
		return StorageLayout::Row;
	case StorageLayout::Auto: {
		WorkloadFeatures F = Scheduler.BuildFeatures(Workload);
		if(AggregateQuery)
			F.QueryPatternScore = std::max(F.QueryPatternScore, 0.75f);
		return Scheduler.PredictLayout(F);
	}
	}
	return StorageLayout::Row;
}

void HybridTableSlot::RecordWrite() {
	++Workload.WriteCount;
	Workload.RowCountPeak = std::max(Workload.RowCountPeak, RowStore.size());
	Workload.CardinalityEstimate = EstimateCardinalityFromRows(RowStore);
}

void HybridTableSlot::RecordRead(bool AggregateQuery) const {
	++Workload.ReadCount;
	if(AggregateQuery)
		++Workload.AggregateReadCount;
}

void HybridTableSlot::RebuildColumnarFromRows() {
	Columnar.RebuildFromRows(RowStore);
	ColumnarSynced = true;
}

void HybridTableSlot::SyncColumnarAfterRowMutation() {
	if(DeclaredPolicy == StorageLayout::Columnar || DeclaredPolicy == StorageLayout::Hybrid ||
	   DeclaredPolicy == StorageLayout::Auto) {
		if(RowStore.empty() && Columnar.RowCount > 0)
			ColumnarSynced = true;
		else
			RebuildColumnarFromRows();
	} else
		ColumnarSynced = false;
}

void HybridTableSlot::EnsureRowStoreFromColumnar() {
	if(Columnar.RowCount == 0)
		return;
	if(RowStore.size() == Columnar.RowCount && !RowStore.empty() && !RowStore.front().empty())
		return;
	RowStore = Columnar.MaterializeAllRows();
	ColumnarSynced = true;
}

void HybridTableSlot::EnsureRowStoreFromColumnar(const Database *Db, const std::string &TableName) {
	if(Columnar.RowCount == 0)
		return;
	if(RowStore.size() == Columnar.RowCount && !RowStore.empty() && !RowStore.front().empty())
		return;
	if(Columnar.BulkSyntheticLazy && Db != nullptr) {
		const auto Sch = Db->TableSchemaAssumeDbMutexHeld(TableName);
		if(Sch && !Sch->empty()) {
			if(SemistructuredResultCommitted &&
			   EnsureSemistructuredRowStoreMaterialized(*this, *Sch, RowStore)) {
				ColumnarSynced = true;
				return;
			}
			MaterializeLazyBulkToRowStore(Columnar, *Sch, Db, TableName, RowStore);
			ColumnarSynced = true;
			return;
		}
	}
	EnsureRowStoreFromColumnar();
}

const HybridTableSlot::Table &HybridTableSlot::RowsForRead(std::optional<StorageLayout> QueryHint,
                                                           bool AggregateQuery) const {
	if(RowStore.empty() && Columnar.RowCount > 0)
		const_cast<HybridTableSlot *>(this)->EnsureRowStoreFromColumnar();
	RecordRead(AggregateQuery);
	const StorageLayout Eff = EffectiveLayout(QueryHint, AggregateQuery);
	const WorkloadFeatures F = Scheduler.BuildFeatures(Workload);
	const float CostStart = Scheduler.EstimateCost(Eff, F);

	if(Eff == StorageLayout::Columnar) {
		if(!ColumnarSynced)
			const_cast<HybridTableSlot *>(this)->RebuildColumnarFromRows();
		if(Columnar.BulkSyntheticLazy && RowStore.empty()) {
			const_cast<HybridStorageScheduler &>(Scheduler)
			    .ObserveOutcome(F, StorageLayout::Columnar, CostStart * 0.15f);
			return RowStore;
		}
		if(DeclaredPolicy == StorageLayout::Auto || DeclaredPolicy == StorageLayout::Hybrid) {
			const_cast<HybridTableSlot *>(this)->RowStore = Columnar.MaterializeAllRows();
			const_cast<HybridStorageScheduler &>(Scheduler)
			    .ObserveOutcome(F, StorageLayout::Columnar, CostStart * 0.85f);
		}
		return RowStore;
	}

	if(DeclaredPolicy == StorageLayout::Auto)
		const_cast<HybridStorageScheduler &>(Scheduler).ObserveOutcome(F, StorageLayout::Row, CostStart);
	return RowStore;
}

} // namespace AstralDB
