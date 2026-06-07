#include <SQL/SemistructuredVM.hxx>

#include <Database/Storage/BulkSynthetic.hxx>
#include <Database/Storage/BulkSyntheticPrecompute.hxx>
#include <Database/Storage/BulkSyntheticSemistructured.hxx>
#include <Database/Storage/ColumnFilterSimd.hxx>
#include <Database/Storage/PassBitWalk.hxx>
#include <Database/Storage/PredicateKind.hxx>
#include <Database/Storage/SemistructuredKernels.hxx>
#include <Database/Storage/SemistructuredMicrokernels.hxx>
#include <IO/Job.hxx>
#include <SQL/SemistructuredPlanner.hxx>
#include <SQL/SQL.hxx>

#include <algorithm>
#include <cstdlib>
#include <future>
#include <queue>
#include <thread>

namespace AstralDB {
namespace SQL {

namespace {

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

bool OrderUsesTextRank(const SemistructuredProjectionSpec *OrderSpec) noexcept {
	return OrderSpec != nullptr && static_cast<ScalarSqlFn>(OrderSpec->FnTag) == ScalarSqlFn::TextRank;
}

const Database::Column *FindSchemaCol(const std::vector<Database::Column> &Schema, const std::string &Name) noexcept {
	for(const Database::Column &C : Schema) {
		if(C.Name == Name)
			return &C;
	}
	return nullptr;
}

void MergeWorkerTopK(const std::vector<std::vector<std::pair<float, std::uint64_t>>> &WorkerOut, std::size_t Keep,
                     bool Ascending, std::vector<std::pair<float, std::uint64_t>> &Out) {
	Out.clear();
	if(WorkerOut.empty() || Keep == 0)
		return;
	struct Cursor {
		std::size_t Worker = 0;
		std::size_t Index = 0;
	};
	struct HeapCmp {
		const std::vector<std::vector<std::pair<float, std::uint64_t>>> *Lists = nullptr;
		bool Ascending = false;
		bool operator()(const Cursor &A, const Cursor &B) const {
			const float Ka = (*Lists)[A.Worker][A.Index].first;
			const float Kb = (*Lists)[B.Worker][B.Index].first;
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

} // namespace

bool EnvSemistructuredVmEnabled() noexcept {
	const char *V = EnvGet("ASTRALDB_SEMISTRUCTURED_VM");
	return V != nullptr && V[0] != '0' && V[0] != 'n' && V[0] != 'N';
}

bool SemistructuredVM::Execute(const SSProgram &Prog, Database &Db, const std::string &Table,
                               const BulkWhereDnf &FilterDnf,
                               const std::vector<SemistructuredProjectionSpec> &Projections, const std::string &OrderCol,
                               const bool OrderAscending, const std::size_t Limit, RowTable &Out,
                               std::uint64_t *RowsScannedOut) noexcept {
	Out.clear();
	if(Table.empty() || Limit == 0)
		return false;
	HybridTableSlot *Slot = Db.FindTableSlotAssumeDbMutexHeld(Table);
	if(!Slot || Slot->Columnar.RowCount == 0)
		return false;
	const auto Schema = Db.TableSchemaAssumeDbMutexHeld(Table);
	if(!Schema || Schema->empty())
		return false;

	ColumnarTable &Col = Slot->Columnar;
	if(RowsScannedOut != nullptr)
		*RowsScannedOut += static_cast<std::uint64_t>(Col.RowCount);

	OrderSpec_ = nullptr;
	for(const SemistructuredProjectionSpec &P : Projections) {
		if(P.OutCol == OrderCol)
			OrderSpec_ = &P;
	}

	const bool RankOrder = OrderUsesTextRank(OrderSpec_);
	const bool ComputedRankTopK = RankOrder && Col.BulkSyntheticPhysicalOrder && Col.BulkStep == 1 &&
	                              Col.BulkSyntheticLazyRankF32.size() != Col.RowCount;
	if(RankOrder && !ComputedRankTopK)
		EnsureBulkSyntheticLazyRankF32(Col);

	TopK_ = Limit;
	OrderAscending_ = OrderAscending;
	BatchSize_ = Prog.BatchSize > 0 ? Prog.BatchSize : 1024;
	TopKHeap_.clear();
	WorkerTopK_.clear();

	if(!Col.BulkSyntheticWhereDnfs.empty() && !Col.BulkSyntheticPassBits.empty()) {
		const BulkWhereDnf &Dnf = Col.BulkSyntheticWhereDnfs.back();
		const std::uint64_t QueryMask = BulkSyntheticDnfRequiredKindMask(Dnf, *Schema, nullptr);
		if(BulkSyntheticPassBitsCoverQuery(Col, QueryMask)) {
			EnsureBulkSyntheticPassSparseWords(Col);
			PassBitTopKParams Params;
			Params.Bits = Col.BulkSyntheticPassBits.data();
			Params.RowCount = Col.RowCount;
			Params.K = Limit;
			Params.Ascending = OrderAscending;
			const std::size_t Words = (Col.RowCount + 63) / 64;
			if(!Col.BulkSyntheticPassSparseWords.empty() &&
			   Col.BulkSyntheticPassSparseWords.size() * 4 <= Words * 3) {
				Params.SparsePassWords = Col.BulkSyntheticPassSparseWords.data();
				Params.SparsePassWordCount = Col.BulkSyntheticPassSparseWords.size();
			}
			if(!Col.BulkSyntheticPassGroupCounts.empty()) {
				Params.PassGroupCounts = Col.BulkSyntheticPassGroupCounts.data();
				Params.PassGroupCount = Col.BulkSyntheticPassGroupCounts.size();
			}
			Params.KnownPassCount = BulkSyntheticCountPassBits(Col);
			if(ComputedRankTopK) {
				Params.PhysicalRankStart = Col.BulkStartId;
				Params.PhysicalRankStep = 1;
			} else if(RankOrder && Col.BulkSyntheticLazyRankF32.size() == Col.RowCount) {
				Params.Keys = Col.BulkSyntheticLazyRankF32.data();
			} else {
				Params.KeyFn = [&](const std::size_t RowIndex) -> float {
					const int64_t RowId = BulkSyntheticRowIdAt(Col, RowIndex);
					if(OrderUsesTextRank(OrderSpec_))
						return BulkSyntheticBioRankF32Row(RowId);
					return static_cast<float>(RowId);
				};
			}
			std::vector<std::size_t> Winners;
			SemistructuredMicrokernels::SelectTopKPassBits(Params, Winners);
			if(Winners.empty())
				return true;
			return MaterializeSemistructuredWinners(Col, *Schema, Projections, Winners, Out);
		}
	}

	if(Prog.UseParallel && Col.RowCount >= 256'000) {
		const char *Par = EnvGet("ASTRALDB_SEMISTRUCTURED_PARALLEL");
		if(Par == nullptr || (Par[0] != '0' && Par[0] != 'n' && Par[0] != 'N'))
			ExecuteParallel(Prog, Col, *Schema, FilterDnf, Projections, OrderCol, OrderAscending, Limit);
		else
			ExecuteFused(Prog, Col, *Schema, FilterDnf, Projections, OrderCol, OrderAscending, Limit, 0, Col.RowCount);
	} else {
		ExecuteFused(Prog, Col, *Schema, FilterDnf, Projections, OrderCol, OrderAscending, Limit, 0, Col.RowCount);
	}

	MergeTopKHeaps();
	if(TopKHeap_.empty())
		return true;

	std::vector<std::size_t> Winners;
	Winners.reserve(TopKHeap_.size());
	for(const auto &E : TopKHeap_)
		Winners.push_back(static_cast<std::size_t>(E.second));

	return MaterializeSemistructuredWinners(Col, *Schema, Projections, Winners, Out);
}

void SemistructuredVM::ExecuteFused(const SSProgram &Prog, const ColumnarTable &Col,
                                    const std::vector<Database::Column> &Schema, const BulkWhereDnf &FilterDnf,
                                    const std::vector<SemistructuredProjectionSpec> &Projections,
                                    const std::string &OrderCol, const bool OrderAscending, const std::size_t Limit,
                                    const std::size_t StartRow, const std::size_t EndRow) {
	(void)OrderCol;
	OrderSpec_ = nullptr;
	for(const SemistructuredProjectionSpec &P : Projections) {
		if(P.OutCol == OrderCol)
			OrderSpec_ = &P;
	}
	TopK_ = Limit;
	OrderAscending_ = OrderAscending;
	BatchSize_ = Prog.BatchSize > 0 ? Prog.BatchSize : 1024;
	for(std::size_t B = StartRow; B < EndRow; B += BatchSize_)
		ProcessBatch(Prog, Col, Schema, FilterDnf, B, std::min(B + BatchSize_, EndRow));
}

void SemistructuredVM::ExecuteParallel(const SSProgram &Prog, const ColumnarTable &Col,
                                       const std::vector<Database::Column> &Schema, const BulkWhereDnf &FilterDnf,
                                       const std::vector<SemistructuredProjectionSpec> &Projections,
                                       const std::string &OrderCol, const bool OrderAscending,
                                       const std::size_t Limit) {
	if(!JobSystem::Instance().IsRunning()) {
		ExecuteFused(Prog, Col, Schema, FilterDnf, Projections, OrderCol, OrderAscending, Limit, 0, Col.RowCount);
		return;
	}
	const std::size_t NumBatches = (Col.RowCount + BatchSize_ - 1) / BatchSize_;
	const std::size_t Workers =
	    std::min<std::size_t>(8, std::max<std::size_t>(1, std::thread::hardware_concurrency()));
	const std::size_t BatchesPerWorker = (NumBatches + Workers - 1) / Workers;
	WorkerTopK_.assign(Workers, {});
	std::vector<std::future<std::vector<std::pair<float, std::uint64_t>>>> HeapFuts;
	HeapFuts.reserve(Workers);
	for(std::size_t W = 0; W < Workers; ++W) {
		const std::size_t BatchBegin = W * BatchesPerWorker;
		if(BatchBegin >= NumBatches)
			break;
		const std::size_t BatchEnd = std::min(NumBatches, BatchBegin + BatchesPerWorker);
		const std::size_t RowBegin = BatchBegin * BatchSize_;
		const std::size_t RowEnd = std::min(Col.RowCount, BatchEnd * BatchSize_);
		HeapFuts.push_back(JobSystem::Instance().SubmitAsync(
		    [&Prog, &Col, &Schema, &FilterDnf, &Projections, OrderCol, OrderAscending, Limit, RowBegin, RowEnd,
		     BatchSize = BatchSize_, OrderSpec = OrderSpec_]() {
			    SemistructuredVM Local;
			    Local.BatchSize_ = BatchSize;
			    Local.OrderSpec_ = OrderSpec;
			    Local.ExecuteFused(Prog, Col, Schema, FilterDnf, Projections, OrderCol, OrderAscending, Limit, RowBegin,
			                       RowEnd);
			    return std::move(Local.TopKHeap_);
		    }));
	}
	WorkerTopK_.clear();
	WorkerTopK_.reserve(HeapFuts.size());
	for(auto &F : HeapFuts)
		WorkerTopK_.push_back(F.get());
}

void SemistructuredVM::ProcessBatch(const SSProgram &Prog, const ColumnarTable &Col,
                                    const std::vector<Database::Column> &Schema, const BulkWhereDnf &FilterDnf,
                                    const std::size_t BatchStart, const std::size_t BatchEnd) {
	const std::size_t N = BatchEnd - BatchStart;
	if(N == 0)
		return;
	PredicateMasks_.assign(N, 1);
	ApplyPredicatesBatch(Prog, Col, Schema, FilterDnf, BatchStart, BatchEnd, PredicateMasks_.data());

	Ranks_.clear();
	RowIds_.clear();
	FillRankBatch(Col, OrderSpec_, BatchStart, BatchEnd, PredicateMasks_.data(), Ranks_, RowIds_);
	TopKInsertBatch(Ranks_.data(), RowIds_.data(), Ranks_.size());
}

void SemistructuredVM::ApplyPredicatesBatch(const SSProgram &Prog, const ColumnarTable &Col,
                                            const std::vector<Database::Column> &Schema, const BulkWhereDnf &FilterDnf,
                                            const std::size_t BatchStart, const std::size_t BatchEnd,
                                            std::uint8_t *Mask) {
	const std::size_t N = BatchEnd - BatchStart;
	std::vector<std::uint8_t> Scratch(N);

	auto ApplyLazyPredicate = [&](const std::string &ColName, const std::string &Op, const std::string &Rhs) {
		const Database::Column *ColDef = FindSchemaCol(Schema, ColName);
		for(std::size_t I = 0; I < N; ++I) {
			const std::size_t RowIndex = BatchStart + I;
			const int64_t RowId = BulkSyntheticRowIdAt(Col, RowIndex);
			const auto Fast = BulkSyntheticTryMatchPredicate(RowId, ColName, Op, Rhs, ColDef);
			Scratch[I] = Fast && *Fast ? 1 : 0;
		}
#if defined(__AVX2__)
		for(std::size_t I = 0; I < N; ++I)
			Mask[I] &= Scratch[I];
#else
		for(std::size_t I = 0; I < N; ++I)
			Mask[I] &= Scratch[I];
#endif
	};

	for(const SSInstruction &Inst : Prog.Instructions) {
		if(Inst.Op == SSOpcode::JSON_EXTRACT_BATCH && Inst.StrOperands.size() >= 3) {
			const std::string &ColName = Inst.StrOperands[0];
			const std::string &Path = Inst.StrOperands[1];
			const std::string &Expected = Inst.StrOperands[2];
			if(Col.BulkSyntheticLazy) {
				ApplyLazyPredicate(ColName, "JSON_EXTRACT", Path + std::string("\x1E") + Expected);
				continue;
			}
			const auto It = Col.Columns.find(ColName);
			if(It == Col.Columns.end())
				continue;
			std::vector<const char *> Ptrs(N);
			std::vector<std::size_t> Lens(N);
			for(std::size_t I = 0; I < N; ++I) {
				const std::size_t Ri = BatchStart + I;
				const std::string &Cell = Ri < It->second.size() ? It->second[Ri] : It->second.back();
				Ptrs[I] = Cell.data();
				Lens[I] = Cell.size();
			}
			JsonBatchKernel::MatchEqBatch(Ptrs.data(), Lens.data(), Path.c_str(), Expected.c_str(), N, Scratch.data());
			for(std::size_t I = 0; I < N; ++I)
				Mask[I] &= Scratch[I];
		} else if(Inst.Op == SSOpcode::XML_EXTRACT_BATCH && !Inst.StrOperands.empty()) {
			const std::string &ColName = Inst.StrOperands[0];
			if(!Inst.IntOperands.empty() && Inst.IntOperands[0] == 1) {
				if(Col.BulkSyntheticLazy) {
					ApplyLazyPredicate(ColName, "XML_VALID", "1");
					continue;
				}
				const auto It = Col.Columns.find(ColName);
				if(It == Col.Columns.end())
					continue;
				std::vector<const char *> Ptrs(N);
				std::vector<std::size_t> Lens(N);
				for(std::size_t I = 0; I < N; ++I) {
					const std::size_t Ri = BatchStart + I;
					const std::string &Cell = Ri < It->second.size() ? It->second[Ri] : It->second.back();
					Ptrs[I] = Cell.data();
					Lens[I] = Cell.size();
				}
				XmlBatchKernel::ValidBatch(Ptrs.data(), Lens.data(), N, Scratch.data());
				for(std::size_t I = 0; I < N; ++I)
					Mask[I] &= Scratch[I];
			}
		} else if(Inst.Op == SSOpcode::REGEX_BATCH && Inst.StrOperands.size() >= 2) {
			const std::string &ColName = Inst.StrOperands[0];
			const std::string &Pattern = Inst.StrOperands[1];
			if(Col.BulkSyntheticLazy) {
				ApplyLazyPredicate(ColName, "REGEXP", Pattern);
				continue;
			}
			const auto It = Col.Columns.find(ColName);
			if(It == Col.Columns.end())
				continue;
			std::vector<const char *> Ptrs(N);
			std::vector<std::size_t> Lens(N);
			for(std::size_t I = 0; I < N; ++I) {
				const std::size_t Ri = BatchStart + I;
				const std::string &Cell = Ri < It->second.size() ? It->second[Ri] : It->second.back();
				Ptrs[I] = Cell.data();
				Lens[I] = Cell.size();
			}
			RegexBatchKernel::MatchBatch(Ptrs.data(), Lens.data(), Pattern.c_str(), N, Scratch.data());
			for(std::size_t I = 0; I < N; ++I)
				Mask[I] &= Scratch[I];
		} else if(Inst.Op == SSOpcode::FTS_MATCH_BATCH && Inst.StrOperands.size() >= 2) {
			const std::string &ColName = Inst.StrOperands[0];
			const std::string &Query = Inst.StrOperands[1];
			if(Col.BulkSyntheticLazy) {
				ApplyLazyPredicate(ColName, "MATCH", Query);
				continue;
			}
		}
	}

	if(!FilterDnf.empty() && FilterDnf.front().size() > 1) {
		for(const auto &Pred : FilterDnf.front()) {
			const auto &[ColName, Op, Lit] = Pred;
			ApplyLazyPredicate(ColName, Op, Lit);
		}
	}
}

void SemistructuredVM::FillRankBatch(const ColumnarTable &Col, const SemistructuredProjectionSpec *OrderSpec,
                                     const std::size_t BatchStart, const std::size_t BatchEnd, const std::uint8_t *Mask,
                                     std::vector<float> &Ranks, std::vector<std::uint64_t> &RowIds) {
	const float *DenseRank =
	    Col.BulkSyntheticLazyRankF32.size() == Col.RowCount ? Col.BulkSyntheticLazyRankF32.data() : nullptr;
	for(std::size_t I = 0; I < BatchEnd - BatchStart; ++I) {
		if(!Mask[I])
			continue;
		const std::size_t RowIndex = BatchStart + I;
		float Key = 0.f;
		if(DenseRank != nullptr)
			Key = DenseRank[RowIndex];
		else {
			const int64_t RowId = BulkSyntheticRowIdAt(Col, RowIndex);
			Key = OrderUsesTextRank(OrderSpec) ? BulkSyntheticBioRankF32Row(RowId) : static_cast<float>(RowId);
		}
		Ranks.push_back(Key);
		RowIds.push_back(static_cast<std::uint64_t>(RowIndex));
	}
}

void SemistructuredVM::TopKInsertBatch(const float *Ranks, const std::uint64_t *Ids, const std::size_t N) {
	if(N == 0 || TopK_ == 0)
		return;
	if(TopK_ >= 100'000 && N > TopK_ * 2) {
		std::vector<std::pair<float, std::uint64_t>> BatchOut;
		TopKBatchKernel::PartialSortBatch(Ranks, Ids, N, TopK_, BatchOut);
		for(const auto &E : BatchOut) {
			const float Key = E.first;
			const std::uint64_t Id = E.second;
			TopKBatchKernel::InsertBatch(&Key, &Id, 1, TopK_, OrderAscending_, TopKHeap_);
		}
		return;
	}
	TopKBatchKernel::InsertBatch(Ranks, Ids, N, TopK_, OrderAscending_, TopKHeap_);
}

void SemistructuredVM::MergeTopKHeaps() {
	if(WorkerTopK_.empty())
		return;
	std::vector<std::pair<float, std::uint64_t>> Merged;
	MergeWorkerTopK(WorkerTopK_, TopK_, OrderAscending_, Merged);
	TopKHeap_ = std::move(Merged);
}

void SemistructuredVM::MaterializeBatch(const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                                          const std::vector<SemistructuredProjectionSpec> &Projections, RowTable &Out) {
	std::vector<std::size_t> Winners;
	Winners.reserve(TopKHeap_.size());
	for(const auto &E : TopKHeap_)
		Winners.push_back(static_cast<std::size_t>(E.second));
	MaterializeSemistructuredWinners(Col, Schema, Projections, Winners, Out);
}

} // namespace SQL
} // namespace AstralDB
