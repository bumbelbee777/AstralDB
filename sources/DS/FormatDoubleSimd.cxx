#include <DS/FormatDoubleSimd.hxx>

#include <Database/Storage/SimdTiling.hxx>
#include <IO/Job.hxx>
#include <IO/SIMD.hxx>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#if defined(__AVX512F__)
#include <immintrin.h>
#elif defined(__AVX2__)
#include <immintrin.h>
#endif
#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace AstralDB::FormatDoubleSimd {

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

std::size_t EnvSize(const char *Name, std::size_t Default) {
	if(const char *V = EnvGet(Name)) {
		char *End = nullptr;
		const unsigned long long N = std::strtoull(V, &End, 10);
		if(End != V && N > 0)
			return static_cast<std::size_t>(N);
	}
	return Default;
}

bool EnvParallelSemistructured() noexcept {
	const char *V = EnvGet("ASTRALDB_SEMISTRUCTURED_PARALLEL");
	return V == nullptr || (V[0] != '0' && V[0] != 'n' && V[0] != 'N');
}

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

long long RoundHalfAway(double V) noexcept {
	return static_cast<long long>(V >= 0. ? V + 0.5 : V - 0.5);
}

void FormatOneRounded(double V, char *Buf, std::uint16_t &Len) noexcept {
	const long long R = RoundHalfAway(V);
	char *End = WriteI64(Buf, R);
	Len = static_cast<std::uint16_t>(End - Buf);
}

void FormatPanelRounded(const double *Values, std::size_t Begin, std::size_t End, std::vector<char> &Chars,
                        std::vector<std::uint32_t> &Offsets, std::vector<std::uint16_t> &Lengths,
                        const SimdTileSession &Session) noexcept {
	std::size_t I = Begin;
#if defined(__AVX512F__)
	for(; I + 8 <= End; I += 8) {
		SimdTiling::PrefetchStreamAhead(Session, Values, I, sizeof(double), End - Begin);
		const __m512d V = _mm512_loadu_pd(Values + I);
		alignas(64) double Vd[8];
		_mm512_store_pd(Vd, V);
		for(unsigned U = 0; U < 8; ++U) {
			char StackBuf[32];
			std::uint16_t Len = 0;
			FormatOneRounded(Vd[U], StackBuf, Len);
			const std::uint32_t Off = static_cast<std::uint32_t>(Chars.size());
			Offsets.push_back(Off);
			Lengths.push_back(Len);
			Chars.insert(Chars.end(), StackBuf, StackBuf + Len);
		}
	}
#elif defined(__AVX2__)
	for(; I + 4 <= End; I += 4) {
		SimdTiling::PrefetchStreamAhead(Session, Values, I, sizeof(double), End - Begin);
		const __m256d V = _mm256_loadu_pd(Values + I);
		alignas(32) double Vd[4];
		_mm256_store_pd(Vd, V);
		for(unsigned U = 0; U < 4; ++U) {
			char StackBuf[32];
			std::uint16_t Len = 0;
			FormatOneRounded(Vd[U], StackBuf, Len);
			const std::uint32_t Off = static_cast<std::uint32_t>(Chars.size());
			Offsets.push_back(Off);
			Lengths.push_back(Len);
			Chars.insert(Chars.end(), StackBuf, StackBuf + Len);
		}
	}
#elif defined(__ARM_NEON) || defined(__aarch64__)
	for(; I + 2 <= End; I += 2) {
		SimdTiling::PrefetchStreamAhead(Session, Values, I, sizeof(double), End - Begin);
		const float64x2_t V = vld1q_f64(Values + I);
		alignas(16) double Vd[2];
		vst1q_f64(Vd, V);
		for(unsigned U = 0; U < 2; ++U) {
			char StackBuf[32];
			std::uint16_t Len = 0;
			FormatOneRounded(Vd[U], StackBuf, Len);
			const std::uint32_t Off = static_cast<std::uint32_t>(Chars.size());
			Offsets.push_back(Off);
			Lengths.push_back(Len);
			Chars.insert(Chars.end(), StackBuf, StackBuf + Len);
		}
	}
#endif
	for(; I < End; ++I) {
		SimdTiling::PrefetchStreamAhead(Session, Values, I, sizeof(double), End - Begin);
		char StackBuf[32];
		std::uint16_t Len = 0;
		FormatOneRounded(Values[I], StackBuf, Len);
		const std::uint32_t Off = static_cast<std::uint32_t>(Chars.size());
		Offsets.push_back(Off);
		Lengths.push_back(Len);
		Chars.insert(Chars.end(), StackBuf, StackBuf + Len);
	}
}

void MergeFormattedColumn(FormattedDoubleColumn &Dst, FormattedDoubleColumn &&Src) {
	if(Src.Lengths.empty())
		return;
	const std::uint32_t Base = static_cast<std::uint32_t>(Dst.Chars.size());
	Dst.Chars.insert(Dst.Chars.end(), Src.Chars.begin(), Src.Chars.end());
	for(std::size_t I = 0; I < Src.Lengths.size(); ++I) {
		Dst.Offsets.push_back(Base + Src.Offsets[I]);
		Dst.Lengths.push_back(Src.Lengths[I]);
	}
}

std::size_t AsyncRowThreshold() noexcept {
	return EnvSize("ASTRALDB_FORMAT_DOUBLE_ASYNC_ROWS", 1u << 20);
}

} // namespace

void FormattedDoubleColumn::Clear() noexcept {
	Chars.clear();
	Offsets.clear();
	Lengths.clear();
}

void FormattedDoubleColumn::Reserve(std::size_t RowCount, std::size_t AvgCharsPerRow) noexcept {
	Chars.reserve(RowCount * AvgCharsPerRow);
	Offsets.reserve(RowCount + 1);
	Lengths.reserve(RowCount);
}

std::string_view FormattedDoubleColumn::View(std::size_t Index) const noexcept {
	if(Index >= Lengths.size())
		return {};
	const std::uint32_t Off = Offsets[Index];
	return std::string_view(Chars.data() + Off, Lengths[Index]);
}

void FormattedDoubleColumn::AssignToVector(std::vector<std::string> &Out) const {
	Out.resize(Lengths.size());
	for(std::size_t I = 0; I < Lengths.size(); ++I)
		Out[I].assign(Chars.data() + Offsets[I], Lengths[I]);
}

void FormatRounded(double Value, std::string &Out) noexcept {
	char Buf[32];
	std::uint16_t Len = 0;
	FormatOneRounded(Value, Buf, Len);
	Out.assign(Buf, Len);
}

void FormatRoundedColumn(const double *Values, std::size_t Count, FormattedDoubleColumn &Out,
                         WorkloadClass Workload) noexcept {
	Out.Clear();
	if(Count == 0 || Values == nullptr)
		return;
	Out.Reserve(Count);

	const unsigned Hw = std::max(1u, std::thread::hardware_concurrency());
	const TiledCachePlan Plan = SimdTiling::ActivePlan(Workload, sizeof(double));
	const std::size_t L2Block = SimdTiling::L2BlockElements(Plan, sizeof(double));
	if(Count >= AsyncRowThreshold() && Hw > 1 && L2Block > 0 && Count > L2Block) {
		std::vector<std::future<FormattedDoubleColumn>> Jobs;
		Jobs.reserve((Count + L2Block - 1) / L2Block);
		for(std::size_t Begin = 0; Begin < Count; Begin += L2Block) {
			const std::size_t Panel = std::min(L2Block, Count - Begin);
			Jobs.push_back(std::async(std::launch::async, [Values, Begin, Panel, Workload]() {
				FormattedDoubleColumn Part;
				FormatRoundedColumn(Values + Begin, Panel, Part, Workload);
				return Part;
			}));
		}
		for(auto &F : Jobs)
			MergeFormattedColumn(Out, F.get());
		Out.Offsets.push_back(static_cast<std::uint32_t>(Out.Chars.size()));
		return;
	}

	SimdTiling::ForL1Panels(
	    Count, sizeof(double), Workload,
	    [&](std::size_t Begin, std::size_t End, const TiledCachePlan &, SimdTileSession &Session) {
		    SimdTiling::PrefetchPanelAhead(Session, Values, Begin, (End - Begin) * sizeof(double));
		    FormatPanelRounded(Values, Begin, End, Out.Chars, Out.Offsets, Out.Lengths, Session);
	    },
	    []() {});
	Out.Offsets.push_back(static_cast<std::uint32_t>(Out.Chars.size()));
}

void FormatRoundedColumn(const double *Values, std::size_t Count, std::vector<std::string> &Out,
                         WorkloadClass Workload) noexcept {
	FormattedDoubleColumn Col;
	FormatRoundedColumn(Values, Count, Col, Workload);
	Col.AssignToVector(Out);
}

std::future<void> FormatRoundedColumnAsync(const double *Values, std::size_t Count, FormattedDoubleColumn &Out,
                                           WorkloadClass Workload) {
	return std::async(std::launch::async, [Values, Count, &Out, Workload]() {
		FormatRoundedColumn(Values, Count, Out, Workload);
	});
}

void FormatOneRankMilli(float V, char *Buf, std::uint16_t &Len) noexcept {
	int M = static_cast<int>(V * 1000.f + 0.5f);
	if(M < 0)
		M = 0;
	if(M > 999)
		M = 999;
	Buf[0] = '0';
	Buf[1] = '.';
	Buf[2] = static_cast<char>('0' + (M / 100) % 10);
	Buf[3] = static_cast<char>('0' + (M / 10) % 10);
	Buf[4] = static_cast<char>('0' + M % 10);
	Len = 5;
}

void FormatPanelRankF32(const float *Values, std::size_t Begin, std::size_t End, std::vector<char> &Chars,
                        std::vector<std::uint32_t> &Offsets, std::vector<std::uint16_t> &Lengths,
                        const SimdTileSession &Session) noexcept {
	std::size_t I = Begin;
#if defined(__AVX2__)
	for(; I + 8 <= End; I += 8) {
		SimdTiling::PrefetchStreamAhead(Session, Values, I, sizeof(float), End - Begin);
		alignas(32) float Lane[8];
		_mm256_storeu_ps(Lane, _mm256_loadu_ps(Values + I));
		for(unsigned U = 0; U < 8; ++U) {
			char StackBuf[8];
			std::uint16_t Len = 0;
			FormatOneRankMilli(Lane[U], StackBuf, Len);
			const std::uint32_t Off = static_cast<std::uint32_t>(Chars.size());
			Offsets.push_back(Off);
			Lengths.push_back(Len);
			Chars.insert(Chars.end(), StackBuf, StackBuf + Len);
		}
	}
#endif
	for(; I < End; ++I) {
		SimdTiling::PrefetchStreamAhead(Session, Values, I, sizeof(float), End - Begin);
		char StackBuf[8];
		std::uint16_t Len = 0;
		FormatOneRankMilli(Values[I], StackBuf, Len);
		const std::uint32_t Off = static_cast<std::uint32_t>(Chars.size());
		Offsets.push_back(Off);
		Lengths.push_back(Len);
		Chars.insert(Chars.end(), StackBuf, StackBuf + Len);
	}
}

void FormatRankF32Column(const float *Values, std::size_t Count, FormattedDoubleColumn &Out,
                         WorkloadClass Workload) noexcept {
	Out.Clear();
	if(Count == 0 || Values == nullptr)
		return;
	Out.Reserve(Count, 6);
	SimdTiling::ForL1Panels(
	    Count, sizeof(float), Workload,
	    [&](std::size_t Begin, std::size_t End, const TiledCachePlan &, SimdTileSession &Session) {
		    SimdTiling::PrefetchPanelAhead(Session, Values, Begin, (End - Begin) * sizeof(float));
		    FormatPanelRankF32(Values, Begin, End, Out.Chars, Out.Offsets, Out.Lengths, Session);
	    },
	    []() {});
	Out.Offsets.push_back(static_cast<std::uint32_t>(Out.Chars.size()));
}

void FormatRankF32GatherIndices(const float *Keys, const std::uint32_t *RowIndices, const std::size_t Count,
                                FormattedDoubleColumn &Out, WorkloadClass Workload) noexcept {
	Out.Clear();
	if(Count == 0 || Keys == nullptr || RowIndices == nullptr)
		return;
	Out.Reserve(Count, 6);
	Out.Chars.reserve(Count * 6);
	Out.Offsets.reserve(Count + 1);
	Out.Lengths.reserve(Count);
	std::size_t I = 0;
#if defined(__AVX2__)
	for(; I + 8 <= Count; I += 8) {
		alignas(32) float Lane[8];
		for(unsigned J = 0; J < 8; ++J)
			Lane[J] = Keys[RowIndices[I + J]];
		for(unsigned U = 0; U < 8; ++U) {
			char StackBuf[8];
			std::uint16_t Len = 0;
			FormatOneRankMilli(Lane[U], StackBuf, Len);
			const std::uint32_t Off = static_cast<std::uint32_t>(Out.Chars.size());
			Out.Offsets.push_back(Off);
			Out.Lengths.push_back(Len);
			Out.Chars.insert(Out.Chars.end(), StackBuf, StackBuf + Len);
		}
	}
#endif
	for(; I < Count; ++I) {
		char StackBuf[8];
		std::uint16_t Len = 0;
		FormatOneRankMilli(Keys[RowIndices[I]], StackBuf, Len);
		const std::uint32_t Off = static_cast<std::uint32_t>(Out.Chars.size());
		Out.Offsets.push_back(Off);
		Out.Lengths.push_back(Len);
		Out.Chars.insert(Out.Chars.end(), StackBuf, StackBuf + Len);
	}
	Out.Offsets.push_back(static_cast<std::uint32_t>(Out.Chars.size()));
	(void)Workload;
}

namespace {

void MaterializeMixedZipBand(const std::vector<std::string> &ColNames,
                             const std::vector<const std::vector<std::string> *> &StringCols,
                             const std::vector<const FormattedDoubleColumn *> &FormattedCols,
                             const std::size_t FormattedColBegin, const std::size_t RowBegin, const std::size_t RowEnd,
                             std::vector<std::unordered_map<std::string, std::string>> &OutRows) {
	const std::size_t Nstr = StringCols.size();
	const std::size_t Nfmt = FormattedCols.size();
	for(std::size_t Ri = RowBegin; Ri < RowEnd; ++Ri)
		OutRows[Ri].reserve(ColNames.size());
	for(std::size_t Ci = 0; Ci < Nstr; ++Ci) {
		const std::vector<std::string> *Col = StringCols[Ci];
		if(Col == nullptr || Col->empty())
			continue;
		const std::string &Name = ColNames[Ci];
		const std::size_t ColLen = Col->size();
		for(std::size_t Ri = RowBegin; Ri < RowEnd; ++Ri) {
			if(Ri < ColLen)
				OutRows[Ri].emplace(Name, (*Col)[Ri]);
		}
	}
	for(std::size_t Fi = 0; Fi < Nfmt; ++Fi) {
		const FormattedDoubleColumn *Col = FormattedCols[Fi];
		const std::size_t NameIdx = FormattedColBegin + Fi;
		if(Col == nullptr || NameIdx >= ColNames.size())
			continue;
		const std::string &Name = ColNames[NameIdx];
		for(std::size_t Ri = RowBegin; Ri < RowEnd; ++Ri) {
			if(Ri >= Col->Lengths.size())
				continue;
			const std::string_view View = Col->View(Ri);
			OutRows[Ri].emplace(Name, std::string(View.data(), View.size()));
		}
	}
}

} // namespace

void MaterializeMixedZip(const std::vector<std::string> &ColNames,
                         const std::vector<const std::vector<std::string> *> &StringCols,
                         const std::vector<const FormattedDoubleColumn *> &FormattedCols,
                         const std::size_t FormattedColBegin, const std::size_t RowCount,
                         std::vector<std::unordered_map<std::string, std::string>> &OutRows) {
	if(RowCount == 0 || ColNames.empty())
		return;
	const std::size_t Nstr = StringCols.size();
	const std::size_t Nfmt = FormattedCols.size();
	if(ColNames.size() != Nstr + Nfmt)
		return;
	OutRows.resize(RowCount);

	const bool Parallel =
	    EnvParallelSemistructured() && RowCount >= 8'000 && JobSystem::Instance().IsRunning();
	for(std::unordered_map<std::string, std::string> &Row : OutRows)
		Row.reserve(ColNames.size());
	if(!Parallel) {
		MaterializeMixedZipBand(ColNames, StringCols, FormattedCols, FormattedColBegin, 0, RowCount, OutRows);
		return;
	}

	const std::size_t Workers =
	    std::min<std::size_t>(12, std::max<std::size_t>(2, (RowCount + 7'999) / 8'000));
	const std::size_t RowsPerWorker = (RowCount + Workers - 1) / Workers;
	std::vector<std::future<void>> Futs;
	Futs.reserve(Workers);
	for(std::size_t W = 0; W < Workers; ++W) {
		const std::size_t RowBegin = W * RowsPerWorker;
		if(RowBegin >= RowCount)
			break;
		const std::size_t RowEnd = std::min(RowCount, RowBegin + RowsPerWorker);
		Futs.push_back(JobSystem::Instance().SubmitAsync([&ColNames, &StringCols, &FormattedCols, FormattedColBegin,
		                                                  &OutRows, RowBegin, RowEnd]() {
			MaterializeMixedZipBand(ColNames, StringCols, FormattedCols, FormattedColBegin, RowBegin, RowEnd, OutRows);
		}));
	}
	for(std::future<void> &F : Futs)
		F.wait();
}

void MaterializeColumnarZip(const std::vector<std::string> &ColNames,
                            const std::vector<const FormattedDoubleColumn *> &Cols, std::size_t RowCount,
                            std::vector<std::unordered_map<std::string, std::string>> &OutRows) {
	if(ColNames.size() != Cols.size() || RowCount == 0)
		return;
	OutRows.resize(RowCount);
	const std::size_t Ncol = ColNames.size();
	for(std::size_t Ri = 0; Ri < RowCount; ++Ri) {
		std::unordered_map<std::string, std::string> &Row = OutRows[Ri];
		Row.reserve(Ncol);
		for(std::size_t Ci = 0; Ci < Ncol; ++Ci) {
			const FormattedDoubleColumn *Col = Cols[Ci];
			if(!Col || Ri >= Col->Lengths.size())
				continue;
			Row.emplace(ColNames[Ci], std::string(Col->View(Ri)));
		}
	}
}

std::future<std::size_t> AsyncWriteColumn(const FormattedDoubleColumn &Col, int FileDescriptor) noexcept {
	return std::async(std::launch::async, [&Col, FileDescriptor]() -> std::size_t {
		if(FileDescriptor < 0 || Col.Chars.empty())
			return 0;
		const char *Base = Col.Chars.data();
		std::size_t Remaining = Col.Chars.size();
		std::size_t Written = 0;
		while(Remaining > 0) {
#if defined(_WIN32)
			const int N = _write(FileDescriptor, Base, static_cast<unsigned>(std::min(Remaining, static_cast<std::size_t>(0x7fffffff))));
#else
			const ssize_t N = ::write(FileDescriptor, Base, Remaining);
#endif
			if(N <= 0)
				break;
			Written += static_cast<std::size_t>(N);
			Base += N;
			Remaining -= static_cast<std::size_t>(N);
		}
		if(const char *FdEnv = EnvGet("ASTRALDB_CLIENT_PUSH_FD")) {
			(void)FdEnv;
		}
		return Written;
	});
}

} // namespace AstralDB::FormatDoubleSimd
