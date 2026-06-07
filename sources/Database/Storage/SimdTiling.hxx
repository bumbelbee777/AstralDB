#pragma once

#include <Database/Storage/Superfetch.hxx>
#include <IO/SIMD.hxx>

#include <cstddef>
#include <cstdint>
#include <functional>

namespace AstralDB {

/** OpenBLAS/MKL-style cache blocking plan derived from Superfetch hardware profile. */
struct TiledCachePlan {
	std::size_t Mr = 4;
	std::size_t Nr = 4;
	std::size_t Kc = 384;
	std::size_t Mc = 4096;
	std::size_t Nc = 4096;
	std::size_t L1PanelElements = 4096;
	std::size_t L2BlockElements = 65536;
	std::size_t L3BlockElements = 524288;
	std::size_t PrefetchRows = Superfetch::DefaultLookaheadRows;
	unsigned Unroll = 8;
	PrefetchHint Hint = PrefetchHint::Temporal;
};

/** SIMD scan session: Superfetch prefetch policy + active tile boundaries. */
struct SimdTileSession {
	Superfetch::ColumnScanSession Prefetch;
	TiledCachePlan Plan;
	std::size_t Count = 0;
	std::size_t PanelBegin = 0;
	std::size_t PanelEnd = 0;
	std::size_t L2BlockBegin = 0;
	WorkloadClass Workload = WorkloadClass::OlapScan;
	bool Armed = false;
};

namespace SimdTiling {

[[nodiscard]] TiledCachePlan ActivePlan(WorkloadClass Workload, std::size_t ElemBytes) noexcept;

void InvalidatePlanCache() noexcept;

[[nodiscard]] std::size_t L1PanelElements(const TiledCachePlan &Plan, std::size_t ElemBytes) noexcept;

[[nodiscard]] std::size_t L2BlockElements(const TiledCachePlan &Plan, std::size_t ElemBytes) noexcept;

void BeginTileScan(std::size_t Count, WorkloadClass Workload, std::size_t ElemBytes, SimdTileSession &Session) noexcept;

void AdvanceTilePanel(SimdTileSession &Session, std::size_t PanelBegin, std::size_t PanelEnd) noexcept;

void PrefetchPanelAhead(const SimdTileSession &Session, const void *Base, std::size_t Index,
                        std::size_t PanelBytes) noexcept;

void PrefetchStreamAhead(const SimdTileSession &Session, const void *Base, std::size_t Index,
                         std::size_t ElemBytes, std::size_t Count) noexcept;

/** L1-panel loop with optional merge hook (OpenBLAS-style panel + macro-kernel). */
template <typename PanelFn, typename MergeFn>
inline void ForL1Panels(std::size_t Count, std::size_t ElemBytes, WorkloadClass Workload, PanelFn &&RunPanel,
                        MergeFn &&MergePanel) {
	TiledCachePlan Plan = ActivePlan(Workload, ElemBytes);
	const std::size_t Panel = L1PanelElements(Plan, ElemBytes);
	if(Panel == 0 || Count == 0)
		return;
	SimdTileSession Session;
	BeginTileScan(Count, Workload, ElemBytes, Session);
	for(std::size_t Begin = 0; Begin < Count; Begin += Panel) {
		const std::size_t End = (Begin + Panel < Count) ? Begin + Panel : Count;
		AdvanceTilePanel(Session, Begin, End);
		RunPanel(Begin, End, Plan, Session);
		MergePanel();
	}
}

/** Apply \c ActivePlan to \c Simd::KernelConfig GEMV blocking (KC/MR). */
void ApplyToKernelConfig(Simd::KernelConfig &Cfg, WorkloadClass Workload = WorkloadClass::MathSciNumeric) noexcept;

/** Cached kernel config (tiling plan applied once per process). */
[[nodiscard]] const Simd::KernelConfig &CachedKernelConfig() noexcept;

} // namespace SimdTiling

} // namespace AstralDB
