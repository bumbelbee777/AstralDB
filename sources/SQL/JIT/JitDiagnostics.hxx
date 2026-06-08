#pragma once

#include <cstdio>

namespace AstralDB {
namespace SQL {

/** Log Apple JIT host + signing hints to stderr (no-op off macOS). */
void PrintAppleJitDiagnostics(FILE *Out = stderr);

bool JitTraceEnabled() noexcept;
#if defined(__clang__) || defined(__GNUC__)
void JitTracef(const char *Fmt, ...) __attribute__((format(printf, 1, 2)));
#else
void JitTracef(const char *Fmt, ...);
#endif

} // namespace SQL
} // namespace AstralDB
