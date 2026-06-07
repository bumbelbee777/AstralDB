#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace AstralDB {

enum class PrefetchHint : uint8_t { Temporal = 0, NonTemporal = 1, Spatial = 2 };

enum class WorkloadClass : uint8_t { OlapScan = 0, OlapWindow = 1, OltpRow = 2, JoinNested = 3, MathSciNumeric = 4 };

enum class DisarmReason : uint8_t {
	None = 0,
	TooSmall = 1,
	GlobalOff = 2,
	HotCache = 3,
	SaturatedMemory = 4,
	RandomAccess = 5,
	L3Pollution = 6,
	LowMlpConfidence = 7,
	AsyncOverload = 8,
	JoinInnerThrash = 9,
};

/** Miss-hardened prefetch with harmful-access no-op, batched MLP policy, and columnar sessions. */
struct Superfetch {
	static constexpr std::size_t ArmRowThreshold = 64;
	static constexpr std::size_t ArmColumnThreshold = 4096;
	static constexpr std::size_t DefaultLookaheadRows = 4;
	static constexpr std::size_t MaxLookaheadRows = 32;
	static constexpr std::size_t MinLookaheadRows = 1;
	static constexpr unsigned RowPrefetchStride = 4;
	static constexpr unsigned InnerPrefetchStride = 16;
	static constexpr unsigned TimingSampleShift = 5;
	static constexpr unsigned RowUnrollBatch = 4;
	static constexpr std::size_t MlpRetuneIntervalRows = 65536;
	static constexpr std::size_t MaxMlpForwardsPerScan = 8;
	static constexpr uint64_t FastRowCycles = 8'000;
	static constexpr uint64_t SlowRowCycles = 28'000;

	struct ScanPolicy {
		std::size_t Lookahead = DefaultLookaheadRows;
		PrefetchHint Hint = PrefetchHint::Temporal;
		unsigned AsyncDepth = 0;
		std::size_t ValidUntilIndex = 0;
		float MlpConfidence = 1.f;
	};

	struct RowScanSession {
		ScanPolicy Policy;
		std::size_t Lookahead = DefaultLookaheadRows;
		uint64_t LastSampleTsc = 0;
		uint64_t EmaRowCycles = 0;
		uint64_t BaselineEma = 0;
		unsigned RecentMisses = 0;
		unsigned WindowTicks = 0;
		unsigned MlpForwardCount = 0;
		unsigned AsyncRingFull = 0;
		unsigned RescanCount = 0;
		std::size_t RowCount = 0;
		std::size_t LastIndex = 0;
		std::size_t RowsSinceRetune = 0;
		WorkloadClass Workload = WorkloadClass::OltpRow;
		bool Armed = false;
		bool UseMlp = true;
		DisarmReason LastDisarm = DisarmReason::None;
	};

	struct JoinScanSession {
		RowScanSession Outer;
		RowScanSession Inner;
		std::size_t InnerRescans = 0;
	};

	struct ColumnScanSession {
		ScanPolicy Policy;
		std::size_t RowCount = 0;
		std::size_t Index = 0;
		unsigned WindowUnroll = 8;
		bool Armed = false;
		DisarmReason LastDisarm = DisarmReason::None;
	};

	struct HardwareProfile {
		std::size_t CacheLineBytes = 64;
		std::size_t L1Bytes = 32 * 1024;
		std::size_t L2Bytes = 256 * 1024;
		std::size_t L3Bytes = 8 * 1024 * 1024;
		unsigned LogicalCores = 1;
		bool NumaAvailable = false;
		bool HugePagesAvailable = false;
	};

	struct Telemetry {
		uint64_t PrefetchIssued = 0;
		uint64_t MissSignals = 0;
		uint64_t LookaheadBackoff = 0;
		uint64_t LookaheadIncreases = 0;
		uint64_t Disarmed = 0;
		uint64_t MlpForwards = 0;
		uint64_t PolicySkips = 0;
		uint64_t AsyncBatches = 0;
	};

	static Telemetry Stats() noexcept;
	static void ResetTelemetry() noexcept;
	static void Shutdown() noexcept;
	static const HardwareProfile &Hardware() noexcept;
	static uint64_t MlpForwardCountForTests() noexcept;
	static bool GlobalEnabled() noexcept;

	template <typename Table>
	static void BeginRowScan(const Table &TableRef, RowScanSession &Session,
	                         WorkloadClass Workload = WorkloadClass::OltpRow) noexcept {
		if(InAggregationScope())
			return;
		BeginRowScanCommon(TableRef.size(), Workload, Session);
		if(!Session.Armed)
			return;
		PrefetchTableRow(TableRef, 0, Session.Policy.Hint);
		const std::size_t Warm = std::min(Session.Lookahead, TableRef.size() > 0 ? TableRef.size() - 1 : 0);
		if(Warm > 0)
			PrefetchTableRow(TableRef, Warm, Session.Policy.Hint);
	}

	template <typename Table>
	static void AdvanceRowScan(const Table &TableRef, std::size_t Index, RowScanSession &Session) noexcept {
		if(InAggregationScope() || !Session.Armed || Index >= TableRef.size())
			return;
		if((Index & (RowPrefetchStride - 1)) == 0) {
			const std::size_t Ahead = Index + Session.Lookahead;
			if(Ahead < TableRef.size())
				PrefetchTableRow(TableRef, Ahead, Session.Policy.Hint);
		}
		AdvanceRowScanCommon(Index, TableRef.size(), Session);
	}

	template <typename Table>
	static void AdvanceRowScanBatch(const Table &TableRef, std::size_t Index, RowScanSession &Session) noexcept {
		const std::size_t End = std::min(Index + RowUnrollBatch, TableRef.size());
		for(std::size_t I = Index; I < End; ++I)
			AdvanceRowScan(TableRef, I, Session);
	}

	template <typename Table>
	static void PrefetchAppendTarget(const Table &TableRef) noexcept {
		if(TableRef.empty())
			return;
		PrefetchTableRow(TableRef, TableRef.size() - 1, PrefetchHint::Temporal);
	}

	template <typename Table>
	static void BeginJoinScan(const Table &Outer, const Table &Inner, JoinScanSession &Session) noexcept {
		BeginRowScan(Outer, Session.Outer, WorkloadClass::JoinNested);
		BeginRowScan(Inner, Session.Inner, WorkloadClass::JoinNested);
		if(Session.Inner.Armed)
			Session.Inner.Lookahead = std::min(MaxLookaheadRows, Session.Inner.Lookahead + 2);
	}

	template <typename Table>
	static void BeginInnerRescan(const Table &Inner, RowScanSession &InnerSession) noexcept {
		++InnerSession.RescanCount;
		if(!InnerSession.Armed || Inner.empty())
			return;
		const std::size_t Span = std::min(InnerSession.Lookahead + 1, Inner.size());
		for(std::size_t I = 0; I < Span; ++I)
			PrefetchTableRow(Inner, I, InnerSession.Policy.Hint);
	}

	template <typename Table>
	static void AdvanceJoinOuter(const Table &Outer, std::size_t Index, JoinScanSession &Session) noexcept {
		AdvanceRowScan(Outer, Index, Session.Outer);
	}

	template <typename Table>
	static void AdvanceJoinInner(const Table &Inner, std::size_t Index, JoinScanSession &Session) noexcept {
		AdvanceJoinInnerRow(Inner, Index, Session.Outer, Session.Inner);
	}

	template <typename Table>
	static void AdvanceJoinInner(const Table &Inner, std::size_t Index, RowScanSession &InnerSession) noexcept {
		RowScanSession DummyOuter;
		AdvanceJoinInnerRow(Inner, Index, DummyOuter, InnerSession);
	}

	template <typename Table>
	static void AdvanceJoinInnerRow(const Table &Inner, std::size_t Index, RowScanSession &OuterSession,
	                                RowScanSession &InnerSession) noexcept {
		if(!InnerSession.Armed || Index >= Inner.size())
			return;
		if(Inner.size() > 0 && OuterSession.RowCount > 0 && Inner.size() / OuterSession.RowCount > 32 &&
		   InnerSession.RescanCount > 4) {
			DisarmSession(InnerSession, DisarmReason::JoinInnerThrash);
			return;
		}
		if((Index & (InnerPrefetchStride - 1)) != 0)
			return;
		const std::size_t Ahead = Index + InnerSession.Lookahead;
		if(Ahead < Inner.size())
			PrefetchTableRow(Inner, Ahead, InnerSession.Policy.Hint);
	}

	static void BeginColumnScan(std::size_t RowCount, ColumnScanSession &Session,
	                          WorkloadClass Workload = WorkloadClass::OlapWindow) noexcept;

	static void AdvanceColumnScan(ColumnScanSession &Session, const void *Addr, std::size_t Index) noexcept;

	static void PrefetchRange(const void *Base, std::size_t Bytes, PrefetchHint Hint) noexcept;

	static void IssuePrefetch(const void *Address, PrefetchHint Hint) noexcept;

	/** Suppresses row prefetch during hash aggregation (GROUP BY / CUBE). */
	struct AggregationScope {
		AggregationScope() noexcept;
		~AggregationScope() noexcept;
		AggregationScope(const AggregationScope &) = delete;
		AggregationScope &operator=(const AggregationScope &) = delete;
	};
	[[nodiscard]] static bool InAggregationScope() noexcept;

private:
	static void BeginRowScanCommon(std::size_t RowCount, WorkloadClass Workload, RowScanSession &Session) noexcept;

	static void AdvanceRowScanCommon(std::size_t Index, std::size_t RowCount, RowScanSession &Session) noexcept;

	template <typename Table>
	static void PrefetchTableRow(const Table &TableRef, std::size_t Index, PrefetchHint Hint) noexcept {
		if(Index >= TableRef.size())
			return;
		IssuePrefetch(&TableRef[Index], Hint);
		IssuePrefetch(&TableRef[Index], Hint);
	}

	static void DisarmSession(RowScanSession &Session, DisarmReason Reason) noexcept;

	static void ApplyScanPolicy(RowScanSession &Session, std::size_t RowCount, WorkloadClass Workload) noexcept;
};

} // namespace AstralDB
