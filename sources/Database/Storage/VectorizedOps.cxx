#include <Database/Storage/VectorizedOps.hxx>

#include <DS/SimdHash.hxx>

#include <algorithm>
#include <cstdlib>

namespace AstralDB {

std::size_t VectorBatchSizeFromEnv() noexcept {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
	const char *V = std::getenv("ASTRALDB_VEC_BATCH_SIZE");
#pragma warning(pop)
#else
	const char *V = std::getenv("ASTRALDB_VEC_BATCH_SIZE");
#endif
	if(V == nullptr || V[0] == '\0')
		return kDefaultVecBatchSize;
	char *End = nullptr;
	const unsigned long N = std::strtoul(V, &End, 10);
	if(End == V || N == 0 || N > 65536)
		return kDefaultVecBatchSize;
	return static_cast<std::size_t>(N);
}

VectorizedFilter::VectorizedFilter(const int64_t *Values, std::size_t Count, FilterCompareOp Op, int64_t Literal)
    : Values_(Values), Count_(Count), Op_(Op), Literal_(Literal) {}

void VectorizedFilter::ExecuteMask(std::vector<uint8_t> &OutMask) const {
	OutMask.assign(Count_, 0);
	if(Count_ == 0)
		return;
	std::vector<std::size_t> Hit;
	const std::size_t Batch = VectorBatchSizeFromEnv();
	std::size_t Pos = 0;
	while(Pos < Count_) {
		const std::size_t End = std::min(Pos + Batch, Count_);
		const std::size_t Len = End - Pos;
		Hit.clear();
		FilterI64Simd(Values_ + Pos, Len, Op_, Literal_, Hit);
		for(const std::size_t H : Hit)
			OutMask[Pos + H] = 1;
		Pos = End;
	}
}

void VectorizedFilter::ExecuteIndices(std::vector<std::size_t> &OutIndices) const {
	OutIndices.clear();
	if(Count_ == 0)
		return;
	const std::size_t Batch = VectorBatchSizeFromEnv();
	std::size_t Pos = 0;
	while(Pos < Count_) {
		const std::size_t End = std::min(Pos + Batch, Count_);
		const std::size_t Len = End - Pos;
		std::vector<std::size_t> Hit;
		FilterI64Simd(Values_ + Pos, Len, Op_, Literal_, Hit);
		for(const std::size_t H : Hit)
			OutIndices.push_back(Pos + H);
		Pos = End;
	}
}

VectorizedProject::VectorizedProject(std::vector<std::size_t> ColumnIndices)
    : ColumnIndices_(std::move(ColumnIndices)) {}

bool VectorizedProject::Execute(VectorBatch &In, VectorBatch &Out) {
	if(In.Columns.empty() || ColumnIndices_.empty())
		return false;
	Out.Columns.clear();
	Out.Columns.reserve(ColumnIndices_.size());
	Out.Size = In.Size;
	for(const std::size_t Ci : ColumnIndices_) {
		if(Ci >= In.Columns.size())
			return false;
		auto Copy = std::make_unique<ColumnVector>();
		*Copy = *In.Columns[Ci];
		Out.Columns.push_back(std::move(Copy));
	}
	return true;
}

VectorizedHashProbe::VectorizedHashProbe(const int64_t *ProbeKeys, std::size_t ProbeCount,
                                         const std::vector<std::vector<std::size_t>> &BuildBuckets,
                                         std::size_t BuildCount)
    : ProbeKeys_(ProbeKeys), ProbeCount_(ProbeCount), BuildBuckets_(&BuildBuckets), BuildCount_(BuildCount) {
	(void)BuildCount_;
}

void VectorizedHashProbe::Execute(HashProbeResult &Out) const {
	Out.ProbeIndices.clear();
	Out.BuildIndices.clear();
	if(ProbeCount_ == 0 || BuildBuckets_ == nullptr)
		return;
	const std::size_t Batch = VectorBatchSizeFromEnv();
	std::size_t Pos = 0;
	while(Pos < ProbeCount_) {
		const std::size_t End = std::min(Pos + Batch, ProbeCount_);
		for(std::size_t Pi = Pos; Pi < End; ++Pi) {
			const int64_t Key = ProbeKeys_[Pi];
			if(Key < 0 || static_cast<std::size_t>(Key) >= BuildBuckets_->size())
				continue;
			const auto &Hits = (*BuildBuckets_)[static_cast<std::size_t>(Key)];
			for(const std::size_t Bi : Hits) {
				Out.ProbeIndices.push_back(Pi);
				Out.BuildIndices.push_back(Bi);
			}
		}
		Pos = End;
	}
}

void BuildI64ColumnBatch(const std::vector<std::string> &Cells, const std::vector<std::size_t> &RowIndices,
                         std::size_t Begin, std::size_t End, ColumnVector &Out) {
	Out.Kind = ColumnVectorKind::I64;
	Out.I64.clear();
	Out.RowIndices.clear();
	Out.I64.reserve(End - Begin);
	Out.RowIndices.reserve(End - Begin);
	for(std::size_t J = Begin; J < End && J < RowIndices.size(); ++J) {
		const std::size_t Ri = RowIndices[J];
		if(Ri >= Cells.size())
			continue;
		char *EndPtr = nullptr;
		const long long V = std::strtoll(Cells[Ri].c_str(), &EndPtr, 10);
		if(EndPtr == Cells[Ri].c_str() || *EndPtr != '\0')
			continue;
		Out.I64.push_back(static_cast<int64_t>(V));
		Out.RowIndices.push_back(Ri);
	}
	Out.Size = Out.I64.size();
}

} // namespace AstralDB
