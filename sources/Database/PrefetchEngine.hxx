#pragma once

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#if defined(_MSC_VER)
#include <xmmintrin.h>
#endif

namespace AstralDB {

/** Miss-hardened prefetch: near-zero cost on small scans; adapts lookahead when row access stalls. */
struct PrefetchEngine {
	static constexpr std::size_t ArmRowThreshold = 48;
	static constexpr std::size_t DefaultLookaheadRows = 4;
	static constexpr std::size_t MaxLookaheadRows = 12;
	static constexpr std::size_t MinLookaheadRows = 1;
	static constexpr uint64_t SlowRowNs = 12'000;
	static constexpr unsigned MissDecayMask = 31;

	struct RowScanSession {
		std::size_t Lookahead = DefaultLookaheadRows;
		uint64_t LastRowNs = 0;
		unsigned RecentMisses = 0;
		unsigned WindowTicks = 0;
		bool Armed = false;
	};

	struct JoinScanSession {
		RowScanSession Outer;
		RowScanSession Inner;
	};

	struct Telemetry {
		uint64_t PrefetchIssued = 0;
		uint64_t MissSignals = 0;
		uint64_t LookaheadBackoff = 0;
	};

	static Telemetry Stats() noexcept { return Telemetry{Issued_.load(std::memory_order_relaxed),
	                                                     Misses_.load(std::memory_order_relaxed),
	                                                     Backoffs_.load(std::memory_order_relaxed)}; }

	static void ResetTelemetry() noexcept {
		Issued_.store(0, std::memory_order_relaxed);
		Misses_.store(0, std::memory_order_relaxed);
		Backoffs_.store(0, std::memory_order_relaxed);
	}

	template <typename Row>
	static void TouchRow(const Row &RowRef) noexcept {
		if(RowRef.empty())
			return;
		const auto It = RowRef.begin();
		(void)It->first.size();
		(void)It->second.size();
	}

	template <typename Table>
	static void BeginRowScan(const Table &TableRef, RowScanSession &Session) noexcept {
		Session = RowScanSession{};
		if(TableRef.size() < ArmRowThreshold)
			return;
		Session.Armed = true;
		Session.Lookahead =
		    std::clamp(TableRef.size() / 32 + MinLookaheadRows, MinLookaheadRows, MaxLookaheadRows);
		if(!TableRef.empty())
			PrefetchRowStorage(TableRef, 0);
	}

	template <typename Table>
	static void AdvanceRowScan(const Table &TableRef, std::size_t Index, RowScanSession &Session) noexcept {
		if(!Session.Armed || Index >= TableRef.size())
			return;
		const uint64_t Now = SteadyNs();
		if(Session.LastRowNs != 0) {
			const uint64_t Delta = Now - Session.LastRowNs;
			if(Delta >= SlowRowNs) {
				++Session.RecentMisses;
				Misses_.fetch_add(1, std::memory_order_relaxed);
				if(Session.RecentMisses >= 6 && Session.Lookahead > MinLookaheadRows) {
					Session.Lookahead = std::max(MinLookaheadRows, Session.Lookahead / 2);
					Session.RecentMisses = 0;
					Backoffs_.fetch_add(1, std::memory_order_relaxed);
				}
			} else if(Session.Lookahead < MaxLookaheadRows && (Session.WindowTicks & 7u) == 0u)
				++Session.Lookahead;
		}
		Session.LastRowNs = Now;
		++Session.WindowTicks;
		if((Session.WindowTicks & MissDecayMask) == 0u && Session.RecentMisses > 0)
			--Session.RecentMisses;

		const std::size_t Ahead = Index + Session.Lookahead;
		if(Ahead < TableRef.size())
			PrefetchRowStorage(TableRef, Ahead);
		TouchRow(TableRef[Index]);
	}

	template <typename Table>
	static void PrefetchAppendTarget(const Table &TableRef) noexcept {
		if(TableRef.empty())
			return;
		PrefetchRowStorage(TableRef, TableRef.size() - 1);
		CpuPrefetch(TableRef.data());
	}

	template <typename Table>
	static void BeginJoinScan(const Table &Outer, const Table &Inner, JoinScanSession &Session) noexcept {
		BeginRowScan(Outer, Session.Outer);
		BeginRowScan(Inner, Session.Inner);
		if(Session.Inner.Armed && Session.Inner.Lookahead < MaxLookaheadRows)
			Session.Inner.Lookahead = std::min(MaxLookaheadRows, Session.Inner.Lookahead + 2);
	}

	template <typename Table>
	static void AdvanceJoinOuter(const Table &Outer, std::size_t Index, JoinScanSession &Session) noexcept {
		AdvanceRowScan(Outer, Index, Session.Outer);
	}

	template <typename Table>
	static void AdvanceJoinInner(const Table &Inner, std::size_t Index, JoinScanSession &Session) noexcept {
		AdvanceRowScan(Inner, Index, Session.Inner);
	}

private:
	static inline std::atomic<uint64_t> Issued_{0};
	static inline std::atomic<uint64_t> Misses_{0};
	static inline std::atomic<uint64_t> Backoffs_{0};

	static uint64_t SteadyNs() noexcept {
		using Clock = std::chrono::steady_clock;
		return static_cast<uint64_t>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
	}

	static void CpuPrefetch(const void *Address) noexcept {
		if(!Address)
			return;
#if defined(__GNUC__) || defined(__clang__)
		__builtin_prefetch(Address, 0, 3);
#elif defined(_MSC_VER)
		_mm_prefetch(reinterpret_cast<const char *>(Address), _MM_HINT_T0);
#else
		(void)Address;
#endif
		Issued_.fetch_add(1, std::memory_order_relaxed);
	}

	template <typename Table>
	static void PrefetchRowStorage(const Table &TableRef, std::size_t Index) noexcept {
		if(Index >= TableRef.size())
			return;
		CpuPrefetch(&TableRef[Index]);
		const auto &Row = TableRef[Index];
		CpuPrefetch(&Row);
		if(Row.empty())
			return;
		const auto It = Row.begin();
		CpuPrefetch(&It->first);
		CpuPrefetch(&It->second);
	}
};

} // namespace AstralDB
