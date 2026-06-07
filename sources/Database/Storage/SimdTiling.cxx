#include <Database/Storage/SimdTiling.hxx>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace AstralDB {
namespace SimdTiling {
namespace {

std::size_t EnvSize(const char *Name, std::size_t Default) {
#if defined(_MSC_VER)
	char *Buf = nullptr;
	size_t Len = 0;
	if(_dupenv_s(&Buf, &Len, Name) != 0 || !Buf)
		return Default;
	char *End = nullptr;
	const unsigned long long N = std::strtoull(Buf, &End, 10);
	free(Buf);
	if(End == Buf || N == 0)
		return Default;
	return static_cast<std::size_t>(N);
#else
	if(const char *V = std::getenv(Name)) {
		char *End = nullptr;
		const unsigned long long N = std::strtoull(V, &End, 10);
		if(End != V && N > 0)
			return static_cast<std::size_t>(N);
	}
	return Default;
#endif
}

std::size_t RoundDownPow2(std::size_t V) noexcept {
	if(V <= 1)
		return 1;
	std::size_t P = 1;
	while((P << 1) <= V && (P << 1) != 0)
		P <<= 1;
	return P;
}

std::size_t ClampTile(std::size_t V, std::size_t MinV, std::size_t MaxV) noexcept {
	return (std::min)(MaxV, (std::max)(MinV, V));
}

TiledCachePlan PlanFromHardware(WorkloadClass Workload, std::size_t ElemBytes) {
	const auto &Hw = Superfetch::Hardware();
	const Simd::Arch Arch = Simd::DetectArch();
	TiledCachePlan P;
	const std::size_t Line = Hw.CacheLineBytes > 0 ? Hw.CacheLineBytes : 64;
	const std::size_t Elem = (std::max)(std::size_t{1}, ElemBytes);

	switch(Arch) {
	case Simd::Arch::Avx512:
		P.Mr = 4;
		P.Nr = 8;
		P.Unroll = 16;
		break;
	case Simd::Arch::Avx2:
		P.Mr = 4;
		P.Nr = 4;
		P.Unroll = 8;
		break;
	case Simd::Arch::Neon:
		P.Mr = 4;
		P.Nr = 4;
		P.Unroll = 4;
		break;
	default:
		P.Mr = 2;
		P.Nr = 2;
		P.Unroll = 4;
		break;
	}

	const std::size_t L1Budget = (std::max)(Line * 8, Hw.L1Bytes / 2);
	const std::size_t L2Budget = (std::max)(L1Budget * 4, Hw.L2Bytes / 2);
	const std::size_t L3Budget = (std::max)(L2Budget * 4, Hw.L3Bytes / 4);

	P.Kc = RoundDownPow2(L1Budget / (Elem * (P.Mr + 2)));
	P.Kc = ClampTile(P.Kc, 32, 2048);
	P.L1PanelElements = RoundDownPow2(L1Budget / Elem);
	P.L1PanelElements = ClampTile(P.L1PanelElements, 256, 65536);

	P.Mc = RoundDownPow2(L2Budget / (Elem * P.Kc));
	P.Mc = ClampTile(P.Mc, 512, 65536);
	P.L2BlockElements = RoundDownPow2(L2Budget / Elem);
	P.L2BlockElements = ClampTile(P.L2BlockElements, P.L1PanelElements, 1 << 20);

	P.Nc = RoundDownPow2(L3Budget / (Elem * P.Kc));
	P.Nc = ClampTile(P.Nc, 1024, 1 << 20);
	P.L3BlockElements = RoundDownPow2(L3Budget / Elem);
	P.L3BlockElements = ClampTile(P.L3BlockElements, P.L2BlockElements, 8 << 20);

	P.PrefetchRows = Superfetch::DefaultLookaheadRows;
	if(Workload == WorkloadClass::OlapWindow || Workload == WorkloadClass::MathSciNumeric)
		P.PrefetchRows = (std::min)(Superfetch::MaxLookaheadRows, P.PrefetchRows + 4);
	P.Hint = PrefetchHint::Temporal;
	if(Workload == WorkloadClass::OlapScan && P.L3BlockElements * Elem > Hw.L3Bytes / 2)
		P.Hint = PrefetchHint::NonTemporal;

	P.Mr = EnvSize("ASTRALDB_SIMD_MR", P.Mr);
	P.Nr = EnvSize("ASTRALDB_SIMD_NR", P.Nr);
	P.Kc = EnvSize("ASTRALDB_SIMD_KC", P.Kc);
	P.L1PanelElements = EnvSize("ASTRALDB_SIMD_L1_TILE", P.L1PanelElements);
	P.L2BlockElements = EnvSize("ASTRALDB_SIMD_L2_TILE", P.L2BlockElements);
	P.L3BlockElements = EnvSize("ASTRALDB_SIMD_L3_TILE", P.L3BlockElements);
	return P;
}

constexpr std::size_t kMaxWorkloadSlots = 8;
constexpr std::size_t kMaxElemSlots = 16;

struct PlanSlot {
	TiledCachePlan Plan;
	bool Valid = false;
};

PlanSlot g_PlanCache[kMaxWorkloadSlots][kMaxElemSlots];
Simd::KernelConfig g_CachedKernelConfig{};
bool g_KernelConfigValid = false;

std::size_t ElemSlotIndex(const std::size_t ElemBytes) noexcept {
	return (std::min)(ElemBytes, kMaxElemSlots - 1);
}

std::size_t WorkloadSlotIndex(const WorkloadClass Workload) noexcept {
	return (std::min)(static_cast<std::size_t>(Workload), kMaxWorkloadSlots - 1);
}

} // namespace

void InvalidatePlanCache() noexcept {
	for(std::size_t W = 0; W < kMaxWorkloadSlots; ++W) {
		for(std::size_t E = 0; E < kMaxElemSlots; ++E)
			g_PlanCache[W][E].Valid = false;
	}
	g_KernelConfigValid = false;
}

TiledCachePlan ActivePlan(const WorkloadClass Workload, const std::size_t ElemBytes) noexcept {
	const std::size_t Wi = WorkloadSlotIndex(Workload);
	const std::size_t Ei = ElemSlotIndex(ElemBytes);
	PlanSlot &Slot = g_PlanCache[Wi][Ei];
	if(!Slot.Valid) {
		Slot.Plan = PlanFromHardware(Workload, ElemBytes);
		Slot.Valid = true;
	}
	return Slot.Plan;
}

std::size_t L1PanelElements(const TiledCachePlan &Plan, const std::size_t ElemBytes) noexcept {
	const std::size_t Elem = (std::max)(std::size_t{1}, ElemBytes);
	return (std::max)(std::size_t{64}, Plan.L1PanelElements / Elem * Elem);
}

std::size_t L2BlockElements(const TiledCachePlan &Plan, const std::size_t ElemBytes) noexcept {
	const std::size_t Elem = (std::max)(std::size_t{1}, ElemBytes);
	return (std::max)(L1PanelElements(Plan, Elem), Plan.L2BlockElements / Elem * Elem);
}

void BeginTileScan(const std::size_t Count, const WorkloadClass Workload, const std::size_t ElemBytes,
                   SimdTileSession &Session) noexcept {
	Session = SimdTileSession{};
	Session.Count = Count;
	Session.Workload = Workload;
	Session.Plan = ActivePlan(Workload, ElemBytes);
	Session.Armed = Count >= Superfetch::ArmColumnThreshold;
	if(!Session.Armed)
		return;
	Superfetch::BeginColumnScan(Count, Session.Prefetch, Workload);
	Session.Prefetch.WindowUnroll = Session.Plan.Unroll;
	Session.Prefetch.Policy.Lookahead = Session.Plan.PrefetchRows;
	Session.Prefetch.Policy.Hint = Session.Plan.Hint;
}

void AdvanceTilePanel(SimdTileSession &Session, const std::size_t PanelBegin, const std::size_t PanelEnd) noexcept {
	Session.PanelBegin = PanelBegin;
	Session.PanelEnd = PanelEnd;
	if(PanelBegin == 0 || (PanelBegin % Session.Plan.L2BlockElements) == 0)
		Session.L2BlockBegin = PanelBegin;
}

void PrefetchPanelAhead(const SimdTileSession &Session, const void *Base, const std::size_t Index,
                        const std::size_t PanelBytes) noexcept {
	if(!Session.Armed || !Base || PanelBytes == 0)
		return;
	const auto *P = static_cast<const char *>(Base);
	const std::size_t Ahead = Index + Session.Plan.PrefetchRows * Session.Plan.Unroll;
	if(Ahead >= Session.Count)
		return;
	const std::size_t Offset = Ahead * (PanelBytes / (std::max)(Session.PanelEnd - Session.PanelBegin, std::size_t{1}));
	Superfetch::PrefetchRange(P + Offset, PanelBytes, Session.Plan.Hint);
}

void PrefetchStreamAhead(const SimdTileSession &Session, const void *Base, const std::size_t Index,
                         const std::size_t ElemBytes, const std::size_t Count) noexcept {
	if(!Session.Armed || !Base || Count == 0)
		return;
	const std::size_t Ahead = (std::min)(Session.Count, Index + Count);
	const auto *P = static_cast<const char *>(Base);
	Superfetch::PrefetchRange(P + Ahead * ElemBytes, Count * ElemBytes, Session.Plan.Hint);
}

void ApplyToKernelConfig(Simd::KernelConfig &Cfg, const WorkloadClass Workload) noexcept {
	const TiledCachePlan P = ActivePlan(Workload, sizeof(float));
	Cfg.GemvMr = static_cast<size_t>(P.Mr);
	Cfg.GemvKc = static_cast<size_t>(P.Kc);
	if(Cfg.GemvMr == 0)
		Cfg.GemvMr = 1;
	if(Cfg.GemvMr > 8)
		Cfg.GemvMr = 8;
	if(Cfg.GemvKc == 0)
		Cfg.GemvKc = 32;
}

const Simd::KernelConfig &CachedKernelConfig() noexcept {
	if(!g_KernelConfigValid) {
		g_CachedKernelConfig = Simd::KernelConfig{8, 4, 4, 384};
		ApplyToKernelConfig(g_CachedKernelConfig, WorkloadClass::MathSciNumeric);
		g_KernelConfigValid = true;
	}
	return g_CachedKernelConfig;
}

} // namespace SimdTiling
} // namespace AstralDB
