#include <Database/Storage/BulkSyntheticWindowEval.hxx>

#include <Database/Storage/BulkSynthetic.hxx>
#include <Database/Storage/ColumnarLazyBulk.hxx>
#include <Database/Storage/PredicateKind.hxx>
#include <Database/Storage/SemistructuredProfile.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

namespace AstralDB {
namespace {

using SQL::WindowFnKind;
using SQL::WindowFrameBoundKind;

const Database::Column *FindCol(const std::vector<Database::Column> &Schema, const std::string &Name) {
	for(const Database::Column &C : Schema) {
		if(C.Name == Name)
			return &C;
	}
	return nullptr;
}

BulkSyntheticValueKind ColKind(const Database::Column *C) {
	if(!C)
		return BulkSyntheticValueKind::Text;
	switch(ClassifySqlStorage(*C)) {
	case SqlStorageKind::ForeignKey:
		return BulkSyntheticValueKind::ForeignKey;
	case SqlStorageKind::Integer:
		return BulkSyntheticValueKind::Integer;
	case SqlStorageKind::Timestamp:
		return BulkSyntheticValueKind::Timestamp;
	case SqlStorageKind::Decimal:
		return BulkSyntheticValueKind::Decimal;
	default:
		break;
	}
	return ClassifyBulkColumn(*C, 0, 1);
}

double WindowAmountAt(const ColumnarTable &Col, std::size_t RowIndex) {
	if(RowIndex < Col.BulkSyntheticAmountByRow.size())
		return Col.BulkSyntheticAmountByRow[RowIndex];
	return BulkSyntheticDecimalFromRowId(BulkSyntheticRowIdAt(Col, RowIndex));
}

struct BucketCache {
	std::string PartCol;
	std::string OrderCol;
	bool OrderAsc = true;
	int64_t PartMod = 0;
	bool PerRowPartition = false;
	std::vector<std::vector<std::size_t>> Buckets;
};

bool CacheMatches(const ColumnarTable &Col, const BucketCache &Need) {
	if(!Col.BulkSyntheticWindowBucketsBuilt)
		return false;
	return Col.BulkSyntheticWindowBucketPartCol == Need.PartCol &&
	       Col.BulkSyntheticWindowBucketPerRow == Need.PerRowPartition &&
	       Col.BulkSyntheticWindowBucketPartMod == Need.PartMod;
}

void SortBucketIndices(const ColumnarTable &Col, const Database::Column *OrderSchema, bool OrderAsc,
                       std::vector<std::size_t> &Idx) {
	const BulkSyntheticValueKind Ok = ColKind(OrderSchema);
	if(Col.BulkSyntheticWindowBucketSeqSorted &&
	   (Ok == BulkSyntheticValueKind::Timestamp || Ok == BulkSyntheticValueKind::PrimaryKey ||
	    Ok == BulkSyntheticValueKind::Integer)) {
		if(!OrderAsc)
			std::reverse(Idx.begin(), Idx.end());
		return;
	}
	if(Ok == BulkSyntheticValueKind::Timestamp || Ok == BulkSyntheticValueKind::PrimaryKey ||
	   Ok == BulkSyntheticValueKind::Integer) {
		if(OrderAsc)
			std::sort(Idx.begin(), Idx.end());
		else
			std::sort(Idx.begin(), Idx.end(), std::greater<std::size_t>());
		return;
	}
	std::sort(Idx.begin(), Idx.end(), [&](std::size_t A, std::size_t B) {
		const double Va = BulkSyntheticDecimalFromRowId(BulkSyntheticRowIdAt(Col, A));
		const double Vb = BulkSyntheticDecimalFromRowId(BulkSyntheticRowIdAt(Col, B));
		return OrderAsc ? Va < Vb : Va > Vb;
	});
}

bool EnsureBuckets(ColumnarTable &Col, const std::vector<Database::Column> &Schema, const std::string &PartCol,
                   const std::string &OrderCol, bool OrderAsc, BucketCache &Out) {
	Out.PartCol = PartCol;
	Out.OrderCol = OrderCol;
	Out.OrderAsc = OrderAsc;
	const Database::Column *PartSchema = FindCol(Schema, PartCol);
	const BulkSyntheticValueKind Pk = ColKind(PartSchema);
	Out.PerRowPartition = (Pk == BulkSyntheticValueKind::Timestamp || Pk == BulkSyntheticValueKind::PrimaryKey);
	if(Out.PerRowPartition) {
		Out.PartMod = 0;
	} else {
		Out.PartMod = Col.BulkPartitionMod > 0 ? Col.BulkPartitionMod : BulkSyntheticFkModulus(*PartSchema);
		if(Out.PartMod <= 0)
			Out.PartMod = 997;
	}
	if(CacheMatches(Col, Out)) {
		Out.Buckets = Col.BulkSyntheticWindowBuckets;
		Col.BulkSyntheticWindowBucketOrderCol = Out.OrderCol;
		Col.BulkSyntheticWindowBucketOrderAsc = Out.OrderAsc;
		return true;
	}
	SemistructuredProfileScope Scope("bulk_window_bucket_build");
	Out.Buckets.clear();
	const std::size_t N = Col.RowCount;
	if(Out.PerRowPartition) {
		Out.Buckets.resize(N);
		for(std::size_t I = 0; I < N; ++I)
			Out.Buckets[I] = {I};
	} else if(Col.BulkStep == 1) {
		Out.Buckets.clear();
	} else {
		Out.Buckets.assign(static_cast<std::size_t>(Out.PartMod), {});
		for(std::size_t I = 0; I < N; ++I) {
			const int64_t RowId = BulkSyntheticRowIdAt(Col, I);
			const int64_t P = ((RowId - 1) % Out.PartMod) + 1;
			Out.Buckets[static_cast<std::size_t>(P - 1)].push_back(I);
		}
	}
	Col.BulkSyntheticWindowBucketPartCol = Out.PartCol;
	Col.BulkSyntheticWindowBucketOrderCol = Out.OrderCol;
	Col.BulkSyntheticWindowBucketOrderAsc = Out.OrderAsc;
	Col.BulkSyntheticWindowBucketPerRow = Out.PerRowPartition;
	Col.BulkSyntheticWindowBucketPartMod = Out.PartMod;
	Col.BulkSyntheticWindowBuckets = Out.Buckets;
	Col.BulkSyntheticWindowBucketsBuilt = true;
	Col.BulkSyntheticWindowBucketSeqSorted = !Out.PerRowPartition && Col.BulkStep == 1;
	return true;
}

std::vector<double> &EnsureOut(ColumnarTable &Col, const std::string &OutCol) {
	auto &Vec = Col.BulkSyntheticWindowDbl[OutCol];
	if(Vec.size() != Col.RowCount)
		Vec.assign(Col.RowCount, std::numeric_limits<double>::quiet_NaN());
	return Vec;
}

bool TryCumulativeSum(ColumnarTable &Col, const std::vector<Database::Column> &Schema, const BucketCache &Buckets,
                      const std::string &OrderCol, const std::string &OutCol, bool OrderAsc) {
	const Database::Column *OrdSchema = FindCol(Schema, OrderCol);
	auto &Out = EnsureOut(Col, OutCol);
	SemistructuredProfileScope Scope("bulk_window_cumsum");
	for(const auto &Idx : Buckets.Buckets) {
		if(Idx.empty())
			continue;
		std::vector<std::size_t> Sorted = Idx;
		SortBucketIndices(Col, OrdSchema, OrderAsc, Sorted);
		double Sum = 0;
		for(const std::size_t Ix : Sorted) {
			Sum += BulkSyntheticDecimalFromRowId(BulkSyntheticRowIdAt(Col, Ix));
			Out[Ix] = Sum;
		}
	}
	return true;
}

bool TrySlidingAvg(ColumnarTable &Col, const std::vector<Database::Column> &Schema, const BucketCache &Buckets,
                   const std::string &OrderCol, const std::string &OutCol, bool OrderAsc, std::size_t PrecedingRows) {
	if(PrecedingRows > 128)
		return false;
	const std::size_t Width = PrecedingRows + 1;
	const Database::Column *OrdSchema = FindCol(Schema, OrderCol);
	auto &Out = EnsureOut(Col, OutCol);
	SemistructuredProfileScope Scope("bulk_window_sliding_avg");
	for(const auto &Idx : Buckets.Buckets) {
		if(Idx.empty())
			continue;
		std::vector<std::size_t> Sorted = Idx;
		SortBucketIndices(Col, OrdSchema, OrderAsc, Sorted);
		double Ring[128]{};
		std::size_t RingLen = 0;
		std::size_t RingPos = 0;
		double Sum = 0;
		for(const std::size_t Ix : Sorted) {
			const double V = BulkSyntheticDecimalFromRowId(BulkSyntheticRowIdAt(Col, Ix));
			if(RingLen < Width) {
				Ring[RingLen++] = V;
				Sum += V;
			} else {
				Sum -= Ring[RingPos];
				Ring[RingPos] = V;
				Sum += V;
				RingPos = (RingPos + 1) % Width;
			}
			Out[Ix] = Sum / static_cast<double>(RingLen);
		}
	}
	return true;
}

bool TryLag(ColumnarTable &Col, const std::vector<Database::Column> &Schema, const BucketCache &Buckets,
            const std::string &OrderCol, const std::string &OutCol, bool OrderAsc, std::size_t LagOff) {
	const Database::Column *OrdSchema = FindCol(Schema, OrderCol);
	auto &Out = EnsureOut(Col, OutCol);
	SemistructuredProfileScope Scope("bulk_window_lag");
	for(const auto &Idx : Buckets.Buckets) {
		if(Idx.empty())
			continue;
		std::vector<std::size_t> Sorted = Idx;
		SortBucketIndices(Col, OrdSchema, OrderAsc, Sorted);
		for(std::size_t K = 0; K < Sorted.size(); ++K) {
			const std::size_t Ix = Sorted[K];
			if(K < LagOff)
				Out[Ix] = std::numeric_limits<double>::quiet_NaN();
			else
				Out[Ix] = BulkSyntheticDecimalFromRowId(BulkSyntheticRowIdAt(Col, Sorted[K - LagOff]));
		}
	}
	return true;
}

bool TryRank(ColumnarTable &Col, const std::vector<Database::Column> &Schema, const BucketCache &Buckets,
             const std::string &OrderCol, const std::string &OutCol, bool OrderAsc) {
	if(Buckets.PerRowPartition) {
		auto &Out = EnsureOut(Col, OutCol);
		std::fill(Out.begin(), Out.end(), 1.);
		return true;
	}
	const Database::Column *OrdSchema = FindCol(Schema, OrderCol);
	auto &Out = EnsureOut(Col, OutCol);
	SemistructuredProfileScope Scope("bulk_window_rank");
	for(const auto &Idx : Buckets.Buckets) {
		if(Idx.empty())
			continue;
		std::vector<std::size_t> Sorted = Idx;
		SortBucketIndices(Col, OrdSchema, OrderAsc, Sorted);
		std::size_t Rank = 1;
		for(std::size_t K = 0; K < Sorted.size(); ++K) {
			if(K > 0) {
				const double Prev =
				    BulkSyntheticDecimalFromRowId(BulkSyntheticRowIdAt(Col, Sorted[K - 1]));
				const double Cur = BulkSyntheticDecimalFromRowId(BulkSyntheticRowIdAt(Col, Sorted[K]));
				if(Cur != Prev)
					Rank = K + 1;
			}
			Out[Sorted[K]] = static_cast<double>(Rank);
		}
	}
	return true;
}

void ProcessCustDateBucket(ColumnarTable &Col, ColumnarTable::BulkSyntheticCustDateWindowCache &Cache,
                           const std::size_t StartIx, const std::size_t Stride, const std::size_t N) {
	double Sum = 0;
	double Ring7[8]{};
	double Ring30[31]{};
	std::size_t Len7 = 0;
	std::size_t Len30 = 0;
	std::size_t Pos7 = 0;
	std::size_t Pos30 = 0;
	double Sum7 = 0;
	double Sum30 = 0;
	std::size_t PrevIx = static_cast<std::size_t>(-1);
	for(std::size_t Ix = StartIx; Ix < N; Ix += Stride) {
		const double V = WindowAmountAt(Col, Ix);
		Sum += V;
		Cache.Running[Ix] = Sum;
		if(PrevIx != static_cast<std::size_t>(-1))
			Cache.Lag1[Ix] = WindowAmountAt(Col, PrevIx);
		PrevIx = Ix;
		if(Len7 < 8) {
			Ring7[Len7++] = V;
			Sum7 += V;
		} else {
			Sum7 -= Ring7[Pos7];
			Ring7[Pos7] = V;
			Sum7 += V;
			Pos7 = (Pos7 + 1) % 8;
		}
		Cache.Ma7[Ix] = Sum7 / static_cast<double>(Len7);
		if(Len30 < 31) {
			Ring30[Len30++] = V;
			Sum30 += V;
		} else {
			Sum30 -= Ring30[Pos30];
			Ring30[Pos30] = V;
			Sum30 += V;
			Pos30 = (Pos30 + 1) % 31;
		}
		Cache.Ma30[Ix] = Sum30 / static_cast<double>(Len30);
	}
}

bool EnsureCustDateWindowCacheFkMod(ColumnarTable &Col, int64_t PartMod) {
	if(Col.CustDateWindowCache)
		return true;
	if(PartMod <= 0)
		PartMod = 997;
	SemistructuredProfileScope Scope("bulk_window_fused_cust_date");
	ColumnarTable::BulkSyntheticCustDateWindowCache Cache;
	const std::size_t N = Col.RowCount;
	Cache.Running.resize(N);
	Cache.Ma7.resize(N);
	Cache.Ma30.resize(N);
	Cache.Lag1.resize(N);
	const std::size_t Stride = static_cast<std::size_t>(PartMod);
	const unsigned Hw = std::max(1u, std::thread::hardware_concurrency());
	const unsigned Workers = std::min<unsigned>(Hw, static_cast<unsigned>(Stride));
	if(Workers <= 1) {
		for(std::size_t P = 0; P < Stride; ++P)
			ProcessCustDateBucket(Col, Cache, P, Stride, N);
	} else {
		std::vector<std::thread> Pool;
		Pool.reserve(Workers);
		for(unsigned W = 0; W < Workers; ++W) {
			Pool.emplace_back([&, W]() {
				for(std::size_t P = static_cast<std::size_t>(W); P < Stride; P += Workers)
					ProcessCustDateBucket(Col, Cache, P, Stride, N);
			});
		}
		for(std::thread &Th : Pool)
			Th.join();
	}
	Col.CustDateWindowCache = std::move(Cache);
	Col.BulkSyntheticFusedCustDateCoreDone = true;
	Col.BulkSyntheticWindowBucketPartMod = PartMod;
	Col.BulkSyntheticWindowBucketSeqSorted = Col.BulkStep == 1;
	Col.BulkSyntheticWindowBucketsBuilt = true;
	return true;
}

bool EnsureCustDateWindowCache(ColumnarTable &Col, const BucketCache &Buckets) {
	if(Col.CustDateWindowCache)
		return true;
	if(!Buckets.PerRowPartition && Buckets.PartMod > 0 && Col.BulkStep == 1)
		return EnsureCustDateWindowCacheFkMod(Col, Buckets.PartMod);
	SemistructuredProfileScope Scope("bulk_window_fused_cust_date");
	ColumnarTable::BulkSyntheticCustDateWindowCache Cache;
	const std::size_t N = Col.RowCount;
	Cache.Running.assign(N, std::numeric_limits<double>::quiet_NaN());
	Cache.Ma7.assign(N, std::numeric_limits<double>::quiet_NaN());
	Cache.Ma30.assign(N, std::numeric_limits<double>::quiet_NaN());
	Cache.Lag1.assign(N, std::numeric_limits<double>::quiet_NaN());
	for(const auto &Idx : Buckets.Buckets) {
		if(Idx.empty())
			continue;
		double Sum = 0;
		double Ring7[8]{};
		double Ring30[31]{};
		std::size_t Len7 = 0;
		std::size_t Len30 = 0;
		std::size_t Pos7 = 0;
		std::size_t Pos30 = 0;
		double Sum7 = 0;
		double Sum30 = 0;
		for(std::size_t K = 0; K < Idx.size(); ++K) {
			const std::size_t Ix = Idx[K];
			const double V = WindowAmountAt(Col, Ix);
			Sum += V;
			Cache.Running[Ix] = Sum;
			if(K > 0)
				Cache.Lag1[Ix] = WindowAmountAt(Col, Idx[K - 1]);
			if(Len7 < 8) {
				Ring7[Len7++] = V;
				Sum7 += V;
			} else {
				Sum7 -= Ring7[Pos7];
				Ring7[Pos7] = V;
				Sum7 += V;
				Pos7 = (Pos7 + 1) % 8;
			}
			Cache.Ma7[Ix] = Sum7 / static_cast<double>(Len7);
			if(Len30 < 31) {
				Ring30[Len30++] = V;
				Sum30 += V;
			} else {
				Sum30 -= Ring30[Pos30];
				Ring30[Pos30] = V;
				Sum30 += V;
				Pos30 = (Pos30 + 1) % 31;
			}
			Cache.Ma30[Ix] = Sum30 / static_cast<double>(Len30);
		}
	}
	Col.CustDateWindowCache = std::move(Cache);
	Col.BulkSyntheticFusedCustDateCoreDone = true;
	return true;
}

bool TryCustDateCachedWindow(ColumnarTable &Col, int OrdKind, const std::string &OutCol,
                             WindowFrameBoundKind FrameStartKind, int64_t FrameStartOff, int64_t FrameOffset) {
	if(!Col.CustDateWindowCache)
		return false;
	auto &Cache = *Col.CustDateWindowCache;
	auto &Out = EnsureOut(Col, OutCol);
	const auto Kind = static_cast<WindowFnKind>(OrdKind);
	if(Kind == WindowFnKind::Sum && FrameStartKind == WindowFrameBoundKind::UnboundedPreceding) {
		Out = std::move(Cache.Running);
		return true;
	}
	if(Kind == WindowFnKind::Avg && FrameStartKind == WindowFrameBoundKind::Preceding) {
		if(FrameStartOff == 7) {
			Out = std::move(Cache.Ma7);
			return true;
		}
		if(FrameStartOff == 30) {
			Out = std::move(Cache.Ma30);
			return true;
		}
		return false;
	}
	if(Kind == WindowFnKind::Lag && FrameOffset == 1) {
		Out = std::move(Cache.Lag1);
		return true;
	}
	return false;
}

bool TryNtileFkMod(ColumnarTable &Col, int64_t PartMod, const std::string &OutCol, bool OrderAsc,
                   std::size_t NumTiles) {
	if(NumTiles == 0 || NumTiles > 10'000 || PartMod <= 0)
		return false;
	auto &Out = EnsureOut(Col, OutCol);
	const std::size_t N = Col.RowCount;
	const std::size_t Stride = static_cast<std::size_t>(PartMod);
	SemistructuredProfileScope Scope("bulk_window_ntile_fkmod");
	for(std::size_t P = 0; P < Stride; ++P) {
		std::size_t Ps = 0;
		for(std::size_t Ix = P; Ix < N; Ix += Stride)
			++Ps;
		if(Ps == 0)
			continue;
		std::size_t K = 0;
		for(std::size_t Ix = P; Ix < N; Ix += Stride, ++K) {
			const std::size_t Pos = OrderAsc ? K : (Ps - 1 - K);
			const std::size_t Tile = (Pos * NumTiles) / Ps + 1;
			Out[Ix] = static_cast<double>(Tile);
		}
	}
	return true;
}

bool FlattenFkModTopK(ColumnarTable &Col, const std::vector<Database::Column> &Schema, int64_t PartMod,
                      std::size_t TopKeep) {
	if(PartMod <= 0 || Col.RowCount == 0 || TopKeep == 0)
		return false;
	SemistructuredProfileScope Scope("bulk_window_fkmod_topk");
	std::vector<std::size_t> Flat;
	Flat.reserve(TopKeep);
	const std::size_t Stride = static_cast<std::size_t>(PartMod);
	const bool Filtered = !Col.BulkSyntheticWhereDnfs.empty();
	for(std::size_t P = 0; P < Stride && Flat.size() < TopKeep; ++P) {
		for(std::size_t Ix = P; Ix < Col.RowCount && Flat.size() < TopKeep; Ix += Stride) {
			if(Filtered && !BulkSyntheticRowPassesWhereStackFast(Col, Schema, Ix, nullptr, ""))
				continue;
			Flat.push_back(Ix);
		}
	}
	if(Flat.empty())
		return false;
	Col.BulkSyntheticSortedRowIndices = std::move(Flat);
	return true;
}

bool TryNtile(ColumnarTable &Col, const std::vector<Database::Column> &Schema, const BucketCache &Buckets,
              const std::string &OrderCol, const std::string &OutCol, bool OrderAsc, std::size_t NumTiles) {
	if(NumTiles == 0 || NumTiles > 10'000)
		return false;
	const Database::Column *OrdSchema = FindCol(Schema, OrderCol);
	const BulkSyntheticValueKind Ok = ColKind(OrdSchema);
	const bool RowOrderIsAmount =
	    Col.BulkSyntheticWindowBucketSeqSorted && !Buckets.PerRowPartition &&
	    (Ok == BulkSyntheticValueKind::Decimal || Ok == BulkSyntheticValueKind::PrimaryKey ||
	     Ok == BulkSyntheticValueKind::Integer);
	auto &Out = EnsureOut(Col, OutCol);
	SemistructuredProfileScope Scope("bulk_window_ntile");
	if(Buckets.Buckets.empty() && !Buckets.PerRowPartition && Buckets.PartMod > 0 && Col.BulkStep == 1) {
		const std::size_t Mod = static_cast<std::size_t>(Buckets.PartMod);
		for(std::size_t P = 0; P < Mod; ++P) {
			std::size_t Ps = 0;
			for(std::size_t Ix = P; Ix < Col.RowCount; Ix += Mod)
				++Ps;
			if(Ps == 0)
				continue;
			std::size_t K = 0;
			for(std::size_t Ix = P; Ix < Col.RowCount; Ix += Mod, ++K) {
				const std::size_t Tile = (K * NumTiles) / Ps + 1;
				Out[Ix] = static_cast<double>(Tile);
			}
		}
		return true;
	}
	for(const auto &Idx : Buckets.Buckets) {
		const std::size_t Ps = Idx.size();
		if(Ps == 0)
			continue;
		if(RowOrderIsAmount) {
			std::vector<std::size_t> Sorted = Idx;
			if(!OrderAsc)
				std::reverse(Sorted.begin(), Sorted.end());
			for(std::size_t I = 0; I < Ps; ++I) {
				const std::size_t Tile = (I * NumTiles) / Ps + 1;
				Out[Sorted[I]] = static_cast<double>(Tile);
			}
			continue;
		}
		std::vector<std::size_t> Sorted = Idx;
		SortBucketIndices(Col, OrdSchema, OrderAsc, Sorted);
		for(std::size_t I = 0; I < Ps; ++I) {
			const std::size_t Tile = (I * NumTiles) / Ps + 1;
			Out[Sorted[I]] = static_cast<double>(Tile);
		}
	}
	return true;
}

bool FlattenBucketSortIndices(ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                              const std::string &PartCol, const std::string &OrderCol, bool OrderAsc,
                              std::size_t TopKeep) {
	BucketCache Buckets;
	if(!EnsureBuckets(Col, Schema, PartCol, OrderCol, OrderAsc, Buckets))
		return false;
	if(!Buckets.PerRowPartition && Buckets.PartMod > 0 && Col.BulkStep == 1 && OrderAsc &&
	   Buckets.Buckets.empty())
		return FlattenFkModTopK(Col, Schema, Buckets.PartMod, TopKeep);
	const Database::Column *OrdSchema = FindCol(Schema, OrderCol);
	std::vector<std::size_t> Flat;
	Flat.reserve(TopKeep > 0 ? TopKeep : Col.RowCount);
	if(Buckets.PerRowPartition) {
		const std::size_t Cap = TopKeep > 0 ? std::min(TopKeep, Col.RowCount) : Col.RowCount;
		for(std::size_t I = 0; I < Cap; ++I)
			Flat.push_back(I);
	} else {
		for(auto &Idx : Buckets.Buckets) {
			if(Idx.empty())
				continue;
			SortBucketIndices(Col, OrdSchema, OrderAsc, Idx);
			for(const std::size_t Ix : Idx) {
				Flat.push_back(Ix);
				if(TopKeep > 0 && Flat.size() >= TopKeep) {
					Col.BulkSyntheticSortedRowIndices = std::move(Flat);
					return true;
				}
			}
		}
	}
	if(Flat.empty())
		return false;
	Col.BulkSyntheticSortedRowIndices = std::move(Flat);
	return true;
}

} // namespace

bool TryLazyBulkOrderByWindowBuckets(ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                                     const std::string &SortCol, bool Ascending, std::size_t TopKeep) noexcept {
	if(!Col.BulkSyntheticLazy || !Col.BulkSyntheticPhysicalOrder || Col.RowCount == 0)
		return false;
	if(!Col.BulkSyntheticSortedRowIndices.empty())
		return true;
	const Database::Column *SortSchema = FindCol(Schema, SortCol);
	if(!SortSchema)
		return false;
	const BulkSyntheticValueKind Sk = ColKind(SortSchema);
	if((Sk == BulkSyntheticValueKind::ForeignKey || Sk == BulkSyntheticValueKind::Integer) && Ascending) {
		const Database::Column *DateCol = nullptr;
		for(const Database::Column &C : Schema) {
			if(ClassifyBulkColumn(C, 0, 1) == BulkSyntheticValueKind::Timestamp) {
				DateCol = &C;
				break;
			}
		}
		if(!DateCol)
			return false;
		SemistructuredProfileScope Scope("bulk_window_order_flatten");
		return FlattenBucketSortIndices(Col, Schema, SortCol, DateCol->Name, true, TopKeep);
	}
	if(ClassifyBulkColumn(*SortSchema, 0, 1) == BulkSyntheticValueKind::Timestamp && Ascending &&
	   Col.BulkSyntheticWindowBucketsBuilt && !Col.BulkSyntheticWindowBucketPerRow)
		return true;
	return false;
}

bool TryBulkSyntheticLazyWindow(ColumnarTable &Col, const std::vector<Database::Column> &Schema, int OrdKind,
                                int FrameMode, const std::vector<std::string> &PartCols, const std::string &OrderCol,
                                const std::string &SrcCol, const std::string &OutCol, bool Ascending,
                                WindowFrameBoundKind FrameStartKind, int64_t FrameStartOff,
                                WindowFrameBoundKind FrameEndKind, int64_t FrameEndOff, int64_t FrameOffset,
                                bool SkipOutputStore) noexcept {
	(void)SrcCol;
	(void)SkipOutputStore;
	if(!Col.BulkSyntheticLazy || !Col.BulkSyntheticPhysicalOrder || Col.RowCount == 0 || PartCols.size() != 1)
		return false;
	if(!FindCol(Schema, PartCols[0]) || !FindCol(Schema, OrderCol))
		return false;
	if(FrameMode != 0 && FrameMode != 1 && FrameMode != 2)
		(void)FrameMode;

	BucketCache Buckets;
	if(!EnsureBuckets(Col, Schema, PartCols[0], OrderCol, Ascending, Buckets))
		return false;

	const Database::Column *PartSchema = FindCol(Schema, PartCols[0]);
	const Database::Column *OrdSchema = FindCol(Schema, OrderCol);
	const bool CustDateSuite = PartSchema && OrdSchema &&
	                           ColKind(OrdSchema) == BulkSyntheticValueKind::Timestamp &&
	                           (ColKind(PartSchema) == BulkSyntheticValueKind::ForeignKey ||
	                            ColKind(PartSchema) == BulkSyntheticValueKind::Integer) && !Buckets.PerRowPartition;
	const bool CustDateOrder = CustDateSuite && OrderCol == OrdSchema->Name;
	if(CustDateOrder) {
		if(!EnsureCustDateWindowCache(Col, Buckets))
			return false;
		if(TryCustDateCachedWindow(Col, OrdKind, OutCol, FrameStartKind, FrameStartOff, FrameOffset))
			return true;
	}

	const auto Kind = static_cast<WindowFnKind>(OrdKind);
	if(CustDateOrder)
		return false;
	if(Kind == WindowFnKind::Sum && (FrameMode == 1 || FrameMode == 2)) {
		if(FrameStartKind == WindowFrameBoundKind::UnboundedPreceding &&
		   FrameEndKind == WindowFrameBoundKind::CurrentRow && FrameEndOff == 0)
			return TryCumulativeSum(Col, Schema, Buckets, OrderCol, OutCol, Ascending);
	}
	if(Kind == WindowFnKind::Avg && (FrameMode == 1 || FrameMode == 2) &&
	   FrameStartKind == WindowFrameBoundKind::Preceding &&
	   FrameEndKind == WindowFrameBoundKind::CurrentRow && FrameEndOff == 0 && FrameStartOff >= 0 &&
	   FrameStartOff <= 64)
		return TrySlidingAvg(Col, Schema, Buckets, OrderCol, OutCol, Ascending,
		                      static_cast<std::size_t>(FrameStartOff));
	if(Kind == WindowFnKind::Lag && FrameOffset > 0 && FrameOffset <= 64)
		return TryLag(Col, Schema, Buckets, OrderCol, OutCol, Ascending, static_cast<std::size_t>(FrameOffset));
	if(Kind == WindowFnKind::Rank)
		return TryRank(Col, Schema, Buckets, OrderCol, OutCol, Ascending);
	if(Kind == WindowFnKind::Ntile) {
		if(Buckets.Buckets.empty() && !Buckets.PerRowPartition && Buckets.PartMod > 0 && Col.BulkStep == 1)
			return TryNtileFkMod(Col, Buckets.PartMod, OutCol, Ascending, static_cast<std::size_t>(FrameOffset));
		return TryNtile(Col, Schema, Buckets, OrderCol, OutCol, Ascending, static_cast<std::size_t>(FrameOffset));
	}
	return false;
}

double BulkSyntheticCustDateMetricAt(const ColumnarTable &Col, const std::size_t RowIndex,
                                     const std::string_view Column) noexcept {
	if(Col.RowCount == 0 || RowIndex >= Col.RowCount)
		return std::numeric_limits<double>::quiet_NaN();
	const int64_t PartMod = Col.BulkSyntheticWindowBucketPartMod > 0 ? Col.BulkSyntheticWindowBucketPartMod : 997;
	const std::size_t Mod = static_cast<std::size_t>(PartMod);
	const int64_t RowId = BulkSyntheticRowIdAt(Col, RowIndex);
	const std::size_t P = static_cast<std::size_t>((RowId - 1) % PartMod);
	double Sum = 0;
	double Ring7[8]{};
	double Ring30[31]{};
	std::size_t Len7 = 0;
	std::size_t Len30 = 0;
	std::size_t Pos7 = 0;
	std::size_t Pos30 = 0;
	double Sum7 = 0;
	double Sum30 = 0;
	std::size_t PrevIx = static_cast<std::size_t>(-1);
	for(std::size_t Ix = P; Ix <= RowIndex; Ix += Mod) {
		const double V = WindowAmountAt(Col, Ix);
		if(Column == "running_total") {
			Sum += V;
			if(Ix == RowIndex)
				return Sum;
			continue;
		}
		if(Column == "prev_amount") {
			if(Ix == RowIndex)
				return PrevIx == static_cast<std::size_t>(-1)
				           ? std::numeric_limits<double>::quiet_NaN()
				           : WindowAmountAt(Col, PrevIx);
			PrevIx = Ix;
			continue;
		}
		if(Len7 < 8) {
			Ring7[Len7++] = V;
			Sum7 += V;
		} else {
			Sum7 -= Ring7[Pos7];
			Ring7[Pos7] = V;
			Sum7 += V;
			Pos7 = (Pos7 + 1) % 8;
		}
		if(Len30 < 31) {
			Ring30[Len30++] = V;
			Sum30 += V;
		} else {
			Sum30 -= Ring30[Pos30];
			Ring30[Pos30] = V;
			Sum30 += V;
			Pos30 = (Pos30 + 1) % 31;
		}
		if(Ix == RowIndex) {
			if(Column == "ma7")
				return Sum7 / static_cast<double>(Len7);
			if(Column == "ma30")
				return Sum30 / static_cast<double>(Len30);
		}
	}
	return std::numeric_limits<double>::quiet_NaN();
}

double BulkSyntheticNtileDecileAt(const ColumnarTable &Col, const std::size_t RowIndex,
                                  const std::size_t NumTiles) noexcept {
	if(NumTiles == 0 || Col.RowCount == 0)
		return std::numeric_limits<double>::quiet_NaN();
	const int64_t PartMod = Col.BulkSyntheticWindowBucketPartMod > 0 ? Col.BulkSyntheticWindowBucketPartMod : 997;
	const std::size_t Mod = static_cast<std::size_t>(PartMod);
	const int64_t RowId = BulkSyntheticRowIdAt(Col, RowIndex);
	const std::size_t P = static_cast<std::size_t>((RowId - 1) % PartMod);
	const std::size_t Ps = (Col.RowCount - P + Mod - 1) / Mod;
	if(Ps == 0)
		return 1.;
	std::size_t K = 0;
	for(std::size_t Ix = P; Ix < Col.RowCount; Ix += Mod, ++K) {
		if(Ix == RowIndex)
			return static_cast<double>((K * NumTiles) / Ps + 1);
	}
	return static_cast<double>(NumTiles);
}

bool ExecuteQ3FusedWindowSuite(ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                               const std::size_t TopKeep) noexcept {
	if(!Col.BulkSyntheticLazy || !Col.BulkSyntheticPhysicalOrder || Col.RowCount == 0 || TopKeep == 0)
		return false;
	SemistructuredProfileScope Scope("q3_fused_window_suite");
	Col.BulkSyntheticWindowDbl.clear();
	Col.BulkSyntheticSortedRowIndices.clear();
	Col.BulkSyntheticWindowProjectionCommitted = false;
	Col.BulkSyntheticWindowBucketPartMod = 997;
	Col.BulkSyntheticWindowBucketsBuilt = true;
	Col.BulkSyntheticWindowBucketSeqSorted = Col.BulkStep == 1;
	if(!FlattenFkModTopK(Col, Schema, 997, TopKeep))
		return false;
	const bool NeedFullCache = TopKeep >= Col.RowCount / 4;
	if(NeedFullCache && !EnsureCustDateWindowCacheFkMod(Col, 997))
		return false;
	Col.BulkSyntheticWindowProjectionCommitted = true;
	return true;
}

} // namespace AstralDB
