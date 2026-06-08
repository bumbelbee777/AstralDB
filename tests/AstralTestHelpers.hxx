#pragma once
#include <SQL/JIT/JitCompiler.hxx>
#include <SQL/JIT/JitDiagnostics.hxx>
#include <doctest/doctest.h>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace AstralTest {
using doctest::Approx;
inline void AssertSqlOk(bool Ok, const char *Msg = "") { REQUIRE_MESSAGE(Ok, Msg); }
inline void RequireJitNative(AstralDB::SQL::JitCompiler &Jit) {
	if(!Jit.LastCompileWasNative()) {
		std::fprintf(stderr, "[jit] native compile/verify failed kernel=%s native=%d\n",
		             Jit.LastCompiledKernelName().c_str(), Jit.LastCompileWasNative() ? 1 : 0);
		Jit.DumpLastCompiledKernel();
		AstralDB::SQL::PrintAppleJitDiagnostics();
		std::fprintf(stderr, "[jit] export ASTRALDB_JIT_TRACE=1 for publish-step traces\n");
		std::fflush(stderr);
	}
	REQUIRE_MESSAGE(Jit.LastCompileWasNative(),
	                "JIT kernel did not compile/verify as native (see [jit] / [jit-diag] stderr above)");
}
class PerfSection {
	const char *L_;
	std::chrono::steady_clock::time_point T0_;

public:
	explicit PerfSection(const char *Lab) : L_(Lab), T0_(std::chrono::steady_clock::now()) {}
	PerfSection(const PerfSection &) = delete;
	PerfSection &operator=(const PerfSection &) = delete;
	~PerfSection() {
		const double Ms =
		    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0_).count();
		static std::mutex M;
		std::lock_guard<std::mutex> G(M);
		std::fprintf(stderr, "[perf] %s: %.4f ms\n", L_ ? L_ : "?", Ms);
		std::fflush(stderr);
	}
};
} // namespace AstralTest
