#include <SQL/JIT/JitCompiler.hxx>

#include <Database/Storage/ColumnFilterSimd.hxx>
#include <Database/Storage/VectorizedOps.hxx>

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace AstralDB {
namespace SQL {

namespace {

bool EnvJitEnabled() {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
	const char *V = std::getenv("ASTRALDB_JIT");
#pragma warning(pop)
#else
	const char *V = std::getenv("ASTRALDB_JIT");
#endif
	if(!V)
		return true;
	return V[0] != '0' && V[0] != 'n' && V[0] != 'N';
}

std::size_t FilterDenseInterpreted(FilterCompareOp Op, int64_t Literal, const int64_t *Values, std::size_t Count,
                                   std::size_t *OutIndices) {
	VectorizedFilter Filter(Values, Count, Op, Literal);
	std::vector<std::size_t> Hits;
	Filter.ExecuteIndices(Hits);
	for(std::size_t I = 0; I < Hits.size(); ++I)
		OutIndices[I] = Hits[I];
	return Hits.size();
}

std::size_t FilterDenseTrampolineEq(const int64_t *Values, std::size_t Count, int64_t Literal, std::size_t *OutIndices) {
	return FilterDenseInterpreted(FilterCompareOp::Eq, Literal, Values, Count, OutIndices);
}
std::size_t FilterDenseTrampolineNe(const int64_t *Values, std::size_t Count, int64_t Literal, std::size_t *OutIndices) {
	return FilterDenseInterpreted(FilterCompareOp::Ne, Literal, Values, Count, OutIndices);
}
std::size_t FilterDenseTrampolineGt(const int64_t *Values, std::size_t Count, int64_t Literal, std::size_t *OutIndices) {
	return FilterDenseInterpreted(FilterCompareOp::Gt, Literal, Values, Count, OutIndices);
}
std::size_t FilterDenseTrampolineGe(const int64_t *Values, std::size_t Count, int64_t Literal, std::size_t *OutIndices) {
	return FilterDenseInterpreted(FilterCompareOp::Ge, Literal, Values, Count, OutIndices);
}
std::size_t FilterDenseTrampolineLt(const int64_t *Values, std::size_t Count, int64_t Literal, std::size_t *OutIndices) {
	return FilterDenseInterpreted(FilterCompareOp::Lt, Literal, Values, Count, OutIndices);
}
std::size_t FilterDenseTrampolineLe(const int64_t *Values, std::size_t Count, int64_t Literal, std::size_t *OutIndices) {
	return FilterDenseInterpreted(FilterCompareOp::Le, Literal, Values, Count, OutIndices);
}

JitFilterDenseFn SelectInterpretedFilter(FilterCompareOp Op) {
	switch(Op) {
	case FilterCompareOp::Eq:
		return FilterDenseTrampolineEq;
	case FilterCompareOp::Ne:
		return FilterDenseTrampolineNe;
	case FilterCompareOp::Gt:
		return FilterDenseTrampolineGt;
	case FilterCompareOp::Ge:
		return FilterDenseTrampolineGe;
	case FilterCompareOp::Lt:
		return FilterDenseTrampolineLt;
	case FilterCompareOp::Le:
		return FilterDenseTrampolineLe;
	}
	return FilterDenseTrampolineEq;
}

int64_t InterpretedSum(const int64_t *Values, std::size_t Count) {
	return SumI64Avx2(Values, Count);
}

int64_t InterpretedMin(const int64_t *Values, std::size_t Count) {
	return MinI64Avx2(Values, Count);
}

int64_t InterpretedMax(const int64_t *Values, std::size_t Count) {
	return MaxI64Avx2(Values, Count);
}

template<typename Fn>
Fn PublishKernel(std::vector<std::uint8_t> &Code, std::unique_ptr<JitExecPage> &Page) {
	if(void *Entry = JitExecPage::PublishOwned(Code, Page))
		return reinterpret_cast<Fn>(Entry);
	return nullptr;
}

bool VerifyFilterDense(JitFilterDenseFn Fn, FilterCompareOp Op, int64_t Literal) {
	if(!Fn)
		return false;
	const int64_t Sample[] = {1, 5, 3, 5, 9};
	std::size_t GotIdx[5]{};
	std::size_t RefIdx[5]{};
	const std::size_t Got = Fn(Sample, 5, Literal, GotIdx);
	const std::size_t Expect = FilterDenseInterpreted(Op, Literal, Sample, 5, RefIdx);
	if(Got != Expect)
		return false;
	for(std::size_t I = 0; I < Got; ++I) {
		if(GotIdx[I] != RefIdx[I])
			return false;
	}
	return true;
}

bool VerifySum(JitSumFn Fn) {
	if(!Fn)
		return false;
	const int64_t Sample[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
	return Fn(Sample, 10) == 55;
}

bool VerifyMin(JitMinFn Fn) {
	if(!Fn)
		return false;
	const int64_t Sample[] = {4, -2, 9, 1};
	return Fn(Sample, 4) == -2;
}

bool VerifyMax(JitMaxFn Fn) {
	if(!Fn)
		return false;
	const int64_t Sample[] = {4, -2, 9, 1};
	return Fn(Sample, 4) == 9;
}

} // namespace

void JitCompiler::DumpLastCompiledKernel() const {
	std::fprintf(stderr, "[jit] kernel=%s size=%zu\n", LastKernelName_.c_str(), LastKernelBytes_.size());
	for(std::size_t I = 0; I < LastKernelBytes_.size(); ++I) {
		if(I % 16 == 0)
			std::fprintf(stderr, "[jit] %04zx:", I);
		std::fprintf(stderr, " %02x", LastKernelBytes_[I]);
		if(I % 16 == 15 || I + 1 == LastKernelBytes_.size())
			std::fprintf(stderr, "\n");
	}
	std::fflush(stderr);
}

void JitCompiler::NoteCompiledKernel(const char *Name, const std::vector<std::uint8_t> &Code) {
	LastKernelName_ = Name ? Name : "?";
	LastKernelBytes_ = Code;
#if defined(__APPLE__)
	DumpLastCompiledKernel();
#else
	const char *Dump = std::getenv("ASTRALDB_JIT_DUMP");
	if(Dump && Dump[0] != '0' && Dump[0] != 'n' && Dump[0] != 'N')
		DumpLastCompiledKernel();
#endif
}

JitCompiler &JitCompiler::Instance() {
	static JitCompiler Inst;
	return Inst;
}

bool JitCompiler::Enabled() noexcept { return EnvJitEnabled(); }

JitFilterDenseFn JitCompiler::CompileFilterDense(FilterCompareOp Op, int64_t Literal) {
	const JitFilterCacheKey Key{Op, Literal};
	const auto It = FilterCache_.find(Key);
	if(It != FilterCache_.end())
		return It->second;

	std::vector<std::uint8_t> Code;
	JitFilterDenseFn Fn = nullptr;
#if defined(__x86_64__) || defined(_M_X64)
	if(EmitX86_64FilterDenseKernel(Op, Literal, Code) && !Code.empty()) {
		NoteCompiledKernel("filter_dense", Code);
		Fn = PublishKernel<JitFilterDenseFn>(Code, CodePage_);
	}
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
	if(EmitArm64FilterDenseKernel(Op, Literal, Code) && !Code.empty()) {
		NoteCompiledKernel("filter_dense", Code);
		Fn = PublishKernel<JitFilterDenseFn>(Code, CodePage_);
	}
#endif

	if(Fn && !VerifyFilterDense(Fn, Op, Literal)) {
		std::fprintf(stderr, "[jit] verify failed: filter_dense\n");
		std::fflush(stderr);
		Fn = nullptr;
	}

	LastCompileNative_ = Fn != nullptr;
	if(!Fn)
		Fn = SelectInterpretedFilter(Op);

	FilterCache_[Key] = Fn;
	return Fn;
}

JitSumFn JitCompiler::CompileSum() {
	if(SumFn_)
		return SumFn_;

	std::vector<std::uint8_t> Code;
#if defined(__x86_64__) || defined(_M_X64)
	if(EmitX86_64SumKernel(Code) && !Code.empty()) {
		NoteCompiledKernel("sum", Code);
		SumFn_ = PublishKernel<JitSumFn>(Code, CodePage_);
	}
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
	if(EmitArm64SumKernel(Code) && !Code.empty()) {
		NoteCompiledKernel("sum", Code);
		SumFn_ = PublishKernel<JitSumFn>(Code, CodePage_);
	}
#endif

	if(SumFn_ && !VerifySum(SumFn_)) {
		std::fprintf(stderr, "[jit] verify failed: sum\n");
		std::fflush(stderr);
		SumFn_ = nullptr;
	}

	LastCompileNative_ = SumFn_ != nullptr;
	if(!SumFn_)
		SumFn_ = InterpretedSum;
	return SumFn_;
}

JitMinFn JitCompiler::CompileMin() {
	if(MinFn_)
		return MinFn_;

	std::vector<std::uint8_t> Code;
#if defined(__x86_64__) || defined(_M_X64)
	if(EmitX86_64MinKernel(Code) && !Code.empty()) {
		NoteCompiledKernel("min", Code);
		MinFn_ = PublishKernel<JitMinFn>(Code, CodePage_);
	}
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
	if(EmitArm64MinKernel(Code) && !Code.empty()) {
		NoteCompiledKernel("min", Code);
		MinFn_ = PublishKernel<JitMinFn>(Code, CodePage_);
	}
#endif

	if(MinFn_ && !VerifyMin(MinFn_)) {
		std::fprintf(stderr, "[jit] verify failed: min\n");
		std::fflush(stderr);
		MinFn_ = nullptr;
	}

	LastCompileNative_ = MinFn_ != nullptr;
	if(!MinFn_)
		MinFn_ = InterpretedMin;
	return MinFn_;
}

JitMaxFn JitCompiler::CompileMax() {
	if(MaxFn_)
		return MaxFn_;

	std::vector<std::uint8_t> Code;
#if defined(__x86_64__) || defined(_M_X64)
	if(EmitX86_64MaxKernel(Code) && !Code.empty()) {
		NoteCompiledKernel("max", Code);
		MaxFn_ = PublishKernel<JitMaxFn>(Code, CodePage_);
	}
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
	if(EmitArm64MaxKernel(Code) && !Code.empty()) {
		NoteCompiledKernel("max", Code);
		MaxFn_ = PublishKernel<JitMaxFn>(Code, CodePage_);
	}
#endif

	if(MaxFn_ && !VerifyMax(MaxFn_)) {
		std::fprintf(stderr, "[jit] verify failed: max\n");
		std::fflush(stderr);
		MaxFn_ = nullptr;
	}

	LastCompileNative_ = MaxFn_ != nullptr;
	if(!MaxFn_)
		MaxFn_ = InterpretedMax;
	return MaxFn_;
}

void JitCompiler::InvalidateAll() {
	FilterCache_.clear();
	SumFn_ = nullptr;
	MinFn_ = nullptr;
	MaxFn_ = nullptr;
	CodePage_.reset();
	LastCompileNative_ = false;
}

} // namespace SQL
} // namespace AstralDB
