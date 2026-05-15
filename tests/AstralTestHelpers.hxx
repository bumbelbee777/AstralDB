#pragma once
#include <doctest/doctest.h>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace AstralTest {
using doctest::Approx;
inline void AssertSqlOk(bool Ok, const char *Msg = "") { REQUIRE_MESSAGE(Ok, Msg); }
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
