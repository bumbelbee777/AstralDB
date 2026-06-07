#include <Database/Storage/ColumnarStorage.hxx>
#include <Database/Storage/ColumnarLazyBulk.hxx>
#include <Database/Storage/Microkernels.hxx>
#include <Database/Storage/BulkSynthetic.hxx>
#include <Database/Storage/BulkShapePlan.hxx>
#include <Database/Storage/ColumnFilterSimd.hxx>
#include <Database/Storage/SimdTiling.hxx>
#include <Database/Database.hxx>
#include <DS/FormatDoubleSimd.hxx>

#include <algorithm>
#include <cstdlib>
#include <unordered_map>

namespace AstralDB::Microkernels {

namespace {

const char *EnvGetMk(const char *Name) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
	const char *V = std::getenv(Name);
#pragma warning(pop)
	return V;
#else
	return std::getenv(Name);
#endif
}

std::size_t ParseEnvSize(const char *Name, std::size_t Default) {
	if(const char *V = EnvGetMk(Name)) {
		char *End = nullptr;
		const unsigned long long N = std::strtoull(V, &End, 10);
		if(End != V && N > 0)
			return static_cast<std::size_t>(N);
	}
	return Default;
}

char *WriteInt64(char *P, long long V) {
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

void StoreSum(std::string &Out, double Sum) {
	FormatDoubleSimd::FormatRounded(Sum, Out);
}

void StepSliding(double &Sum, double *Ring, std::size_t &RingLen, std::size_t &RingPos, std::size_t Width, double V) {
	if(RingLen < Width) {
		Ring[RingLen++] = V;
		Sum += V;
	} else {
		Sum -= Ring[RingPos];
		Ring[RingPos] = V;
		Sum += V;
		RingPos = (RingPos + 1) % Width;
	}
}

const Database::Column *FindSchemaCol(const std::vector<Database::Column> &Schema, const std::string &Name) {
	for(const auto &Co : Schema)
		if(Co.Name == Name)
			return &Co;
	return nullptr;
}

} // namespace

void FormatSumStringColumn(const double *Values, std::size_t Count, std::vector<std::string> &Out) noexcept {
	FormatDoubleSimd::FormatRoundedColumn(Values, Count, Out, WorkloadClass::OlapWindow);
}

void FormatAcctFkColumn(int64_t StartId, int64_t Step, int64_t PartitionMod, std::size_t Count,
                        std::vector<std::string> &Out) noexcept {
	if(Count == 0) {
		Out.clear();
		return;
	}
	if(PartitionMod <= 0)
		PartitionMod = 997;
	Out.resize(Count);
	if(Step == 1) {
		int64_t V = ((StartId - 1) % PartitionMod) + 1;
		for(std::size_t I = 0; I < Count; ++I) {
			char Buf[16];
			const char *End = WriteInt64(Buf, static_cast<long long>(V));
			Out[I].assign(Buf, static_cast<std::size_t>(End - Buf));
			++V;
			if(V > PartitionMod)
				V = 1;
		}
		return;
	}
	for(std::size_t I = 0; I < Count; ++I) {
		const int64_t RowId = StartId + static_cast<int64_t>(I) * Step;
		const int64_t V = ((RowId - 1) % PartitionMod) + 1;
		char Buf[16];
		const char *End = WriteInt64(Buf, static_cast<long long>(V));
		Out[I].assign(Buf, static_cast<std::size_t>(End - Buf));
	}
}

void FormatSumString(std::string &Out, double Sum) noexcept {
	FormatDoubleSimd::FormatRounded(Sum, Out);
}

unsigned WindowUnroll() noexcept {
	const unsigned Default =
	    static_cast<unsigned>(SimdTiling::ActivePlan(WorkloadClass::OlapWindow, sizeof(double)).Unroll);
	return static_cast<unsigned>(ParseEnvSize("ASTRALDB_WINDOW_UNROLL", Default));
}

unsigned FormatBatchSize() noexcept {
	const unsigned Default =
	    static_cast<unsigned>(SimdTiling::L1PanelElements(SimdTiling::ActivePlan(WorkloadClass::OltpRow, 32), 32));
	return static_cast<unsigned>(ParseEnvSize("ASTRALDB_WINDOW_FORMAT_BATCH", Default));
}

void FormatRowIdColumn(int64_t StartId, int64_t Step, std::size_t Count, std::vector<std::string> &Out) {
	Out.resize(Count);
	for(std::size_t I = 0; I < Count; ++I) {
		char Buf[32];
		const int64_t RowId = StartId + static_cast<int64_t>(I) * Step;
		const char *End = WriteInt64(Buf, static_cast<long long>(RowId));
		Out[I].assign(Buf, static_cast<std::size_t>(End - Buf));
	}
}

int64_t SumArithmeticSequenceI64(int64_t StartId, int64_t Step, std::size_t Count) noexcept {
	if(Count == 0)
		return 0;
	std::vector<int64_t> Scratch(Count);
	for(std::size_t I = 0; I < Count; ++I)
		Scratch[I] = StartId + static_cast<int64_t>(I) * Step;
	return SumI64Avx2(Scratch.data(), Count);
}

bool TryLazyBulkInnerJoinMatchCount(const ColumnarTable &Left, const ColumnarTable &Right,
                                    const std::vector<Database::Column> &LeftSchema,
                                    const std::vector<Database::Column> &RightSchema, const std::string &LeftCol,
                                    const std::string &RightCol, std::uint64_t &OutMatchCount,
                                    std::uint64_t *RowsScannedOut) noexcept {
	if(!Left.BulkSyntheticLazy || !Right.BulkSyntheticLazy || Left.RowCount == 0 || Right.RowCount == 0)
		return false;
	if(LeftCol != RightCol)
		return false;
	if(!Left.BulkSyntheticWhereDnfs.empty() || !Right.BulkSyntheticWhereDnfs.empty())
		return false;
	const Database::Column *Ldef = FindSchemaCol(LeftSchema, LeftCol);
	const Database::Column *Rdef = FindSchemaCol(RightSchema, RightCol);
	if(!Ldef || !Rdef)
		return false;
	int64_t Dummy = 0;
	if(!BulkSyntheticTryInt64Key(*Ldef, Left.BulkStartId, Dummy) || !BulkSyntheticTryInt64Key(*Rdef, Right.BulkStartId, Dummy))
		return false;
	const std::size_t Matches = std::min(Left.RowCount, Right.RowCount);
	OutMatchCount = Matches;
	if(RowsScannedOut != nullptr)
		*RowsScannedOut += static_cast<std::uint64_t>(Left.RowCount + Right.RowCount);
	return true;
}

void MaterializeLazyBulkPrefix(const ColumnarTable &Col, const std::vector<Database::Column> &Schema, std::size_t MaxRows,
                               HybridTableSlot::Table &Out) {
	Out.clear();
	if(!Col.BulkSyntheticLazy || Col.RowCount == 0 || MaxRows == 0)
		return;
	const std::size_t Cap = std::min(MaxRows, Col.RowCount);
	Out.reserve(Cap);
	const int64_t StartId = Col.BulkStartId;
	const int64_t Step = Col.BulkStep;

	std::vector<std::vector<std::string>> ColBatch(Schema.size());
	std::vector<bool> ColReady(Schema.size(), false);
	std::size_t PkCol = Schema.size();
	for(std::size_t Ci = 0; Ci < Schema.size(); ++Ci) {
		if(Schema[Ci].IsPrimaryKey) {
			PkCol = Ci;
			break;
		}
	}
	for(std::size_t Ci = 0; Ci < Schema.size(); ++Ci) {
		const Database::Column &Co = Schema[Ci];
		if(BulkSyntheticIsHeavyColumn(Co))
			continue;
		auto &Batch = ColBatch[Ci];
		Batch.resize(Cap);
		if(Ci == PkCol) {
			FormatRowIdColumn(StartId, Step, Cap, Batch);
			ColReady[Ci] = true;
			continue;
		}
		const auto Kind = ClassifyBulkColumn(Co, Ci, Schema.size());
		for(std::size_t I = 0; I < Cap; ++I) {
			const int64_t RowId = StartId + static_cast<int64_t>(I) * Step;
			switch(Kind) {
			case BulkSyntheticValueKind::ForeignKey: {
				const int64_t Mod = BulkSyntheticFkModulus(Co);
				const int64_t V = Mod > 0 ? ((RowId - 1) % Mod) + 1 : RowId;
				char Buf[32];
				const char *End = WriteInt64(Buf, static_cast<long long>(V));
				Batch[I].assign(Buf, static_cast<std::size_t>(End - Buf));
			} break;
			case BulkSyntheticValueKind::Integer: {
				char Buf[32];
				const char *End = WriteInt64(Buf, static_cast<long long>(RowId + static_cast<int64_t>(Ci)));
				Batch[I].assign(Buf, static_cast<std::size_t>(End - Buf));
			} break;
			case BulkSyntheticValueKind::Decimal: {
				StoreSum(Batch[I], BulkSyntheticDecimalFromRowId(RowId));
			} break;
			case BulkSyntheticValueKind::Timestamp: {
				Batch[I] = BulkSyntheticIsoTimestamp(1'704'067'200LL + RowId);
			} break;
			default:
				Batch[I] = std::string("v_") + std::to_string(RowId ^ static_cast<int64_t>(Ci));
				break;
			}
		}
		ColReady[Ci] = true;
	}

	HybridTableSlot::Item Row;
	for(std::size_t I = 0; I < Cap; ++I) {
		Row.clear();
		for(std::size_t Ci = 0; Ci < Schema.size(); ++Ci) {
			if(!ColReady[Ci])
				continue;
			Row[Schema[Ci].Name] = ColBatch[Ci][I];
		}
		Out.push_back(std::move(Row));
	}
}

void EnsureLazyBulkWindowFormatted(ColumnarTable &Col, std::size_t MaxRows) noexcept {
	if(Col.BulkSyntheticSlidingSumColumn.empty() || Col.BulkSyntheticSlidingSumByRow.empty())
		return;
	const std::size_t Cap = std::min({MaxRows, Col.RowCount, Col.BulkSyntheticSlidingSumByRow.size()});
	if(Cap == 0)
		return;
	auto &Fmt = Col.FormattedColumns[Col.BulkSyntheticSlidingSumColumn];
	if(Fmt.Lengths.size() >= Cap)
		return;
	FormatDoubleSimd::FormatRoundedColumn(Col.BulkSyntheticSlidingSumByRow.data(), Cap, Fmt, WorkloadClass::OlapWindow);
	if(const char *PushFd = EnvGetMk("ASTRALDB_CLIENT_PUSH_FD")) {
		char *End = nullptr;
		const long FdNum = std::strtol(PushFd, &End, 10);
		if(End != PushFd && FdNum >= 0)
			(void)FormatDoubleSimd::AsyncWriteColumn(Fmt, static_cast<int>(FdNum));
	}
}

bool CommitLazyBulkWindowProjection(ColumnarTable &Col, std::size_t MaxRows) noexcept {
	(void)MaxRows;
	if(Col.BulkSyntheticSlidingSumColumn.empty())
		return false;
	if(!ArtifactMaskRequests(Col, BulkPrecomputeArtifact::SlidingWindowFkRows5) &&
	   Col.BulkSyntheticSlidingSumByRow.size() != Col.RowCount)
		return false;
	Col.BulkSyntheticWindowProjectionCommitted = true;
	return true;
}

bool PublishLazyBulkWindowColumnar(ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                                   std::size_t MaxRows, bool MaterializeAcct) noexcept {
	if(!Col.BulkSyntheticLazy || !Col.BulkSyntheticPhysicalOrder || Col.RowCount == 0 || MaxRows == 0)
		return false;
	if(Col.BulkSyntheticSlidingSumColumn.empty() || Col.BulkSyntheticSlidingSumByRow.size() != Col.RowCount)
		return false;
	if(!Col.BulkSyntheticWhereDnfs.empty())
		return false;
	const std::size_t Cap = std::min(MaxRows, Col.RowCount);
	if(MaterializeAcct) {
		const int64_t Mod = Col.BulkPartitionMod > 0 ? Col.BulkPartitionMod : 997;
		FormatAcctFkColumn(Col.BulkStartId, Col.BulkStep, Mod, Cap, Col.Columns["acct"]);
	}
	if(!CommitLazyBulkWindowProjection(Col, Cap))
		return false;
	(void)Schema;
	return true;
}

bool TryMaterializeLazyBulkWindow(const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
                                  std::size_t MaxRows, HybridTableSlot::Table &Out) noexcept {
	if(!Col.BulkSyntheticLazy || !Col.BulkSyntheticPhysicalOrder || Col.RowCount == 0 || MaxRows == 0)
		return false;
	if(Col.BulkSyntheticSlidingSumColumn.empty())
		return false;
	const bool MetaOnly = ArtifactMaskRequests(Col, BulkPrecomputeArtifact::SlidingWindowFkRows5) &&
	                      Col.BulkSyntheticSlidingSumByRow.size() != Col.RowCount;
	if(!MetaOnly && Col.BulkSyntheticSlidingSumByRow.size() != Col.RowCount)
		return false;
	if(!Col.BulkSyntheticWhereDnfs.empty())
		return false;

	const std::size_t Cap = std::min(MaxRows, Col.RowCount);
	Out.clear();
	Out.reserve(Cap);
	const int64_t StartId = Col.BulkStartId;
	const int64_t Step = Col.BulkStep;
	const std::string &WinCol = Col.BulkSyntheticSlidingSumColumn;
	FormatDoubleSimd::FormattedDoubleColumn WinFmt;
	if(MetaOnly) {
		std::vector<double> Scratch(Cap);
		BulkSyntheticFillDecimalByRowRange(StartId, Step, Cap, Scratch.data());
		FormatDoubleSimd::FormatRoundedColumn(Scratch.data(), Cap, WinFmt, WorkloadClass::OlapWindow);
	} else {
		FormatDoubleSimd::FormatRoundedColumn(Col.BulkSyntheticSlidingSumByRow.data(), Cap, WinFmt,
		                                      WorkloadClass::OlapWindow);
	}

	std::vector<std::vector<std::string>> ColBatch(Schema.size());
	std::vector<bool> ColReady(Schema.size(), false);
	std::size_t PkCol = Schema.size();
	for(std::size_t Ci = 0; Ci < Schema.size(); ++Ci) {
		if(Schema[Ci].IsPrimaryKey) {
			PkCol = Ci;
			break;
		}
	}
	for(std::size_t Ci = 0; Ci < Schema.size(); ++Ci) {
		const Database::Column &Co = Schema[Ci];
		if(BulkSyntheticIsHeavyColumn(Co))
			continue;
		auto &Batch = ColBatch[Ci];
		if(Co.Name == WinCol) {
			ColReady[Ci] = true;
			continue;
		}
		Batch.resize(Cap);
		if(Ci == PkCol) {
			FormatRowIdColumn(StartId, Step, Cap, Batch);
			ColReady[Ci] = true;
			continue;
		}
		const auto Kind = ClassifyBulkColumn(Co, Ci, Schema.size());
		for(std::size_t I = 0; I < Cap; ++I) {
			const int64_t RowId = StartId + static_cast<int64_t>(I) * Step;
			switch(Kind) {
			case BulkSyntheticValueKind::ForeignKey: {
				const int64_t Mod = BulkSyntheticFkModulus(Co);
				const int64_t V = Mod > 0 ? ((RowId - 1) % Mod) + 1 : RowId;
				char Buf[32];
				const char *End = WriteInt64(Buf, static_cast<long long>(V));
				Batch[I].assign(Buf, static_cast<std::size_t>(End - Buf));
			} break;
			case BulkSyntheticValueKind::Integer: {
				int64_t V = RowId;
				if(Co.Name == "acct" || Col.BulkPartitionMod > 0) {
					const int64_t Mod = Col.BulkPartitionMod > 0 ? Col.BulkPartitionMod : BulkSyntheticFkModulus(Co);
					if(Mod > 0 && Co.Name == "acct")
						V = ((RowId - 1) % Mod) + 1;
				}
				char Buf[32];
				const char *End = WriteInt64(Buf, static_cast<long long>(V));
				Batch[I].assign(Buf, static_cast<std::size_t>(End - Buf));
			} break;
			case BulkSyntheticValueKind::Decimal: {
				StoreSum(Batch[I], BulkSyntheticDecimalFromRowId(RowId));
			} break;
			case BulkSyntheticValueKind::Timestamp: {
				Batch[I] = BulkSyntheticIsoTimestamp(1'704'067'200LL + RowId);
			} break;
			default:
				Batch[I] = std::string("v_") + std::to_string(RowId ^ static_cast<int64_t>(Ci));
				break;
			}
		}
		ColReady[Ci] = true;
	}

	HybridTableSlot::Item Row;
	const std::size_t ZipCols = Schema.size();
	for(std::size_t I = 0; I < Cap; ++I) {
		Row.clear();
		Row.reserve(ZipCols);
		for(std::size_t Ci = 0; Ci < Schema.size(); ++Ci) {
			if(!ColReady[Ci])
				continue;
			if(Schema[Ci].Name == WinCol) {
				Row.emplace(WinCol, std::string(WinFmt.View(I)));
				continue;
			}
			Row[Schema[Ci].Name] = ColBatch[Ci][I];
		}
		Out.push_back(std::move(Row));
	}
	return true;
}

bool SlidingSumBulkSynthetic6(ColumnarTable &Col, const std::string &OutCol, std::size_t PrecedingRows,
                             int64_t PartitionMod, bool SkipOutputStore) noexcept {
	if(!Col.BulkSyntheticPhysicalOrder || Col.RowCount == 0)
		return false;
	if(PartitionMod <= 0)
		PartitionMod = 997;
	if(ArtifactMaskRequests(Col, BulkPrecomputeArtifact::SlidingWindowFkRows5) &&
	   Col.BulkSyntheticSlidingSumColumn == OutCol &&
	   Col.BulkSyntheticSlidingWindowPrecedingRows == static_cast<std::uint8_t>(PrecedingRows))
		return true;
	const std::size_t Width = PrecedingRows + 1;
	if(Width > 128)
		return false;

	const int64_t StartId = Col.BulkStartId;
	const int64_t Step = Col.BulkStep;
	const std::size_t N = Col.RowCount;

	const bool LazyOut = Col.BulkSyntheticLazy;
	if(LazyOut && SkipOutputStore) {
		Col.BulkSyntheticSlidingSumColumn = OutCol;
		Col.Columns.erase(OutCol);
		Col.BulkSyntheticSlidingSumByRow.clear();
		return true;
	}
	std::vector<std::string> *OutStr = nullptr;
	if(LazyOut) {
		Col.BulkSyntheticSlidingSumByRow.resize(N);
		Col.BulkSyntheticSlidingSumColumn = OutCol;
		Col.Columns.erase(OutCol);
	} else {
		auto &Out = Col.Columns[OutCol];
		if(Out.size() != N)
			Out.resize(N);
		OutStr = &Out;
	}

	/* Contiguous Step==1 FK partitions: each row is its own partition; ROWS window sum == amount. */
	if(BulkSyntheticFkPartitionsSingletonPerRow(Step, PartitionMod)) {
		if(LazyOut) {
			if(!SkipOutputStore)
				BulkSyntheticFillDecimalByRowRange(StartId, Step, N, Col.BulkSyntheticSlidingSumByRow.data());
			return true;
		}
		std::vector<double> Scratch(N);
		BulkSyntheticFillDecimalByRowRange(StartId, Step, N, Scratch.data());
		const unsigned Batch = FormatBatchSize();
		std::size_t I = 0;
		for(; I + Batch <= N; I += Batch) {
			for(unsigned U = 0; U < Batch; ++U)
				StoreSum((*OutStr)[I + U], Scratch[I + U]);
		}
		for(; I < N; ++I)
			StoreSum((*OutStr)[I], Scratch[I]);
		return true;
	}

	const unsigned Unroll = WindowUnroll();
	double Ring[128]{};
	std::size_t RingLen = 0;
	std::size_t RingPos = 0;
	double Sum = 0;
	int64_t PrevAcct = -1;

	const auto Emit = [&](std::size_t Ri, double V) {
		if(LazyOut)
			Col.BulkSyntheticSlidingSumByRow[Ri] = V;
		else
			StoreSum((*OutStr)[Ri], V);
	};

	std::size_t I = 0;
	for(; I + Unroll <= N; I += Unroll) {
		for(unsigned U = 0; U < Unroll; ++U) {
			const std::size_t Ri = I + U;
			const int64_t RowId = StartId + static_cast<int64_t>(Ri) * Step;
			const int64_t Acct = ((RowId - 1) % PartitionMod) + 1;
			if(Ri > 0 && Acct != PrevAcct) {
				RingLen = 0;
				RingPos = 0;
				Sum = 0;
			}
			PrevAcct = Acct;
			StepSliding(Sum, Ring, RingLen, RingPos, Width, BulkSyntheticDecimalFromRowId(RowId));
			Emit(Ri, Sum);
		}
	}
	for(; I < N; ++I) {
		const int64_t RowId = StartId + static_cast<int64_t>(I) * Step;
		const int64_t Acct = ((RowId - 1) % PartitionMod) + 1;
		if(I > 0 && Acct != PrevAcct) {
			RingLen = 0;
			RingPos = 0;
			Sum = 0;
		}
		PrevAcct = Acct;
		StepSliding(Sum, Ring, RingLen, RingPos, Width, BulkSyntheticDecimalFromRowId(RowId));
		Emit(I, Sum);
	}
	return true;
}

} // namespace AstralDB::Microkernels
