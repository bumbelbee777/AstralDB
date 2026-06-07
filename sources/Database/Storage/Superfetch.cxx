#include <Database/Storage/Superfetch.hxx>
#include <Database/Storage/SuperfetchWeights.inc>
#include <Database/Storage/SimdTiling.hxx>
#include <IO/Job.hxx>
#include <IO/SIMD.hxx>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <intrin.h>
#include <windows.h>
#include <xmmintrin.h>
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
#include <mach/mach_time.h>
#endif

namespace AstralDB {

namespace {

std::atomic<uint64_t> Issued_{0};
std::atomic<uint64_t> Misses_{0};
std::atomic<uint64_t> Backoffs_{0};
std::atomic<uint64_t> Increases_{0};
std::atomic<uint64_t> Disarmed_{0};
std::atomic<uint64_t> MlpForwardsGlobal_{0};
std::atomic<uint64_t> PolicySkips_{0};
std::atomic<uint64_t> AsyncBatches_{0};
std::atomic<bool> MlpDisabled_{false};

Superfetch::HardwareProfile g_Hw;
std::once_flag g_HwOnce;
std::once_flag g_AsyncOnce;

struct PrefetchJob {
	const void *Addr = nullptr;
	PrefetchHint Hint = PrefetchHint::Temporal;
};

static constexpr unsigned kAsyncRingDepth = 4;
PrefetchJob g_AsyncRing[kAsyncRingDepth];
std::atomic<unsigned> g_AsyncHead{0};
std::atomic<unsigned> g_AsyncTail{0};
std::atomic<bool> g_AsyncStop{false};
std::thread g_AsyncThread;
std::mutex g_AsyncMutex;
std::condition_variable g_AsyncCv;
thread_local unsigned g_AggregationDepth = 0;

const char *EnvGet(const char *Name) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
	return std::getenv(Name);
#pragma warning(pop)
#else
	return std::getenv(Name);
#endif
}

bool EnvEnabled(const char *Name, bool Default) {
	if(const char *V = EnvGet(Name)) {
		if(V[0] == '0' && V[1] == '\0')
			return false;
		if(V[0] == 'n' || V[0] == 'N')
			return false;
		return true;
	}
	return Default;
}

std::size_t EnvSize(const char *Name, std::size_t Default) {
	if(const char *V = EnvGet(Name)) {
		char *End = nullptr;
		const unsigned long long N = std::strtoull(V, &End, 10);
		if(End != V && N > 0)
			return static_cast<std::size_t>(N);
	}
	return Default;
}

uint64_t ReadTsc() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
	return __rdtsc();
#elif defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
	unsigned Hi = 0;
	unsigned Lo = 0;
	__asm__ __volatile__("rdtsc" : "=a"(Lo), "=d"(Hi));
	return (static_cast<uint64_t>(Hi) << 32) | Lo;
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
#if defined(__APPLE__)
	return mach_absolute_time();
#else
	uint64_t Cnt = 0;
	__asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(Cnt));
	return Cnt;
#endif
#else
	return 0;
#endif
}

void DetectHardware() {
	g_Hw.LogicalCores = (std::max)(1u, std::thread::hardware_concurrency());
	g_Hw.CacheLineBytes = 64;
#if defined(__unix__) || defined(__APPLE__)
	if(long L = sysconf(_SC_LEVEL1_DCACHE_LINESIZE); L > 0)
		g_Hw.CacheLineBytes = static_cast<std::size_t>(L);
#endif
	g_Hw.L1Bytes = 32 * 1024;
	g_Hw.L2Bytes = 256 * 1024;
	g_Hw.L3Bytes = 8 * 1024 * 1024;
#if defined(_MSC_VER)
	DWORD Len = 0;
	GetLogicalProcessorInformation(nullptr, &Len);
	if(GetLastError() == ERROR_INSUFFICIENT_BUFFER && Len > 0) {
		std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> Buf(Len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) + 1);
		if(GetLogicalProcessorInformation(Buf.data(), &Len)) {
			for(const auto &E : Buf) {
				if(E.Relationship == RelationCache && E.Cache.Level == 3)
					g_Hw.L3Bytes = (std::max)(g_Hw.L3Bytes, static_cast<std::size_t>(E.Cache.Size));
			}
		}
	}
	SYSTEM_INFO Si{};
	GetSystemInfo(&Si);
	g_Hw.NumaAvailable = Si.dwNumberOfProcessors > 1;
#endif
}

void IssuePrefetchLocality(const void *Address, int Locality) noexcept {
	if(!Address)
		return;
#if defined(__GNUC__) || defined(__clang__)
	switch(Locality) {
	case 0:
		__builtin_prefetch(Address, 0, 0);
		break;
	case 1:
		__builtin_prefetch(Address, 0, 1);
		break;
	case 2:
		__builtin_prefetch(Address, 0, 2);
		break;
	case 3:
		__builtin_prefetch(Address, 0, 3);
		break;
	default:
		__builtin_prefetch(Address, 0, 3);
		break;
	}
#elif defined(_MSC_VER)
	const char *P = reinterpret_cast<const char *>(Address);
	if(Locality == 0)
		_mm_prefetch(P, _MM_HINT_NTA);
	else if(Locality <= 1)
		_mm_prefetch(P, _MM_HINT_T1);
	else
		_mm_prefetch(P, _MM_HINT_T0);
#else
	(void)Locality;
	(void)Address;
#endif
	Issued_.fetch_add(1, std::memory_order_relaxed);
}

void AsyncWorker() noexcept {
	while(!g_AsyncStop.load(std::memory_order_acquire)) {
		PrefetchJob J{};
		bool Have = false;
		{
			std::unique_lock<std::mutex> Lock(g_AsyncMutex);
			g_AsyncCv.wait(Lock, []() {
				if(g_AsyncStop.load(std::memory_order_acquire))
					return true;
				const unsigned Tail = g_AsyncTail.load(std::memory_order_acquire);
				const unsigned Head = g_AsyncHead.load(std::memory_order_acquire);
				return Tail != Head;
			});
			if(g_AsyncStop.load(std::memory_order_acquire))
				break;
			const unsigned Tail = g_AsyncTail.load(std::memory_order_acquire);
			const unsigned Head = g_AsyncHead.load(std::memory_order_acquire);
			if(Tail != Head) {
				J = g_AsyncRing[Tail % kAsyncRingDepth];
				g_AsyncTail.store((Tail + 1) % kAsyncRingDepth, std::memory_order_release);
				Have = true;
			}
		}
		if(!Have)
			continue;
		int Loc = 3;
		if(J.Hint == PrefetchHint::NonTemporal)
			Loc = 0;
		else if(J.Hint == PrefetchHint::Spatial)
			Loc = 2;
		IssuePrefetchLocality(J.Addr, Loc);
	}
}

void EnsureAsyncThread() {
	std::call_once(g_AsyncOnce, []() {
		if(!EnvEnabled("ASTRALDB_SUPERFETCH_ASYNC", false))
			return;
		g_AsyncStop.store(false, std::memory_order_release);
		g_AsyncThread = std::thread(AsyncWorker);
	});
}

bool EnqueueAsync(const void *Addr, PrefetchHint Hint) {
	if(!Addr)
		return false;
	if(EnvEnabled("ASTRALDB_SUPERFETCH_ASYNC", false) && JobSystem::Instance().IsRunning()) {
		JobSystem::Instance().Submit([Addr, Hint]() {
			int Loc = 3;
			if(Hint == PrefetchHint::NonTemporal)
				Loc = 0;
			else if(Hint == PrefetchHint::Spatial)
				Loc = 2;
			IssuePrefetchLocality(Addr, Loc);
		});
		return true;
	}
	EnsureAsyncThread();
	std::lock_guard<std::mutex> Lock(g_AsyncMutex);
	const unsigned Head = g_AsyncHead.load(std::memory_order_relaxed);
	const unsigned Next = (Head + 1) % kAsyncRingDepth;
	const unsigned Tail = g_AsyncTail.load(std::memory_order_acquire);
	if(Next == Tail)
		return false;
	g_AsyncRing[Head % kAsyncRingDepth] = {Addr, Hint};
	g_AsyncHead.store(Next, std::memory_order_release);
	AsyncBatches_.fetch_add(1, std::memory_order_relaxed);
	g_AsyncCv.notify_one();
	return true;
}

float Relu(float X) { return X > 0.f ? X : 0.f; }

void MlpForward(const float Input[SuperfetchWeights::kIn], float Hidden[SuperfetchWeights::kHidden],
                float Output[SuperfetchWeights::kOut]) {
	for(std::size_t H = 0; H < SuperfetchWeights::kHidden; ++H) {
		float Sum = SuperfetchWeights::B1[H];
		for(std::size_t I = 0; I < SuperfetchWeights::kIn; ++I)
			Sum += Input[I] * SuperfetchWeights::W1[I * SuperfetchWeights::kHidden + H];
		Hidden[H] = Relu(Sum);
	}
	for(std::size_t O = 0; O < SuperfetchWeights::kOut; ++O) {
		float Sum = SuperfetchWeights::B2[O];
		for(std::size_t H = 0; H < SuperfetchWeights::kHidden; ++H)
			Sum += Hidden[H] * SuperfetchWeights::W2[H * SuperfetchWeights::kOut + O];
		Output[O] = Sum;
	}
	float MaxV = Output[0];
	for(std::size_t O = 1; O < SuperfetchWeights::kOut; ++O)
		MaxV = (std::max)(MaxV, Output[O]);
	float ExpSum = 0.f;
	for(std::size_t O = 0; O < SuperfetchWeights::kOut; ++O) {
		Output[O] = std::exp(Output[O] - MaxV);
		ExpSum += Output[O];
	}
	if(ExpSum > 0.f) {
		for(std::size_t O = 0; O < SuperfetchWeights::kOut; ++O)
			Output[O] /= ExpSum;
	}
}

float MlpEntropy(const float Output[SuperfetchWeights::kOut]) {
	float H = 0.f;
	for(std::size_t O = 0; O < SuperfetchWeights::kOut; ++O) {
		const float P = Output[O];
		if(P > 1e-6f)
			H -= P * std::log(P);
	}
	return H;
}

void BuildMlpInput(const Superfetch::RowScanSession &Session, std::size_t RowCount, WorkloadClass Workload,
                   float Out[SuperfetchWeights::kIn]) {
	const auto &Hw = Superfetch::Hardware();
	std::memset(Out, 0, sizeof(float) * SuperfetchWeights::kIn);
	Out[0] = std::log1p(static_cast<float>(RowCount));
	Out[1] = static_cast<float>(static_cast<int>(Workload)) / 4.f;
	Out[2] = static_cast<float>(Session.Lookahead) / static_cast<float>(Superfetch::MaxLookaheadRows);
	Out[3] = static_cast<float>(Hw.L3Bytes) / (64.f * 1024.f * 1024.f);
	Out[4] = static_cast<float>(Hw.LogicalCores) / 64.f;
	Out[5] = static_cast<float>(static_cast<int>(Simd::DetectArch())) / 7.f;
	Out[6] = Session.EmaRowCycles > 0 ? static_cast<float>(Session.EmaRowCycles) / 100'000.f : 0.f;
	Out[7] = 1.f;
}

Superfetch::ScanPolicy PolicyFromMlp(const float Output[SuperfetchWeights::kOut], std::size_t RowCount) {
	Superfetch::ScanPolicy P;
	const float Ent = MlpEntropy(Output);
	P.MlpConfidence = 1.f - Ent / std::log(static_cast<float>(SuperfetchWeights::kOut));
	int Best = 0;
	for(int O = 1; O < static_cast<int>(SuperfetchWeights::kOut); ++O)
		if(Output[O] > Output[Best])
			Best = O;
	const float LaNorm = Output[Best];
	{
		const std::size_t La = static_cast<std::size_t>(1 + LaNorm * static_cast<float>(Superfetch::MaxLookaheadRows - 1));
		P.Lookahead = (std::min)(Superfetch::MaxLookaheadRows, (std::max)(Superfetch::MinLookaheadRows, La));
	}
	if(Best == 1 || RowCount >= 1'000'000)
		P.Hint = PrefetchHint::NonTemporal;
	else if(Best == 2)
		P.Hint = PrefetchHint::Spatial;
	P.AsyncDepth = (Best == 3 && RowCount >= 100'000) ? 1u : 0u;
	P.ValidUntilIndex = RowCount;
	return P;
}

bool ShouldArmRow(std::size_t RowCount, WorkloadClass Workload) {
	if(!Superfetch::GlobalEnabled())
		return false;
	if(RowCount < Superfetch::ArmRowThreshold)
		return false;
	if(Workload == WorkloadClass::JoinNested && RowCount < 128)
		return false;
	const std::size_t EstBytes = RowCount * 128;
	if(EstBytes > static_cast<std::size_t>(0.85 * static_cast<double>(Superfetch::Hardware().L3Bytes)) &&
	   Workload == WorkloadClass::OltpRow)
		return false;
	return true;
}

} // namespace

bool Superfetch::GlobalEnabled() noexcept { return EnvEnabled("ASTRALDB_SUPERFETCH", true); }

const Superfetch::HardwareProfile &Superfetch::Hardware() noexcept {
	std::call_once(g_HwOnce, DetectHardware);
	return g_Hw;
}

Superfetch::Telemetry Superfetch::Stats() noexcept {
	return Telemetry{Issued_.load(std::memory_order_relaxed), Misses_.load(std::memory_order_relaxed),
	                 Backoffs_.load(std::memory_order_relaxed), Increases_.load(std::memory_order_relaxed),
	                 Disarmed_.load(std::memory_order_relaxed), MlpForwardsGlobal_.load(std::memory_order_relaxed),
	                 PolicySkips_.load(std::memory_order_relaxed), AsyncBatches_.load(std::memory_order_relaxed)};
}

void Superfetch::ResetTelemetry() noexcept {
	Issued_.store(0, std::memory_order_relaxed);
	Misses_.store(0, std::memory_order_relaxed);
	Backoffs_.store(0, std::memory_order_relaxed);
	Increases_.store(0, std::memory_order_relaxed);
	Disarmed_.store(0, std::memory_order_relaxed);
	MlpForwardsGlobal_.store(0, std::memory_order_relaxed);
	PolicySkips_.store(0, std::memory_order_relaxed);
	AsyncBatches_.store(0, std::memory_order_relaxed);
	MlpDisabled_.store(false, std::memory_order_relaxed);
}

uint64_t Superfetch::MlpForwardCountForTests() noexcept {
	return MlpForwardsGlobal_.load(std::memory_order_relaxed);
}

void Superfetch::Shutdown() noexcept {
	g_AsyncStop.store(true, std::memory_order_release);
	g_AsyncCv.notify_all();
	if(g_AsyncThread.joinable())
		g_AsyncThread.join();
}

Superfetch::AggregationScope::AggregationScope() noexcept { ++g_AggregationDepth; }

Superfetch::AggregationScope::~AggregationScope() noexcept {
	if(g_AggregationDepth > 0)
		--g_AggregationDepth;
}

bool Superfetch::InAggregationScope() noexcept { return g_AggregationDepth > 0; }

void Superfetch::IssuePrefetch(const void *Address, PrefetchHint Hint) noexcept {
	if(!Address || !GlobalEnabled() || InAggregationScope())
		return;
	if(EnvEnabled("ASTRALDB_SUPERFETCH_ASYNC", false) && EnqueueAsync(Address, Hint))
		return;
	if(Hint == PrefetchHint::Spatial) {
		const auto &Hw = Hardware();
		PrefetchRange(Address, Hw.CacheLineBytes * 2, Hint);
		return;
	}
	const int Loc = Hint == PrefetchHint::NonTemporal ? 0 : 3;
	IssuePrefetchLocality(Address, Loc);
}

void Superfetch::PrefetchRange(const void *Base, std::size_t Bytes, PrefetchHint Hint) noexcept {
	if(!Base || Bytes == 0)
		return;
	const auto &Hw = Hardware();
	const std::size_t Line = Hw.CacheLineBytes > 0 ? Hw.CacheLineBytes : 64;
	const auto *P = static_cast<const char *>(Base);
	const std::size_t Lines = (Bytes + Line - 1) / Line;
	const std::size_t Step = std::min<std::size_t>(Lines, 8);
	const int Loc = Hint == PrefetchHint::NonTemporal ? 0 : 3;
	for(std::size_t L = 0; L < Lines; L += Step)
		IssuePrefetchLocality(P + L * Line, Loc);
}

void Superfetch::DisarmSession(RowScanSession &Session, DisarmReason Reason) noexcept {
	Session.Armed = false;
	Session.LastDisarm = Reason;
	Session.Policy.AsyncDepth = 0;
	Disarmed_.fetch_add(1, std::memory_order_relaxed);
}

void Superfetch::ApplyScanPolicy(RowScanSession &Session, std::size_t RowCount, WorkloadClass Workload) noexcept {
	if(!Session.UseMlp || MlpDisabled_.load(std::memory_order_relaxed) || RowCount < ArmColumnThreshold ||
	   Session.MlpForwardCount >= MaxMlpForwardsPerScan) {
		Session.Policy.Lookahead = (std::min)(MaxLookaheadRows,
		                                      (std::max)(MinLookaheadRows, RowCount / 32 + MinLookaheadRows));
		if(RowCount >= 1'000'000 && Workload == WorkloadClass::OlapWindow)
			Session.Policy.Hint = PrefetchHint::NonTemporal;
		Session.Lookahead = Session.Policy.Lookahead;
		return;
	}
	float Input[SuperfetchWeights::kIn];
	float Hidden[SuperfetchWeights::kHidden];
	float Output[SuperfetchWeights::kOut];
	BuildMlpInput(Session, RowCount, Workload, Input);
	const auto T0 = ReadTsc();
	MlpForward(Input, Hidden, Output);
	const auto T1 = ReadTsc();
	if(T1 - T0 > 2000) {
		MlpDisabled_.store(true, std::memory_order_relaxed);
		PolicySkips_.fetch_add(1, std::memory_order_relaxed);
		Session.UseMlp = false;
		Session.Policy.Lookahead = (std::min)(MaxLookaheadRows,
		                                      (std::max)(MinLookaheadRows, RowCount / 32 + MinLookaheadRows));
		Session.Lookahead = Session.Policy.Lookahead;
		return;
	}
	++Session.MlpForwardCount;
	MlpForwardsGlobal_.fetch_add(1, std::memory_order_relaxed);
	Session.Policy = PolicyFromMlp(Output, RowCount);
	if(Session.Policy.MlpConfidence < 0.35f) {
		DisarmSession(Session, DisarmReason::LowMlpConfidence);
		return;
	}
	Session.Lookahead = Session.Policy.Lookahead;
	Session.Policy.ValidUntilIndex =
	    (std::min)(RowCount, EnvSize("ASTRALDB_SUPERFETCH_MLP_RETUNE", MlpRetuneIntervalRows));
}

void Superfetch::BeginRowScanCommon(std::size_t RowCount, WorkloadClass Workload, RowScanSession &Session) noexcept {
	if(InAggregationScope())
		return;
	Session = RowScanSession{};
	Session.RowCount = RowCount;
	Session.Workload = Workload;
	if(!ShouldArmRow(RowCount, Workload)) {
		Session.LastDisarm = GlobalEnabled() ? DisarmReason::TooSmall : DisarmReason::GlobalOff;
		if(Session.LastDisarm != DisarmReason::None)
			Disarmed_.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	Session.Armed = true;
	Session.Lookahead =
	    (std::min)(MaxLookaheadRows, (std::max)(MinLookaheadRows, RowCount / 32 + MinLookaheadRows));
	ApplyScanPolicy(Session, RowCount, Workload);
	Session.RowsSinceRetune = 0;
}

void Superfetch::AdvanceRowScanCommon(std::size_t Index, std::size_t RowCount, RowScanSession &Session) noexcept {
	if(Session.LastIndex != 0 && Index != Session.LastIndex + 1 && Index < 256) {
		if(Index > Session.LastIndex + 4 || (Index > 0 && Index < Session.LastIndex)) {
			DisarmSession(Session, DisarmReason::RandomAccess);
			return;
		}
	}
	Session.LastIndex = Index;
	++Session.WindowTicks;
	++Session.RowsSinceRetune;

	if(Session.RowsSinceRetune >= EnvSize("ASTRALDB_SUPERFETCH_MLP_RETUNE", MlpRetuneIntervalRows) &&
	   Session.MlpForwardCount < MaxMlpForwardsPerScan && Session.UseMlp) {
		if(Session.BaselineEma > 0 && Session.EmaRowCycles > 0) {
			const double Drift = std::abs(static_cast<double>(Session.EmaRowCycles) -
			                              static_cast<double>(Session.BaselineEma)) /
			                     static_cast<double>(Session.BaselineEma);
			if(Drift > 0.15)
				ApplyScanPolicy(Session, RowCount, Session.Workload);
		}
		Session.RowsSinceRetune = 0;
		Session.BaselineEma = Session.EmaRowCycles;
	}

	if((Session.WindowTicks & ((1u << TimingSampleShift) - 1)) != 0)
		return;

	const uint64_t Now = ReadTsc();
	if(Session.LastSampleTsc != 0) {
		const uint64_t Delta = Now - Session.LastSampleTsc;
		Session.EmaRowCycles = Session.EmaRowCycles == 0 ? Delta : (Session.EmaRowCycles * 7 + Delta) / 8;
		if(Session.EmaRowCycles < FastRowCycles && Session.RecentMisses == 0) {
			if(Session.Lookahead > MinLookaheadRows)
				Session.Lookahead = MinLookaheadRows;
			Session.Policy.Lookahead = Session.Lookahead;
			if(Session.EmaRowCycles < FastRowCycles / 2) {
				DisarmSession(Session, DisarmReason::HotCache);
				return;
			}
		}
		const uint64_t Budget = SlowRowCycles + Session.Lookahead * 4'000;
		if(Session.EmaRowCycles >= Budget * 3) {
			DisarmSession(Session, DisarmReason::SaturatedMemory);
			return;
		}
		if(Session.EmaRowCycles >= Budget) {
			if(Session.RecentMisses < 8)
				++Session.RecentMisses;
			Misses_.fetch_add(1, std::memory_order_relaxed);
			if(Session.RecentMisses >= 3 && Session.Lookahead > MinLookaheadRows) {
				Session.Lookahead = (std::max)(MinLookaheadRows, Session.Lookahead / 2);
				Session.Policy.Lookahead = Session.Lookahead;
				Session.RecentMisses = 0;
				Backoffs_.fetch_add(1, std::memory_order_relaxed);
			}
		} else if(Session.Lookahead < MaxLookaheadRows) {
			++Session.Lookahead;
			Session.Policy.Lookahead = Session.Lookahead;
			Increases_.fetch_add(1, std::memory_order_relaxed);
		}
	}
	Session.LastSampleTsc = Now;
}

void Superfetch::BeginColumnScan(std::size_t RowCount, ColumnScanSession &Session, WorkloadClass Workload) noexcept {
	Session = ColumnScanSession{};
	Session.RowCount = RowCount;
	const TiledCachePlan TilePlan = SimdTiling::ActivePlan(Workload, 32);
	Session.WindowUnroll = static_cast<unsigned>(EnvSize("ASTRALDB_WINDOW_UNROLL", TilePlan.Unroll));
	if(!GlobalEnabled() || RowCount < ArmColumnThreshold) {
		Session.LastDisarm = GlobalEnabled() ? DisarmReason::TooSmall : DisarmReason::GlobalOff;
		return;
	}
	const std::size_t EstBytes = RowCount * 32;
	if(EstBytes > static_cast<std::size_t>(0.85 * static_cast<double>(Hardware().L3Bytes)) &&
	   Workload == WorkloadClass::OltpRow) {
		Session.LastDisarm = DisarmReason::L3Pollution;
		return;
	}
	Session.Armed = true;
	Session.Policy.Lookahead = (std::min)(MaxLookaheadRows,
	                                      (std::max)(MinLookaheadRows, TilePlan.PrefetchRows));
	if(RowCount >= 1'000'000)
		Session.Policy.Hint = TilePlan.Hint;
}

void Superfetch::AdvanceColumnScan(ColumnScanSession &Session, const void *Addr, std::size_t Index) noexcept {
	if(!Session.Armed || !Addr)
		return;
	Session.Index = Index;
	if((Index & (Session.WindowUnroll - 1)) != 0)
		return;
	const std::size_t Ahead = Index + Session.Policy.Lookahead;
	if(Ahead < Session.RowCount)
		IssuePrefetch(reinterpret_cast<const char *>(Addr) + Hardware().CacheLineBytes, Session.Policy.Hint);
}

} // namespace AstralDB
