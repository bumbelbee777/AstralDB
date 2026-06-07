#include <Database/Storage/SemistructuredMicrokernels.hxx>

#include <Database/Storage/BulkSynthetic.hxx>
#include <Database/Storage/BulkSyntheticPrecompute.hxx>
#include <Database/Storage/BulkSyntheticSemistructured.hxx>
#include <Database/Storage/SemistructuredLut.hxx>
#include <Database/Storage/PassBitWalk.hxx>
#include <Database/Storage/SimdTiling.hxx>
#include <DS/FormatDoubleSimd.hxx>
#include <IO/Job.hxx>
#include <IO/SIMD.hxx>
#include <Database/Execution/PlanTypes.hxx>

#include <thread>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <future>
#include <numeric>
#include <queue>
#include <vector>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace AstralDB {
namespace SemistructuredMicrokernels {

namespace {

constexpr std::size_t kParallelTopKWordThreshold = 64;
constexpr std::size_t kParallelMatMinWinners = 8'192;
/** Large LIMIT top-K on lazy physical-order bulk (semistructured entity-scan family). */
constexpr std::size_t kPhysicalColumnarMatMinWinners = 1;
/** Parallel column fill + zip (preferred for semistructured LIMIT 100k). */
constexpr std::size_t kFusedFillMinWinners = 512;

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

bool EnvParallelSemistructured() noexcept {
	const char *V = EnvGet("ASTRALDB_SEMISTRUCTURED_PARALLEL");
	return V == nullptr || (V[0] != '0' && V[0] != 'n' && V[0] != 'N');
}

struct KeyRowPair {
	float Key = 0.f;
	std::uint32_t Row = 0;
};

PassBitWordWalk PassBitWordWalkFromParams(const PassBitTopKParams &Params) noexcept {
	PassBitWordWalk Walk;
	Walk.Bits = Params.Bits;
	Walk.RowCount = Params.RowCount;
	Walk.SparseWords = Params.SparsePassWords;
	Walk.SparseWordCount = Params.SparsePassWordCount;
	Walk.GroupCounts = Params.PassGroupCounts;
	Walk.GroupCount = Params.PassGroupCount;
	return Walk;
}

#if defined(__AVX2__)
void FillBioRankF32PhysicalSimdAvx2(const int64_t Start, const int64_t Step, float *Out, const std::size_t Count) noexcept {
	std::size_t I = 0;
	if(Step == 1) {
		for(; I + 8 <= Count; I += 8) {
			alignas(32) float Ranks[8];
			const int64_t Base = Start + static_cast<int64_t>(I);
			Ranks[0] = BulkSyntheticBioRankF32Row(Base + 0);
			Ranks[1] = BulkSyntheticBioRankF32Row(Base + 1);
			Ranks[2] = BulkSyntheticBioRankF32Row(Base + 2);
			Ranks[3] = BulkSyntheticBioRankF32Row(Base + 3);
			Ranks[4] = BulkSyntheticBioRankF32Row(Base + 4);
			Ranks[5] = BulkSyntheticBioRankF32Row(Base + 5);
			Ranks[6] = BulkSyntheticBioRankF32Row(Base + 6);
			Ranks[7] = BulkSyntheticBioRankF32Row(Base + 7);
			_mm256_storeu_ps(Out + I, _mm256_loadu_ps(Ranks));
		}
	} else {
		for(; I + 8 <= Count; I += 8) {
			alignas(32) float Ranks[8];
			for(unsigned J = 0; J < 8; ++J)
				Ranks[J] = BulkSyntheticBioRankF32Row(Start + static_cast<int64_t>(I + J) * Step);
			_mm256_storeu_ps(Out + I, _mm256_loadu_ps(Ranks));
		}
	}
	for(; I < Count; ++I)
		Out[I] = BulkSyntheticBioRankF32Row(Start + static_cast<int64_t>(I) * Step);
}
#endif

void FillBioRankF32PhysicalSimd(const int64_t Start, const int64_t Step, float *Out, const std::size_t Count) noexcept {
#if defined(__AVX2__)
	FillBioRankF32PhysicalSimdAvx2(Start, Step, Out, Count);
#else
	for(std::size_t I = 0; I < Count; ++I)
		Out[I] = BulkSyntheticBioRankF32Row(Start + static_cast<int64_t>(I) * Step);
#endif
}

void ScanPassBitGroupRangeTopK(const std::uint64_t *Bits, const std::uint32_t *GroupPassCounts,
                               const std::size_t GroupBegin, const std::size_t GroupEnd, const std::size_t RowCount,
                               const float *Keys, const int64_t PhysicalRankStart, const int64_t PhysicalRankStep,
                               const std::size_t Keep, const bool Ascending, std::vector<KeyRowPair> &OutSorted) {
	const std::size_t Words = (RowCount + 63) / 64;
	constexpr std::size_t WordsPerGroup = kColumnRowGroupSize / 64;
	PassBitTopKHeap<std::uint32_t> Heap;
	Heap.Reset(Keep, Ascending);
	for(std::size_t G = GroupBegin; G < GroupEnd; ++G) {
		if(GroupPassCounts[G] == 0)
			continue;
		const std::size_t W0 = G * WordsPerGroup;
		const std::size_t W1 = std::min(Words, (G + 1) * WordsPerGroup);
		for(std::size_t W = W0; W < W1; ++W) {
			std::uint64_t Word = PassBitWordMaskedAt(Bits, W, RowCount);
			const std::size_t Base = W * 64;
			while(Word != 0) {
				const unsigned Bit = static_cast<unsigned>(std::countr_zero(Word));
				const std::size_t Row = Base + Bit;
				if(Row < RowCount) {
					const float Key = Keys != nullptr
					                      ? Keys[Row]
					                      : BulkSyntheticBioRankF32Row(PhysicalRankStart +
					                                                   static_cast<int64_t>(Row) * PhysicalRankStep);
					Heap.Consider(Key, static_cast<std::uint32_t>(Row));
				}
				Word &= Word - 1;
			}
		}
	}
	std::vector<typename PassBitTopKHeap<std::uint32_t>::Entry> Sorted;
	Heap.ExtractSorted(Sorted);
	OutSorted.clear();
	OutSorted.reserve(Sorted.size());
	for(const auto &E : Sorted)
		OutSorted.push_back({E.Key, E.Row});
}

void ScanPassBitSparseTopK(const std::uint64_t *Bits, const std::uint32_t *SparseWords, const std::size_t SparseBegin,
                           const std::size_t SparseEnd, const std::size_t RowCount, const float *Keys,
                           const int64_t PhysicalRankStart, const int64_t PhysicalRankStep, const std::size_t Keep,
                           const bool Ascending, std::vector<KeyRowPair> &OutSorted) {
	PassBitTopKHeap<std::uint32_t> Heap;
	Heap.Reset(Keep, Ascending);
	for(std::size_t Si = SparseBegin; Si < SparseEnd; ++Si) {
		const std::size_t W = SparseWords[Si];
		std::uint64_t Word = PassBitWordMaskedAt(Bits, W, RowCount);
		const std::size_t Base = W * 64;
		while(Word != 0) {
			const unsigned Bit = static_cast<unsigned>(std::countr_zero(Word));
			const std::size_t Row = Base + Bit;
			if(Row < RowCount) {
				const float Key =
				    Keys != nullptr
				        ? Keys[Row]
				        : BulkSyntheticBioRankF32Row(PhysicalRankStart + static_cast<int64_t>(Row) * PhysicalRankStep);
				Heap.Consider(Key, static_cast<std::uint32_t>(Row));
			}
			Word &= Word - 1;
		}
	}
	std::vector<typename PassBitTopKHeap<std::uint32_t>::Entry> Sorted;
	Heap.ExtractSorted(Sorted);
	OutSorted.clear();
	OutSorted.reserve(Sorted.size());
	for(const auto &E : Sorted)
		OutSorted.push_back({E.Key, E.Row});
}

/** K-way merge of per-worker top-K lists (each sorted best-first) into global top-K. */
void MergeWorkerTopKSortedLists(const std::vector<std::vector<KeyRowPair>> &WorkerOut, const std::size_t Keep,
                                const bool Ascending, std::vector<KeyRowPair> &Out) {
	Out.clear();
	if(WorkerOut.empty() || Keep == 0)
		return;

	struct Cursor {
		std::size_t Worker = 0;
		std::size_t Index = 0;
	};
	struct HeapCmp {
		const std::vector<std::vector<KeyRowPair>> *Lists = nullptr;
		bool Ascending = false;
		bool operator()(const Cursor &A, const Cursor &B) const {
			const float Ka = (*Lists)[A.Worker][A.Index].Key;
			const float Kb = (*Lists)[B.Worker][B.Index].Key;
			return Ascending ? (Ka > Kb) : (Ka < Kb);
		}
	};

	HeapCmp Cmp{&WorkerOut, Ascending};
	std::priority_queue<Cursor, std::vector<Cursor>, HeapCmp> Heap(Cmp);
	for(std::size_t W = 0; W < WorkerOut.size(); ++W) {
		if(!WorkerOut[W].empty())
			Heap.push({W, 0});
	}
	Out.reserve(std::min(Keep, WorkerOut.size() * Keep));
	while(!Heap.empty() && Out.size() < Keep) {
		const Cursor C = Heap.top();
		Heap.pop();
		Out.push_back(WorkerOut[C.Worker][C.Index]);
		const std::size_t Next = C.Index + 1;
		if(Next < WorkerOut[C.Worker].size())
			Heap.push({C.Worker, Next});
	}
}

void AppendSetBitsKeys(const std::uint64_t Word, const std::size_t BaseRow, const std::size_t RowCount,
                       const float *Keys, float *OutKeys, std::uint32_t *OutRows, std::size_t &Write) noexcept {
	std::uint64_t W = Word;
	while(W != 0) {
		const unsigned Bit = static_cast<unsigned>(std::countr_zero(W));
		const std::size_t Row = BaseRow + Bit;
		if(Row < RowCount) {
			OutKeys[Write] = Keys[Row];
			OutRows[Write] = static_cast<std::uint32_t>(Row);
			++Write;
		}
		W &= W - 1;
	}
}

} // namespace

void FillBioRankF32Column(ColumnarTable &Col) noexcept {
	if(Col.BulkSyntheticLazyRankF32.size() == Col.RowCount)
		return;
	Col.BulkSyntheticLazyRankF32.resize(Col.RowCount);
	float *const Out = Col.BulkSyntheticLazyRankF32.data();
	const std::size_t Count = Col.RowCount;
	if(Count == 0)
		return;

	const auto FillPanel = [&](const std::size_t Begin, const std::size_t End) {
		if(Col.BulkSyntheticPhysicalOrder) {
			const int64_t Start = Col.BulkStartId + static_cast<int64_t>(Begin) * Col.BulkStep;
			const int64_t Step = Col.BulkStep;
			FillBioRankF32PhysicalSimd(Start, Step, Out + Begin, End - Begin);
			return;
		}
		for(std::size_t I = Begin; I < End; ++I)
			Out[I] = BulkSyntheticBioRankF32Row(BulkSyntheticRowIdAt(Col, I));
	};

	const bool Parallel = EnvParallelSemistructured() && JobSystem::Instance().IsRunning() && Count >= 256'000;
	if(!Parallel) {
		SimdTiling::ForL1Panels(Count, sizeof(float), WorkloadClass::OlapScan,
		                        [&](const std::size_t Begin, const std::size_t End, const TiledCachePlan &,
		                            const SimdTileSession &Session) {
			                        (void)Session;
			                        FillPanel(Begin, End);
		                        },
		                        []() {});
		return;
	}

	const std::size_t Workers = std::min<std::size_t>(std::max<std::size_t>(1, std::thread::hardware_concurrency()),
	                                                  8);
	const std::size_t Chunk = (Count + Workers - 1) / Workers;
	const bool Physical = Col.BulkSyntheticPhysicalOrder;
	const int64_t Start = Col.BulkStartId;
	const int64_t Step = Col.BulkStep;
	std::vector<std::future<void>> Futs;
	Futs.reserve(Workers);
	for(std::size_t W = 0; W < Workers; ++W) {
		const std::size_t Begin = W * Chunk;
		if(Begin >= Count)
			break;
		const std::size_t End = std::min(Count, Begin + Chunk);
		Futs.push_back(JobSystem::Instance().SubmitAsync([Physical, Start, Step, Out, &Col, Begin, End]() {
			if(Physical) {
				const int64_t PanelStart = Start + static_cast<int64_t>(Begin) * Step;
				FillBioRankF32PhysicalSimd(PanelStart, Step, Out + Begin, End - Begin);
				return;
			}
			for(std::size_t I = Begin; I < End; ++I)
				Out[I] = BulkSyntheticBioRankF32Row(BulkSyntheticRowIdAt(Col, I));
		}));
	}
	for(auto &F : Futs)
		F.wait();
}

void BuildTopKPassBits(ColumnarTable &Col, const std::vector<std::size_t> &WinnerRowIndices) noexcept {
	const std::size_t Words = (Col.RowCount + 63) / 64;
	Col.BulkSyntheticTopKBits.assign(Words, 0);
	for(const std::size_t Row : WinnerRowIndices) {
		if(Row >= Col.RowCount)
			continue;
		Col.BulkSyntheticTopKBits[Row / 64] |= 1ULL << (Row % 64);
	}
}

void GatherPassBitKeys(const PassBitTopKParams &Params, std::vector<float> &OutKeys,
                       std::vector<std::uint32_t> &OutRows) noexcept {
	OutKeys.clear();
	OutRows.clear();
	if(!Params.Bits || !Params.Keys || Params.RowCount == 0)
		return;
	const PassBitWordWalk Walk = PassBitWordWalkFromParams(Params);
	std::size_t Write = 0;
	if(Params.KnownPassCount > 0) {
		const std::size_t Cap = static_cast<std::size_t>(Params.KnownPassCount);
		OutKeys.resize(Cap);
		OutRows.resize(Cap);
		ForEachNonemptyPassWord(Walk, [&](const std::size_t W) {
			AppendSetBitsKeys(PassBitWordMaskedAt(Walk.Bits, W, Walk.RowCount), W * 64, Walk.RowCount, Params.Keys,
			                OutKeys.data(), OutRows.data(), Write);
		});
	} else {
		std::size_t Total = 0;
		ForEachNonemptyPassWord(Walk, [&](const std::size_t W) {
			Total += static_cast<std::size_t>(std::popcount(PassBitWordMaskedAt(Walk.Bits, W, Walk.RowCount)));
		});
		if(Total == 0)
			return;
		OutKeys.resize(Total);
		OutRows.resize(Total);
		ForEachNonemptyPassWord(Walk, [&](const std::size_t W) {
			AppendSetBitsKeys(PassBitWordMaskedAt(Walk.Bits, W, Walk.RowCount), W * 64, Walk.RowCount, Params.Keys,
			                OutKeys.data(), OutRows.data(), Write);
		});
	}
	OutKeys.resize(Write);
	OutRows.resize(Write);
}

void SelectTopKPassBitsParallelGroup(const PassBitTopKParams &Params, std::vector<std::size_t> &OutSortedRowIndices,
                                     const std::size_t MaxWorkers) {
	OutSortedRowIndices.clear();
	if(!Params.Bits || !Params.PassGroupCounts || Params.PassGroupCount == 0 || Params.RowCount == 0 || Params.K == 0)
		return;
	if(Params.Keys == nullptr && Params.PhysicalRankStep == 0 && !Params.KeyFn)
		return;

	const std::size_t Keep = Params.K;
	const std::size_t GroupCount = Params.PassGroupCount;
	const std::size_t Workers = std::min(
	    MaxWorkers, std::min<std::size_t>(GroupCount, std::max<std::size_t>(1, std::thread::hardware_concurrency())));
	const std::size_t GroupsPerWorker = (GroupCount + Workers - 1) / Workers;

	std::vector<std::vector<KeyRowPair>> WorkerOut(Workers);
	std::vector<std::future<void>> Futs;
	Futs.reserve(Workers);

	for(std::size_t W = 0; W < Workers; ++W) {
		const std::size_t GroupBegin = W * GroupsPerWorker;
		if(GroupBegin >= GroupCount)
			break;
		const std::size_t GroupEnd = std::min(GroupCount, GroupBegin + GroupsPerWorker);
		Futs.push_back(JobSystem::Instance().SubmitAsync([Params, GroupBegin, GroupEnd, Keep, &WorkerOut, W]() {
			ScanPassBitGroupRangeTopK(Params.Bits, Params.PassGroupCounts, GroupBegin, GroupEnd, Params.RowCount,
			                          Params.Keys, Params.PhysicalRankStart, Params.PhysicalRankStep, Keep,
			                          Params.Ascending, WorkerOut[W]);
		}));
	}
	for(auto &F : Futs)
		F.wait();

	std::vector<KeyRowPair> Merged;
	MergeWorkerTopKSortedLists(WorkerOut, Keep, Params.Ascending, Merged);
	if(Merged.empty())
		return;

	OutSortedRowIndices.resize(Merged.size());
	for(std::size_t I = 0; I < Merged.size(); ++I)
		OutSortedRowIndices[I] = Merged[I].Row;
}

void SelectTopKPassBitsParallelMerge(const PassBitTopKParams &Params, std::vector<std::size_t> &OutSortedRowIndices) {
	OutSortedRowIndices.clear();
	if(!Params.Bits || Params.RowCount == 0 || Params.K == 0)
		return;
	if(Params.Keys == nullptr && Params.PhysicalRankStep == 0)
		return;

	const PassBitWordWalk Walk = PassBitWordWalkFromParams(Params);
	const std::uint32_t *SparseWords = Params.SparsePassWords;
	std::size_t SparseCount = Params.SparsePassWordCount;
	std::vector<std::uint32_t> SparseScratch;
	if(SparseCount == 0 && Walk.Bits != nullptr) {
		ForEachNonemptyPassWord(Walk, [&](const std::size_t W) { SparseScratch.push_back(static_cast<std::uint32_t>(W)); });
		SparseWords = SparseScratch.data();
		SparseCount = SparseScratch.size();
	}
	if(SparseCount == 0)
		return;

	const std::size_t Keep = Params.K;
	const std::size_t MinSparsePerWorker =
	    Params.RowCount >= 32'000'000 ? kParallelTopKWordThreshold : (kParallelTopKWordThreshold * 4);
	const std::size_t Workers = std::min<std::size_t>(
	    8, std::min(std::max<std::size_t>(1, std::thread::hardware_concurrency()),
	                std::max<std::size_t>(1, SparseCount / MinSparsePerWorker)));
	const std::size_t SparsePerWorker = (SparseCount + Workers - 1) / Workers;

	std::vector<std::vector<KeyRowPair>> WorkerOut(Workers);
	std::vector<std::future<void>> Futs;
	Futs.reserve(Workers);

	for(std::size_t W = 0; W < Workers; ++W) {
		const std::size_t SparseBegin = W * SparsePerWorker;
		if(SparseBegin >= SparseCount)
			break;
		const std::size_t SparseEnd = std::min(SparseCount, SparseBegin + SparsePerWorker);
		Futs.push_back(JobSystem::Instance().SubmitAsync([Params, SparseWords, SparseBegin, SparseEnd, Keep, &WorkerOut,
		                                                  W]() {
			ScanPassBitSparseTopK(Params.Bits, SparseWords, SparseBegin, SparseEnd, Params.RowCount, Params.Keys,
			                      Params.PhysicalRankStart, Params.PhysicalRankStep, Keep, Params.Ascending,
			                      WorkerOut[W]);
		}));
	}
	for(auto &F : Futs)
		F.wait();

	std::vector<KeyRowPair> Merged;
	MergeWorkerTopKSortedLists(WorkerOut, Keep, Params.Ascending, Merged);
	if(Merged.empty())
		return;

	OutSortedRowIndices.resize(Merged.size());
	for(std::size_t I = 0; I < Merged.size(); ++I)
		OutSortedRowIndices[I] = Merged[I].Row;
}

void SelectTopKPassBitsPartialSort(const PassBitTopKParams &Params, std::vector<std::size_t> &OutSortedRowIndices) {
	OutSortedRowIndices.clear();
	if(!Params.Bits || !Params.Keys || Params.RowCount == 0 || Params.K == 0)
		return;
	thread_local std::vector<float> Keys;
	thread_local std::vector<std::uint32_t> Rows;
	thread_local std::vector<std::uint32_t> Ord;
	GatherPassBitKeys(Params, Keys, Rows);
	if(Keys.empty())
		return;
	const std::size_t N = Keys.size();
	const std::size_t Keep = std::min(Params.K, N);
	Ord.resize(N);
	for(std::size_t I = 0; I < N; ++I)
		Ord[I] = static_cast<std::uint32_t>(I);
	const float *const KeyData = Keys.data();
	const auto LessIdx = [&](const std::uint32_t A, const std::uint32_t B) {
		return Params.Ascending ? (KeyData[A] < KeyData[B]) : (KeyData[A] > KeyData[B]);
	};
	if(N > Keep)
		std::partial_sort(Ord.begin(), Ord.begin() + static_cast<std::ptrdiff_t>(Keep), Ord.end(), LessIdx);
	else
		std::sort(Ord.begin(), Ord.end(), LessIdx);
	OutSortedRowIndices.resize(Keep);
	for(std::size_t I = 0; I < Keep; ++I)
		OutSortedRowIndices[I] = Rows[Ord[I]];
}

void SelectTopKPassBits(const PassBitTopKParams &Params, std::vector<std::size_t> &OutSortedRowIndices) noexcept {
	if(!Params.Bits || Params.RowCount == 0 || Params.K == 0)
		return;
	if(Params.Keys != nullptr && Params.KnownPassCount > 0 &&
	   Params.K < Params.KnownPassCount / 8 && Params.KnownPassCount <= 2'000'000) {
		SelectTopKPassBitsPartialSort(Params, OutSortedRowIndices);
		return;
	}
	if(!Params.Ascending && Params.Keys != nullptr && Params.KnownPassCount > 0 &&
	   Params.KnownPassCount <= Params.K + Params.K / 5) {
		SelectTopKPassBitsPartialSort(Params, OutSortedRowIndices);
		return;
	}
	if(!Params.Ascending && (Params.Keys != nullptr || Params.PhysicalRankStep != 0 || Params.KeyFn) &&
	   Params.RowCount >= 32'000 && EnvParallelSemistructured() && JobSystem::Instance().IsRunning()) {
		const std::size_t Words = (Params.RowCount + 63) / 64;
		const bool SparseOk = Params.SparsePassWords != nullptr && Params.SparsePassWordCount > 0 &&
		                      Params.SparsePassWordCount * 4 <= Words * 3;
		if((Params.Keys != nullptr || Params.PhysicalRankStep != 0) && SparseOk) {
			SelectTopKPassBitsParallelMerge(Params, OutSortedRowIndices);
			if(!OutSortedRowIndices.empty())
				return;
		}
		if(Params.PassGroupCounts != nullptr && Params.PassGroupCount > 0 &&
		   (Params.Keys != nullptr || Params.PhysicalRankStep != 0)) {
			const std::size_t Workers = std::min<std::size_t>(
			    8, std::max<std::size_t>(1, std::thread::hardware_concurrency()));
			SelectTopKPassBitsParallelGroup(Params, OutSortedRowIndices, Workers);
			if(!OutSortedRowIndices.empty())
				return;
		}
	}
	SelectTopKFromPassBits(Params, OutSortedRowIndices);
}

namespace {

using ScalarSqlFn = SQL::ScalarSqlFn;

char *WriteI64(char *P, long long V) {
	if(V == 0) {
		*P++ = '0';
		return P;
	}
	if(V < 0) {
		*P++ = '-';
		V = -V;
	}
	char Buf[24];
	int Len = 0;
	while(V > 0) {
		Buf[Len++] = static_cast<char>('0' + V % 10);
		V /= 10;
	}
	while(Len > 0)
		*P++ = Buf[--Len];
	return P;
}

bool ProjectionRowIdBatchable(const SemistructuredProjectionSpec &P, const bool HasDenseRankKeys) noexcept {
	const auto Fn = static_cast<ScalarSqlFn>(P.FnTag);
	if(Fn == ScalarSqlFn::TextRank)
		return HasDenseRankKeys;
	if(Fn == ScalarSqlFn::CharLength)
		return true;
	if(Fn == ScalarSqlFn::JsonExtract || Fn == ScalarSqlFn::XmlExtract || Fn == ScalarSqlFn::RegexpExtract)
		return P.Args.size() >= 2;
	return false;
}

void GatherWinnerRowIds(const ColumnarTable &Col, const std::vector<std::size_t> &Winners,
                        std::vector<int64_t> &Out) noexcept {
	const std::size_t N = Winners.size();
	Out.resize(N);
	if(Col.BulkSyntheticPhysicalOrder && Col.BulkStep == 1) {
		const int64_t Start = Col.BulkStartId;
		for(std::size_t I = 0; I < N; ++I)
			Out[I] = Start + static_cast<int64_t>(Winners[I]);
		return;
	}
	for(std::size_t I = 0; I < N; ++I)
		Out[I] = BulkSyntheticRowIdAt(Col, Winners[I]);
}

void BatchCharLengthFromRowIds(const std::vector<int64_t> &RowIds, std::vector<std::string> &Out) {
	const std::size_t N = RowIds.size();
	Out.resize(N);
	for(std::size_t I = 0; I < N; ++I)
		Out[I].reserve(8);
	for(std::size_t I = 0; I < N; ++I) {
		char Buf[16];
		const char *End = WriteI64(Buf, static_cast<long long>(BulkSyntheticBioLengthRow(RowIds[I])));
		Out[I].assign(Buf, static_cast<std::size_t>(End - Buf));
	}
}

enum class PhysicalStringFillOp : std::uint8_t { Pk, JsonExtract, XmlExtract, CharLength, RegexpExtract };

struct PhysicalStringFill {
	PhysicalStringFillOp Op = PhysicalStringFillOp::Pk;
	std::string_view Path;
	std::string_view ColName;
	std::vector<std::string> *Out = nullptr;
};

struct PhysicalMatSchedule {
	std::vector<PhysicalStringFill> StringFills;
	bool HasRankColumn = false;
};

void MaterializePhysicalRowsRowMajor(const std::vector<int64_t> &RowIds, const std::vector<std::size_t> &WinnerRowIndices,
                                     const PhysicalMatSchedule &Schedule, const float *RankKeys,
                                     const std::size_t RankCount, const std::string_view RankColName,
                                     RowTable &Rows) {
	const std::size_t N = RowIds.size();
	const bool WriteRank = Schedule.HasRankColumn && RankKeys != nullptr && !RankColName.empty();
	for(std::size_t I = 0; I < N; ++I) {
#if defined(__GNUC__) || defined(__clang__)
		if(I + 8 < N)
			__builtin_prefetch(&RowIds[I + 8], 0, 0);
#endif
		const int64_t RowId = RowIds[I];
		RowItem &Row = Rows[I];
		for(const PhysicalStringFill &Fill : Schedule.StringFills) {
			switch(Fill.Op) {
			case PhysicalStringFillOp::Pk: {
				char Buf[32];
				const char *End = WriteI64(Buf, static_cast<long long>(RowId));
				Row.emplace(Fill.ColName, std::string(Buf, static_cast<std::size_t>(End - Buf)));
				break;
			}
			case PhysicalStringFillOp::CharLength: {
				char Buf[16];
				const char *End = WriteI64(Buf, static_cast<long long>(BulkSyntheticBioLengthRow(RowId)));
				Row.emplace(Fill.ColName, std::string(Buf, static_cast<std::size_t>(End - Buf)));
				break;
			}
			case PhysicalStringFillOp::JsonExtract: {
				std::string Cell;
				if(BulkSyntheticJsonExtractRow(RowId, Fill.Path, Cell))
					Row.emplace(Fill.ColName, std::move(Cell));
				else
					Row.emplace(Fill.ColName, std::string());
				break;
			}
			case PhysicalStringFillOp::XmlExtract: {
				std::string Cell;
				if(BulkSyntheticXmlExtractRow(RowId, Fill.Path, Cell))
					Row.emplace(Fill.ColName, std::move(Cell));
				else
					Row.emplace(Fill.ColName, std::string());
				break;
			}
			case PhysicalStringFillOp::RegexpExtract: {
				std::string Cell;
				if(BulkSyntheticRegexpExtractRow(RowId, Fill.Path, Cell))
					Row.emplace(Fill.ColName, std::move(Cell));
				else
					Row.emplace(Fill.ColName, std::string());
				break;
			}
			}
		}
		if(WriteRank) {
			const std::size_t Ri = WinnerRowIndices[I];
			const float Key = Ri < RankCount ? RankKeys[Ri] : 0.f;
			char Buf[8];
			std::uint16_t Len = 0;
			FormatDoubleSimd::FormatOneRankMilli(Key, Buf, Len);
			Row.emplace(RankColName, std::string(Buf, Len));
		}
	}
}

void FillPhysicalStringColumnBatched(const PhysicalStringFill &Fill, const std::vector<int64_t> &RowIds) {
	std::vector<std::string> &Out = *Fill.Out;
	const std::size_t N = RowIds.size();
	const int64_t *Ids = RowIds.data();
	FormatDoubleSimd::FormattedDoubleColumn Strip;
	switch(Fill.Op) {
	case PhysicalStringFillOp::Pk:
		SemistructuredLut::FillPkStrip(Ids, N, Strip);
		Strip.AssignToVector(Out);
		return;
	case PhysicalStringFillOp::JsonExtract:
		if(SemistructuredLut::FillJsonExtractStrip(Ids, N, Fill.Path, Strip)) {
			Strip.AssignToVector(Out);
			return;
		}
		break;
	case PhysicalStringFillOp::XmlExtract:
		if(SemistructuredLut::FillXmlExtractStrip(Ids, N, Fill.Path, Strip)) {
			Strip.AssignToVector(Out);
			return;
		}
		break;
	case PhysicalStringFillOp::CharLength:
		SemistructuredLut::FillCharLengthStrip(Ids, N, Strip);
		Strip.AssignToVector(Out);
		return;
	case PhysicalStringFillOp::RegexpExtract:
		if(SemistructuredLut::FillRegexpExtractStrip(Ids, N, Fill.Path, Strip)) {
			Strip.AssignToVector(Out);
			return;
		}
		break;
	}
	Out.resize(N);
	for(std::size_t I = 0; I < N; ++I) {
		const int64_t RowId = RowIds[I];
		std::string &Cell = Out[I];
		switch(Fill.Op) {
		case PhysicalStringFillOp::JsonExtract:
			if(!BulkSyntheticJsonExtractRow(RowId, Fill.Path, Cell))
				Cell.clear();
			break;
		case PhysicalStringFillOp::XmlExtract:
			if(!BulkSyntheticXmlExtractRow(RowId, Fill.Path, Cell))
				Cell.clear();
			break;
		case PhysicalStringFillOp::RegexpExtract:
			if(!BulkSyntheticRegexpExtractRow(RowId, Fill.Path, Cell))
				Cell.clear();
			break;
		default:
			Cell.clear();
			break;
		}
	}
}

void FillPhysicalStringColumnsFused(const std::vector<int64_t> &RowIds, const PhysicalMatSchedule &Schedule) {
	for(const PhysicalStringFill &Fill : Schedule.StringFills)
		FillPhysicalStringColumnBatched(Fill, RowIds);
}

bool BuildPhysicalMatSchedule(const std::vector<SemistructuredProjectionSpec> &Projections,
                              const Database::Column *Pk, const bool HasRankKeys,
                              std::vector<std::string> &Names, std::vector<std::vector<std::string>> &OwnedStringCols,
                              std::vector<FormatDoubleSimd::FormattedDoubleColumn> &OwnedFormattedCols,
                              std::size_t &FormattedColBegin, PhysicalMatSchedule &Schedule) {
	Schedule = {};
	FormattedColBegin = static_cast<std::size_t>(-1);
	std::size_t StringColReserve = Pk != nullptr ? 1 : 0;
	(void)OwnedFormattedCols;
	for(const SemistructuredProjectionSpec &P : Projections) {
		if(P.OutCol.empty())
			continue;
		if(Pk != nullptr && P.OutCol == Pk->Name)
			continue;
		const auto Fn = static_cast<ScalarSqlFn>(P.FnTag);
		if(Fn == ScalarSqlFn::TextRank && HasRankKeys)
			Schedule.HasRankColumn = true;
		else if(ProjectionRowIdBatchable(P, HasRankKeys))
			++StringColReserve;
		else
			return false;
	}
	Names.reserve(StringColReserve + (Schedule.HasRankColumn ? 1 : 0));
	OwnedStringCols.reserve(StringColReserve);
	Schedule.StringFills.reserve(StringColReserve);

	if(Pk != nullptr) {
		OwnedStringCols.emplace_back();
		Names.push_back(Pk->Name);
		Schedule.StringFills.push_back({PhysicalStringFillOp::Pk, {}, Names.back(), &OwnedStringCols.back()});
	}
	for(const SemistructuredProjectionSpec &P : Projections) {
		if(P.OutCol.empty())
			continue;
		if(Pk != nullptr && P.OutCol == Pk->Name)
			continue;
		const auto Fn = static_cast<ScalarSqlFn>(P.FnTag);
		if(Fn == ScalarSqlFn::TextRank && HasRankKeys) {
			if(FormattedColBegin == static_cast<std::size_t>(-1))
				FormattedColBegin = Names.size();
			Names.push_back(P.OutCol);
			continue;
		}
		if(!ProjectionRowIdBatchable(P, HasRankKeys))
			return false;
		OwnedStringCols.emplace_back();
		Names.push_back(P.OutCol);
		PhysicalStringFill Fill;
		Fill.ColName = P.OutCol;
		Fill.Out = &OwnedStringCols.back();
		if(Fn == ScalarSqlFn::JsonExtract) {
			Fill.Op = PhysicalStringFillOp::JsonExtract;
			if(P.Args.size() >= 2)
				Fill.Path = P.Args[1].second;
		} else if(Fn == ScalarSqlFn::XmlExtract) {
			Fill.Op = PhysicalStringFillOp::XmlExtract;
			if(P.Args.size() >= 2)
				Fill.Path = P.Args[1].second;
		} else if(Fn == ScalarSqlFn::CharLength) {
			Fill.Op = PhysicalStringFillOp::CharLength;
		} else if(Fn == ScalarSqlFn::RegexpExtract) {
			Fill.Op = PhysicalStringFillOp::RegexpExtract;
		} else {
			return false;
		}
		Schedule.StringFills.push_back(Fill);
	}
	return true;
}

void BatchRowIds(const ColumnarTable &Col, const std::vector<std::size_t> &Winners, std::vector<std::string> &Out) {
	const std::size_t N = Winners.size();
	Out.resize(N);
	if(Col.BulkStep == 1) {
		const int64_t Start = Col.BulkStartId;
		for(std::size_t I = 0; I < N; ++I) {
			char Buf[32];
			const char *End = WriteI64(Buf, static_cast<long long>(Start + static_cast<int64_t>(Winners[I])));
			Out[I].assign(Buf, static_cast<std::size_t>(End - Buf));
		}
		return;
	}
	for(std::size_t I = 0; I < N; ++I) {
		char Buf[32];
		const int64_t RowId = BulkSyntheticRowIdAt(Col, Winners[I]);
		const char *End = WriteI64(Buf, static_cast<long long>(RowId));
		Out[I].assign(Buf, static_cast<std::size_t>(End - Buf));
	}
}

void GatherWinnerRankF32(const float *RankKeys, const std::size_t RankCount, const std::vector<std::size_t> &Winners,
                         std::vector<float> &Out) noexcept {
	const std::size_t N = Winners.size();
	Out.resize(N);
	std::size_t I = 0;
#if defined(__AVX2__)
	for(; I + 8 <= N; I += 8) {
		alignas(32) float Lane[8];
		for(unsigned J = 0; J < 8; ++J) {
			const std::size_t Ri = Winners[I + J];
			Lane[J] = Ri < RankCount ? RankKeys[Ri] : 0.f;
		}
		_mm256_storeu_ps(Out.data() + I, _mm256_loadu_ps(Lane));
	}
#endif
	for(; I < N; ++I) {
		const std::size_t Ri = Winners[I];
		Out[I] = Ri < RankCount ? RankKeys[Ri] : 0.f;
	}
}

bool BatchProjectionFromRowIds(const SemistructuredProjectionSpec &P, const std::vector<int64_t> &RowIds,
                               std::vector<std::string> &Out) {
	const std::size_t N = RowIds.size();
	const auto Fn = static_cast<ScalarSqlFn>(P.FnTag);
	if(Fn == ScalarSqlFn::TextRank)
		return true;
	Out.resize(N);
	if(Fn == ScalarSqlFn::CharLength) {
		BatchCharLengthFromRowIds(RowIds, Out);
		return true;
	}
	if(P.Args.size() < 2)
		return false;
	const std::string_view Path = P.Args[1].second;
	if(Fn == ScalarSqlFn::JsonExtract) {
		for(std::size_t I = 0; I < N; ++I) {
			if(!BulkSyntheticJsonExtractRow(RowIds[I], Path, Out[I]))
				Out[I].clear();
		}
		return true;
	}
	if(Fn == ScalarSqlFn::XmlExtract) {
		for(std::size_t I = 0; I < N; ++I) {
			if(!BulkSyntheticXmlExtractRow(RowIds[I], Path, Out[I]))
				Out[I].clear();
		}
		return true;
	}
	if(Fn == ScalarSqlFn::RegexpExtract) {
		for(std::size_t I = 0; I < N; ++I) {
			if(!BulkSyntheticRegexpExtractRow(RowIds[I], Path, Out[I]))
				Out[I].clear();
		}
		return true;
	}
	return false;
}

bool TryBatchProjection(const ColumnarTable &Col, const SemistructuredProjectionSpec &P,
                        const std::vector<std::size_t> &Winners, std::vector<std::string> &Out) {
	std::vector<int64_t> RowIds;
	GatherWinnerRowIds(Col, Winners, RowIds);
	return BatchProjectionFromRowIds(P, RowIds, Out);
}

void BatchProjection(const ColumnarTable &Col, const SemistructuredProjectionSpec &P,
                     const std::vector<std::size_t> &Winners, std::vector<std::string> &Out) {
	if(TryBatchProjection(Col, P, Winners, Out))
		return;
	const std::size_t N = Winners.size();
	Out.resize(N);
	std::string Cell;
	for(std::size_t I = 0; I < N; ++I) {
		const int64_t RowId = BulkSyntheticRowIdAt(Col, Winners[I]);
		if(BulkSyntheticTryScalarEval(P.FnTag, RowId, P.Args, Cell))
			Out[I] = Cell;
		else
			Out[I].clear();
	}
}

const Database::Column *FindPrimaryKeyColumn(const std::vector<Database::Column> &Schema) noexcept {
	for(const Database::Column &C : Schema) {
		if(C.IsPrimaryKey)
			return &C;
	}
	return Schema.empty() ? nullptr : &Schema.front();
}

SemistructuredMaterializePlan ClassifySemistructuredMaterializeImpl(
    const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
    const std::vector<SemistructuredProjectionSpec> &Projections, const std::size_t WinnerCount) noexcept {
	SemistructuredMaterializePlan Plan;
	if(WinnerCount < kPhysicalColumnarMatMinWinners || !Col.BulkSyntheticLazy || !Col.BulkSyntheticPhysicalOrder ||
	   Col.BulkStep != 1)
		return Plan;
	const bool HasDenseRank =
	    Col.BulkSyntheticLazyRankF32.size() == Col.RowCount && !Col.BulkSyntheticLazyRankF32.empty();
	const Database::Column *Pk = FindPrimaryKeyColumn(Schema);
	for(const SemistructuredProjectionSpec &P : Projections) {
		if(P.OutCol.empty())
			continue;
		if(Pk != nullptr && P.OutCol == Pk->Name)
			continue;
		if(!ProjectionRowIdBatchable(P, HasDenseRank))
			return Plan;
	}
	Plan.Kind = SemistructuredMaterializeKind::PhysicalLazyColumnar;
	return Plan;
}

bool MaterializeWinnersPhysicalColumnarImpl(const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                                        const std::vector<SemistructuredProjectionSpec> &Projections,
                                        const std::vector<std::size_t> &WinnerRowIndices, RowTable &Out) noexcept {
	Out.clear();
	if(WinnerRowIndices.empty())
		return true;

	const Database::Column *Pk = FindPrimaryKeyColumn(Schema);
	const float *RankKeys = Col.BulkSyntheticLazyRankF32.empty() ? nullptr : Col.BulkSyntheticLazyRankF32.data();
	const std::size_t RankCount = Col.BulkSyntheticLazyRankF32.size();
	const bool HasDenseRank = RankKeys != nullptr && RankCount == Col.RowCount;
	const std::size_t N = WinnerRowIndices.size();

	std::vector<int64_t> RowIds;
	GatherWinnerRowIds(Col, WinnerRowIndices, RowIds);

	std::vector<std::string> Names;
	std::vector<std::vector<std::string>> OwnedStringCols;
	std::vector<FormatDoubleSimd::FormattedDoubleColumn> OwnedFormattedCols;
	std::size_t FormattedColBegin = static_cast<std::size_t>(-1);
	PhysicalMatSchedule Schedule;
	if(!BuildPhysicalMatSchedule(Projections, Pk, HasDenseRank, Names, OwnedStringCols, OwnedFormattedCols,
	                               FormattedColBegin, Schedule))
		return false;

	RowTable Rows;
	Rows.resize(N);
	const std::size_t ColCount = Names.size();
	for(RowItem &Row : Rows)
		Row.reserve(ColCount);

	const bool ColumnBatch = N >= kFusedFillMinWinners && !Schedule.StringFills.empty();
	const std::string_view RankColName =
	    (FormattedColBegin < Names.size()) ? std::string_view(Names[FormattedColBegin]) : std::string_view();

	if(ColumnBatch) {
		FillPhysicalStringColumnsFused(RowIds, Schedule);
		std::vector<const std::vector<std::string> *> StringPtrs;
		StringPtrs.reserve(OwnedStringCols.size());
		for(const std::vector<std::string> &C : OwnedStringCols)
			StringPtrs.push_back(&C);
		const std::vector<const FormatDoubleSimd::FormattedDoubleColumn *> NoFmt;
		FormatDoubleSimd::MaterializeMixedZip(Names, StringPtrs, NoFmt, Names.size(), N, Rows);
		if(Schedule.HasRankColumn && RankKeys != nullptr && !RankColName.empty()) {
			for(std::size_t I = 0; I < N; ++I) {
				const std::size_t Ri = WinnerRowIndices[I];
				const float Key = Ri < RankCount ? RankKeys[Ri] : 0.f;
				char Buf[8];
				std::uint16_t Len = 0;
				FormatDoubleSimd::FormatOneRankMilli(Key, Buf, Len);
				Rows[I].emplace(RankColName, std::string(Buf, Len));
			}
		}
	} else {
		MaterializePhysicalRowsRowMajor(RowIds, WinnerRowIndices, Schedule, RankKeys, RankCount, RankColName, Rows);
	}

	Out = std::move(Rows);
	return true;
}

bool MaterializeWinnersBatchedImpl(const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                                   const std::vector<SemistructuredProjectionSpec> &Projections,
                                   const std::vector<std::size_t> &WinnerRowIndices, RowTable &Out) noexcept {
	Out.clear();
	if(WinnerRowIndices.empty())
		return true;

	const SemistructuredMaterializePlan Plan =
	    ClassifySemistructuredMaterializeImpl(Col, Schema, Projections, WinnerRowIndices.size());
	if(Plan.Kind == SemistructuredMaterializeKind::PhysicalLazyColumnar)
		return MaterializeWinnersPhysicalColumnarImpl(Col, Schema, Projections, WinnerRowIndices, Out);

	const Database::Column *Pk = FindPrimaryKeyColumn(Schema);
	const float *RankKeys = Col.BulkSyntheticLazyRankF32.empty() ? nullptr : Col.BulkSyntheticLazyRankF32.data();
	const std::size_t RankCount = Col.BulkSyntheticLazyRankF32.size();

	std::vector<std::string> Names;
	std::vector<std::vector<std::string>> OwnedStringCols;
	std::vector<FormatDoubleSimd::FormattedDoubleColumn> OwnedFormattedCols;
	std::size_t FormattedColBegin = static_cast<std::size_t>(-1);

	std::size_t StringColCount = 0;
	std::size_t FormattedColCount = 0;
	if(Pk != nullptr)
		++StringColCount;
	for(const SemistructuredProjectionSpec &P : Projections) {
		if(P.OutCol.empty())
			continue;
		if(Pk != nullptr && P.OutCol == Pk->Name)
			continue;
		if(static_cast<ScalarSqlFn>(P.FnTag) == ScalarSqlFn::TextRank && RankKeys != nullptr)
			++FormattedColCount;
		else
			++StringColCount;
	}

	OwnedStringCols.reserve(StringColCount);
	OwnedFormattedCols.reserve(FormattedColCount);
	Names.reserve(StringColCount + FormattedColCount);

	const std::size_t WinnerCount = WinnerRowIndices.size();
	const bool ParallelMat = WinnerCount >= kParallelMatMinWinners && EnvParallelSemistructured() &&
	                         JobSystem::Instance().IsRunning();
	std::vector<std::future<void>> MatFuts;

	if(Pk != nullptr) {
		OwnedStringCols.emplace_back();
		Names.push_back(Pk->Name);
		if(ParallelMat) {
			MatFuts.push_back(JobSystem::Instance().SubmitAsync([&Col, &WinnerRowIndices, &OwnedStringCols]() {
				BatchRowIds(Col, WinnerRowIndices, OwnedStringCols.back());
			}));
		} else {
			BatchRowIds(Col, WinnerRowIndices, OwnedStringCols.back());
		}
	}

	std::vector<float> WinnerRankScratch;
	for(const SemistructuredProjectionSpec &P : Projections) {
		if(P.OutCol.empty())
			continue;
		if(Pk != nullptr && P.OutCol == Pk->Name)
			continue;
		if(static_cast<ScalarSqlFn>(P.FnTag) == ScalarSqlFn::TextRank && RankKeys != nullptr) {
			if(FormattedColBegin == static_cast<std::size_t>(-1))
				FormattedColBegin = Names.size();
			OwnedFormattedCols.emplace_back();
			Names.push_back(P.OutCol);
			if(ParallelMat) {
				MatFuts.push_back(JobSystem::Instance().SubmitAsync(
				    [RankKeys, RankCount, &WinnerRowIndices, &WinnerRankScratch, &OwnedFormattedCols]() {
					    GatherWinnerRankF32(RankKeys, RankCount, WinnerRowIndices, WinnerRankScratch);
					    FormatDoubleSimd::FormatRankF32Column(WinnerRankScratch.data(), WinnerRankScratch.size(),
					                                          OwnedFormattedCols.back(), WorkloadClass::OlapScan);
				    }));
			} else {
				GatherWinnerRankF32(RankKeys, RankCount, WinnerRowIndices, WinnerRankScratch);
				FormatDoubleSimd::FormatRankF32Column(WinnerRankScratch.data(), WinnerRankScratch.size(),
				                                      OwnedFormattedCols.back(), WorkloadClass::OlapScan);
			}
			continue;
		}
		OwnedStringCols.emplace_back();
		Names.push_back(P.OutCol);
		if(ParallelMat) {
			const SemistructuredProjectionSpec PCopy = P;
			std::vector<std::string> *const ColOut = &OwnedStringCols.back();
			MatFuts.push_back(JobSystem::Instance().SubmitAsync(
			    [&Col, PCopy, &WinnerRowIndices, ColOut]() { BatchProjection(Col, PCopy, WinnerRowIndices, *ColOut); }));
		} else {
			BatchProjection(Col, P, WinnerRowIndices, OwnedStringCols.back());
		}
	}
	for(std::future<void> &F : MatFuts)
		F.wait();

	std::vector<const std::vector<std::string> *> StringPtrs;
	StringPtrs.reserve(OwnedStringCols.size());
	for(const std::vector<std::string> &C : OwnedStringCols)
		StringPtrs.push_back(&C);

	std::vector<const FormatDoubleSimd::FormattedDoubleColumn *> FormattedPtrs;
	FormattedPtrs.reserve(OwnedFormattedCols.size());
	for(const FormatDoubleSimd::FormattedDoubleColumn &C : OwnedFormattedCols)
		FormattedPtrs.push_back(&C);

	if(FormattedColBegin == static_cast<std::size_t>(-1))
		FormattedColBegin = Names.size();
	RowTable Rows;
	FormatDoubleSimd::MaterializeMixedZip(Names, StringPtrs, FormattedPtrs, FormattedColBegin, WinnerRowIndices.size(),
	                                      Rows);
	Out = std::move(Rows);
	return true;
}

} // namespace

SemistructuredMaterializePlan ClassifySemistructuredMaterialize(
    const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
    const std::vector<SemistructuredProjectionSpec> &Projections, const std::size_t WinnerCount) noexcept {
	return ClassifySemistructuredMaterializeImpl(Col, Schema, Projections, WinnerCount);
}

bool MaterializeWinnersPhysicalColumnar(const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                                        const std::vector<SemistructuredProjectionSpec> &Projections,
                                        const std::vector<std::size_t> &WinnerRowIndices, RowTable &Out) noexcept {
	return MaterializeWinnersPhysicalColumnarImpl(Col, Schema, Projections, WinnerRowIndices, Out);
}

bool MaterializeWinnersBatched(const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                               const std::vector<SemistructuredProjectionSpec> &Projections,
                               const std::vector<std::size_t> &WinnerRowIndices, RowTable &Out) noexcept {
	return MaterializeWinnersBatchedImpl(Col, Schema, Projections, WinnerRowIndices, Out);
}

} // namespace SemistructuredMicrokernels
} // namespace AstralDB
