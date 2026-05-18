#pragma once

#include <atomic>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#include <xmmintrin.h>
#endif

namespace AstralDB {

/** Miss-hardened prefetch: no-op on small scans; sparse staged hints on large row/join walks. */
struct Superfetch {
	static constexpr std::size_t ArmRowThreshold = 64;
	static constexpr std::size_t DefaultLookaheadRows = 4;
	static constexpr std::size_t MaxLookaheadRows = 12;
	static constexpr std::size_t MinLookaheadRows = 1;
	static constexpr unsigned RowPrefetchStride = 4;
	static constexpr unsigned InnerPrefetchStride = 16;
	static constexpr unsigned TimingSampleShift = 5;

	struct RowScanSession {
		std::size_t Lookahead = DefaultLookaheadRows;
		uint64_t LastSampleTsc = 0;
		uint64_t EmaRowCycles = 0;
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

	static Telemetry Stats() noexcept {
		return Telemetry{Issued_.load(std::memory_order_relaxed), Misses_.load(std::memory_order_relaxed),
		                  Backoffs_.load(std::memory_order_relaxed)};
	}

	static void ResetTelemetry() noexcept {
		Issued_.store(0, std::memory_order_relaxed);
		Misses_.store(0, std::memory_order_relaxed);
		Backoffs_.store(0, std::memory_order_relaxed);
	}

	template <typename Table>
	static void BeginRowScan(const Table &TableRef, RowScanSession &Session) noexcept {
		Session = RowScanSession{};
		if(TableRef.size() < ArmRowThreshold)
			return;
		Session.Armed = true;
		Session.Lookahead =
		    std::clamp(TableRef.size() / 32 + MinLookaheadRows, MinLookaheadRows, MaxLookaheadRows);
		PrefetchRowSlot(TableRef, 0);
		const std::size_t Warm = std::min(Session.Lookahead, TableRef.size() - 1);
		if(Warm > 0)
			PrefetchRowSlot(TableRef, Warm);
	}

	template <typename Table>
	static void AdvanceRowScan(const Table &TableRef, std::size_t Index, RowScanSession &Session) noexcept {
		if(!Session.Armed || Index >= TableRef.size())
			return;

		if((Index & (RowPrefetchStride - 1)) == 0) {
			const std::size_t Ahead = Index + Session.Lookahead;
			if(Ahead < TableRef.size())
				PrefetchRowSlot(TableRef, Ahead);
		}

		++Session.WindowTicks;
		if((Session.WindowTicks & ((1u << TimingSampleShift) - 1)) != 0)
			return;

		const uint64_t Now = ReadTsc();
		if(Session.LastSampleTsc != 0) {
			const uint64_t Delta = Now - Session.LastSampleTsc;
			Session.EmaRowCycles =
			    Session.EmaRowCycles == 0 ? Delta : (Session.EmaRowCycles * 7 + Delta) / 8;
			const uint64_t Budget = 28'000 + Session.Lookahead * 4'000;
			if(Session.EmaRowCycles >= Budget) {
				if(Session.RecentMisses < 8)
					++Session.RecentMisses;
				NoteMiss();
				if(Session.RecentMisses >= 3 && Session.Lookahead > MinLookaheadRows) {
					Session.Lookahead = std::max(MinLookaheadRows, Session.Lookahead / 2);
					Session.RecentMisses = 0;
					NoteBackoff();
				}
			} else if(Session.Lookahead < MaxLookaheadRows)
				++Session.Lookahead;
		}
		Session.LastSampleTsc = Now;
	}

	template <typename Table>
	static void PrefetchAppendTarget(const Table &TableRef) noexcept {
		if(TableRef.empty())
			return;
		PrefetchRowSlot(TableRef, TableRef.size() - 1);
	}

	template <typename Table>
	static void BeginJoinScan(const Table &Outer, const Table &Inner, JoinScanSession &Session) noexcept {
		BeginRowScan(Outer, Session.Outer);
		BeginRowScan(Inner, Session.Inner);
		if(Session.Inner.Armed)
			Session.Inner.Lookahead = std::min(MaxLookaheadRows, Session.Inner.Lookahead + 2);
	}

	template <typename Table>
	static void BeginInnerRescan(const Table &Inner, RowScanSession &InnerSession) noexcept {
		if(!InnerSession.Armed || Inner.empty())
			return;
		const std::size_t Span = std::min(InnerSession.Lookahead + 1, Inner.size());
		for(std::size_t I = 0; I < Span; ++I)
			PrefetchRowSlot(Inner, I);
	}

	template <typename Table>
	static void AdvanceJoinOuter(const Table &Outer, std::size_t Index, JoinScanSession &Session) noexcept {
		AdvanceRowScan(Outer, Index, Session.Outer);
	}

	template <typename Table>
	static void AdvanceJoinInner(const Table &Inner, std::size_t Index, RowScanSession &InnerSession) noexcept {
		if(!InnerSession.Armed || Index >= Inner.size())
			return;
		if((Index & (InnerPrefetchStride - 1)) != 0)
			return;
		const std::size_t Ahead = Index + InnerSession.Lookahead;
		if(Ahead < Inner.size())
			PrefetchRowSlot(Inner, Ahead);
	}

private:
	static inline std::atomic<uint64_t> Issued_{0};
	static inline std::atomic<uint64_t> Misses_{0};
	static inline std::atomic<uint64_t> Backoffs_{0};

	static uint64_t ReadTsc() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
		return __rdtsc();
#elif defined(__GNUC__) || defined(__clang__)
		unsigned Hi = 0;
		unsigned Lo = 0;
		__asm__ __volatile__("rdtsc" : "=a"(Lo), "=d"(Hi));
		return (static_cast<uint64_t>(Hi) << 32) | Lo;
#else
		return 0;
#endif
	}

	static void NoteMiss() noexcept { Misses_.fetch_add(1, std::memory_order_relaxed); }

	static void NoteBackoff() noexcept { Backoffs_.fetch_add(1, std::memory_order_relaxed); }

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
	static void PrefetchRowSlot(const Table &TableRef, std::size_t Index) noexcept {
		if(Index >= TableRef.size())
			return;
		CpuPrefetch(&TableRef[Index]);
		const auto &Row = TableRef[Index];
		CpuPrefetch(&Row);
	}
};

} // namespace AstralDB
