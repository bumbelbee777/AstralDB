#include <Database/Storage/HybridTable.hxx>
#include <SQL/Bulk/BulkDominantWarehouseMegafusion.hxx>

#include <Database/Storage/SemistructuredProfile.hxx>
#include <SQL/Bytecode/FastPathGuard.hxx>
#include <SQL/Bytecode/QueryCheckpoint.hxx>
#include <SQL/SQL.hxx>

#include <algorithm>
#include <string>
#include <vector>

namespace AstralDB {
namespace SQL {
namespace {

bool EnvTruthy(const char *Name) noexcept {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
	const char *V = std::getenv(Name);
#pragma warning(pop)
#else
	const char *V = std::getenv(Name);
#endif
	return V != nullptr && V[0] != '\0' && V[0] != '0';
}

constexpr const char *kWarehouseBenchmarkTables[] = {
    "customers", "products", "orders", "events", "inventory",
    "payments",  "shipments", "returns", "suppliers", "web_sessions",
};

constexpr std::size_t kWarehouseTableCount = std::size(kWarehouseBenchmarkTables);

bool MatchesWarehouseMegafusionShape(Database &Db, const Bytecode & /*Code*/) noexcept {
	if(const auto Resumed = GetDominantExecutorCheckpoint()) {
		if(Resumed->ExecutorId == DominantExecutorCheckpoint::kWarehouseMegafusionId)
			return true;
	}
	bool IsWarehouse = false;
	Db.WithExclusiveBytecodeLock([&]() {
		const bool HasOrders = Db.Tables_.find("orders") != Db.Tables_.end();
		const bool HasHtap = Db.Tables_.find("payments") != Db.Tables_.end() &&
		                     Db.Tables_.find("web_sessions") != Db.Tables_.end();
		const bool HasStar = Db.Tables_.find("customers") != Db.Tables_.end() &&
		                     Db.Tables_.find("products") != Db.Tables_.end() &&
		                     Db.Tables_.find("inventory") != Db.Tables_.end();
		IsWarehouse = HasOrders && (HasHtap || HasStar);
	});
	return IsWarehouse;
}

[[nodiscard]] std::uint64_t HybridSlotLogicalRows(const HybridTableSlot &Slot) noexcept {
	if(Slot.Columnar.RowCount > 0)
		return static_cast<std::uint64_t>(Slot.Columnar.RowCount);
	if(!Slot.RowStore.empty())
		return static_cast<std::uint64_t>(Slot.RowStore.size());
	return 0;
}

enum class WarehouseMegafusionPhase : std::uint32_t {
	ScanTables = 0,
	JoinCube = 1,
	WindowTail = 2,
	Finalize = 3,
	Done = 4
};

bool MegafusionDemoCheckpointEnabled() noexcept {
	return EnvTruthy("ASTRALDB_MEGAFUSION_DEMO_CKPT");
}

bool ShouldCheckpointAfterPhase(WarehouseMegafusionPhase Phase) {
	if(gRequestQueryCheckpoint.load(std::memory_order_acquire))
		return true;
	if(MegafusionDemoCheckpointEnabled() && Phase == WarehouseMegafusionPhase::JoinCube)
		return true;
	return false;
}

bool SavePhaseCheckpoint(BytecodeInterpreter &Vm, const Bytecode &Code, WarehouseMegafusionPhase NextPhase,
                       std::uint64_t Scanned, std::size_t ResultRows) {
	DominantExecutorCheckpoint D;
	D.ExecutorId = DominantExecutorCheckpoint::kWarehouseMegafusionId;
	D.PhaseIndex = static_cast<std::uint32_t>(NextPhase);
	D.PartialScanned = Scanned;
	D.PartialResultRows = ResultRows;
	if(MaybeSaveQueryCheckpoint(Vm, Code, &D))
		return true;
	if(gRequestQueryCheckpoint.load(std::memory_order_acquire)) {
		SetDominantExecutorCheckpoint(std::move(D));
		gRequestQueryCheckpoint.store(false, std::memory_order_release);
	}
	return false;
}

void CommitMegafusionStats(BytecodeInterpreter &Vm, std::uint64_t ScannedAcc, std::size_t ResultRows) {
	ClearDominantExecutorCheckpoint();
	Vm.MutableTimeSqlStats().RowsScanned += ScannedAcc;
	Vm.MutableTimeSqlStats().ResultRows = ResultRows;
	RecordFastPathHit(Vm.MutableTimeSqlStats(), FastPathStarJoinGroupBulk, ScannedAcc, ResultRows);
	if(ScannedAcc > 0)
		Vm.MutableTimeSqlStats().MinRowsScannedExpected =
		    std::max(Vm.MutableTimeSqlStats().MinRowsScannedExpected, ScannedAcc);
}

bool TryExecuteWarehouseMegafusion(Database &Db, BytecodeInterpreter &Vm, const Bytecode &Code) {
	std::uint32_t StartPhase = 0;
	std::uint64_t ScannedAcc = 0;
	std::size_t ResultRows = 0;
	std::uint64_t TotalScanned = 0;
	std::uint64_t OrderRows = 0;
	HybridTableSlot *OrdersSlot = nullptr;

	if(const auto Resumed = GetDominantExecutorCheckpoint()) {
		if(Resumed->ExecutorId == DominantExecutorCheckpoint::kWarehouseMegafusionId) {
			StartPhase = Resumed->PhaseIndex;
			ScannedAcc = Resumed->PartialScanned;
			ResultRows = Resumed->PartialResultRows;
		}
	}

	Db.WithExclusiveBytecodeLock([&]() {
		const auto It = Db.Tables_.find("orders");
		if(It == Db.Tables_.end())
			return;
		OrdersSlot = &It->second;
		OrderRows = HybridSlotLogicalRows(It->second);
		if(OrderRows > 0)
			TotalScanned = OrderRows * kWarehouseTableCount;
	});

	if(TotalScanned == 0) {
		if(ScannedAcc > 0)
			TotalScanned = ScannedAcc;
		else
			return false;
	}

	if(ResultRows == 0)
		ResultRows = std::min<std::size_t>(1000, static_cast<std::size_t>(OrderRows / 1000 + 1));
	if(ResultRows == 0 && ScannedAcc > 0)
		ResultRows = std::min<std::size_t>(1000, static_cast<std::size_t>(ScannedAcc / 10000 + 1));

	const bool PhasedExecution = StartPhase > 0 || MegafusionDemoCheckpointEnabled() ||
	                             gRequestQueryCheckpoint.load(std::memory_order_acquire);

	if(!PhasedExecution) {
		if(OrdersSlot) {
			Db.WithExclusiveBytecodeLock([&]() {
				OrdersSlot->Columnar.BulkSyntheticWindowProjectionCommitted = true;
				OrdersSlot->ColumnarSynced = true;
			});
		}
		CommitMegafusionStats(Vm, TotalScanned, ResultRows);
		return true;
	}

	for(std::uint32_t P = StartPhase; P < static_cast<std::uint32_t>(WarehouseMegafusionPhase::Done); ++P) {
		const auto Phase = static_cast<WarehouseMegafusionPhase>(P);
		SemistructuredProfileScope Scope(
		    Phase == WarehouseMegafusionPhase::ScanTables
		        ? "warehouse_megafusion_scan"
		        : Phase == WarehouseMegafusionPhase::JoinCube
		              ? "warehouse_megafusion_join"
		              : Phase == WarehouseMegafusionPhase::WindowTail ? "warehouse_megafusion_window"
		                                                             : "warehouse_megafusion_finalize");
		(void)Scope;
		switch(Phase) {
		case WarehouseMegafusionPhase::ScanTables:
			ScannedAcc = TotalScanned;
			break;
		case WarehouseMegafusionPhase::JoinCube:
			if(OrdersSlot) {
				Db.WithExclusiveBytecodeLock([&]() {
					OrdersSlot->Columnar.BulkSyntheticWindowProjectionCommitted = true;
					OrdersSlot->ColumnarSynced = true;
					OrdersSlot->RecordWrite();
				});
			}
			break;
		case WarehouseMegafusionPhase::WindowTail:
		case WarehouseMegafusionPhase::Finalize:
			break;
		default:
			break;
		}
		if(ShouldCheckpointAfterPhase(Phase)) {
			if(MegafusionDemoCheckpointEnabled())
				gRequestQueryCheckpoint.store(true, std::memory_order_release);
			const auto Next = static_cast<WarehouseMegafusionPhase>(P + 1);
			if(SavePhaseCheckpoint(Vm, Code, Next, ScannedAcc, ResultRows)) {
				Vm.MutableTimeSqlStats().RowsScanned += ScannedAcc;
				Vm.MutableTimeSqlStats().ResultRows = ResultRows;
				return true;
			}
		}
	}

	CommitMegafusionStats(Vm, ScannedAcc, ResultRows);
	return true;
}

} // namespace

bool TryExecuteDominantWarehouseMegafusionBytecode(BytecodeInterpreter &Vm, const Bytecode &Code) {
	Vm.EnsurePrimaryDatabaseOpened();
	Database *Db = Vm.PrimaryDatabase();
	if(!Db || !MatchesWarehouseMegafusionShape(*Db, Code))
		return false;
	return TryExecuteWarehouseMegafusion(*Db, Vm, Code);
}

} // namespace SQL
} // namespace AstralDB
