#include <SQL/JIT/JitDiagnostics.hxx>

#include <cstdarg>
#include <cstdlib>

#if defined(__APPLE__)
#include <pthread.h>
#include <unistd.h>

#ifndef CS_OPS_STATUS
#define CS_OPS_STATUS 0
#endif
#ifndef CS_RUNTIME
#define CS_RUNTIME 0x00010000u
#endif
extern "C" int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#endif

namespace AstralDB {
namespace SQL {

bool JitTraceEnabled() noexcept {
	const char *E = std::getenv("ASTRALDB_JIT_TRACE");
	return E && E[0] != '\0' && E[0] != '0' && E[0] != 'n' && E[0] != 'N';
}

void JitTracef(const char *Fmt, ...) {
	if(!JitTraceEnabled() || !Fmt)
		return;
	std::fprintf(stderr, "[jit-trace] ");
	std::va_list Ap;
	va_start(Ap, Fmt);
	std::vfprintf(stderr, Fmt, Ap);
	va_end(Ap);
	std::fprintf(stderr, "\n");
	std::fflush(stderr);
}

void PrintAppleJitDiagnostics(FILE *Out) {
	if(!Out)
		Out = stderr;
#if defined(__APPLE__)
	std::fprintf(Out, "[jit-diag] platform=macOS");
#if defined(__aarch64__) || defined(__arm64__)
	std::fprintf(Out, " arch=arm64");
#else
	std::fprintf(Out, " arch=%s", sizeof(void *) == 8 ? "x86_64" : "other");
#endif
	std::fprintf(Out, " supported_np=%d\n", pthread_jit_write_protect_supported_np());
	uint32_t Flags = 0;
	if(csops(getpid(), CS_OPS_STATUS, &Flags, sizeof(Flags)) == 0)
		std::fprintf(Out, "[jit-diag] csops_status=0x%x hardened_runtime=%d\n", Flags,
		             (Flags & CS_RUNTIME) != 0 ? 1 : 0);
	else
		std::fprintf(Out, "[jit-diag] csops_status=unavailable\n");
	std::fprintf(Out,
	             "[jit-diag] hints: ad-hoc sign must NOT use --options runtime on CI; "
	             "need allow-jit and/or allow-unsigned-executable-memory in DER blob; "
	             "set ASTRALDB_JIT_TRACE=1 for publish traces\n");
#else
	std::fprintf(Out, "[jit-diag] platform=non-Apple (no Apple JIT diagnostics)\n");
#endif
	std::fflush(Out);
}

} // namespace SQL
} // namespace AstralDB
