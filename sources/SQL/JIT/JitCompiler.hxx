#pragma once

#include <Database/Storage/ColumnFilterSimd.hxx>
#include <SQL/Bytecode/Bytecode.hxx>
#include <SQL/JIT/JitExecPage.hxx>

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace AstralDB {
namespace SQL {

/** Writes matching positions; returns match count (\p OutIndices has room for \p Count entries). */
using JitFilterDenseFn = std::size_t (*)(const int64_t *Values, std::size_t Count, int64_t Literal,
                                         std::size_t *OutIndices);
using JitSumFn = int64_t (*)(const int64_t *Values, std::size_t Count);
using JitMinFn = int64_t (*)(const int64_t *Values, std::size_t Count);
using JitMaxFn = int64_t (*)(const int64_t *Values, std::size_t Count);

struct JitFilterCacheKey {
	FilterCompareOp Op = FilterCompareOp::Eq;
	int64_t Literal = 0;
	bool operator==(const JitFilterCacheKey &O) const { return Op == O.Op && Literal == O.Literal; }
};

struct JitFilterCacheKeyHash {
	std::size_t operator()(const JitFilterCacheKey &K) const {
		return std::hash<int>{}(static_cast<int>(K.Op)) ^ (std::hash<int64_t>{}(K.Literal) << 1);
	}
};

class HotPathDetector {
public:
	void Reset(std::size_t CodeSize);
	void RecordHit(std::size_t Ip);
	bool IsHot(std::size_t Ip, std::uint64_t Threshold = 1000) const;

private:
	std::unique_ptr<std::atomic<std::uint64_t>[]> Counters_;
	std::size_t Size_ = 0;
};

/** JIT cache and compile helpers for columnar numeric kernels. */
class JitCompiler {
public:
	static JitCompiler &Instance();

	static bool Enabled() noexcept;

	JitFilterDenseFn CompileFilterDense(FilterCompareOp Op, int64_t Literal);
	JitSumFn CompileSum();
	JitMinFn CompileMin();
	JitMaxFn CompileMax();
	void InvalidateAll();

	/** True when the most recent Compile* published native machine code (not interpreter fallback). */
	bool LastCompileWasNative() const noexcept { return LastCompileNative_; }

	const std::string &LastCompiledKernelName() const noexcept { return LastKernelName_; }
	const std::vector<std::uint8_t> &LastCompiledKernelBytes() const noexcept { return LastKernelBytes_; }
	void DumpLastCompiledKernel() const;

private:
	JitCompiler() = default;

	void NoteCompiledKernel(const char *Name, const std::vector<std::uint8_t> &Code);

	std::unordered_map<JitFilterCacheKey, JitFilterDenseFn, JitFilterCacheKeyHash> FilterCache_;
	JitSumFn SumFn_ = nullptr;
	JitMinFn MinFn_ = nullptr;
	JitMaxFn MaxFn_ = nullptr;
	std::unique_ptr<JitExecPage> CodePage_;
	bool LastCompileNative_ = false;
	std::string LastKernelName_;
	std::vector<std::uint8_t> LastKernelBytes_;
};

bool EmitX86_64FilterDenseKernel(FilterCompareOp Op, int64_t Literal, std::vector<std::uint8_t> &Out);
bool EmitX86_64SumKernel(std::vector<std::uint8_t> &Out);
bool EmitX86_64MinKernel(std::vector<std::uint8_t> &Out);
bool EmitX86_64MaxKernel(std::vector<std::uint8_t> &Out);

#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
bool EmitArm64FilterDenseKernel(FilterCompareOp Op, int64_t Literal, std::vector<std::uint8_t> &Out);
bool EmitArm64SumKernel(std::vector<std::uint8_t> &Out);
bool EmitArm64MinKernel(std::vector<std::uint8_t> &Out);
bool EmitArm64MaxKernel(std::vector<std::uint8_t> &Out);
#endif

} // namespace SQL
} // namespace AstralDB
