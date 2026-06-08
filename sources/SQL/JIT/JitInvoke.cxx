#include <SQL/JIT/JitInvoke.hxx>

#include <SQL/JIT/JitAppleMap.hxx>

namespace AstralDB {
namespace SQL {

namespace {

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
/** Unauthenticated blr x16 — typed Fn() may emit PAC/autib and SIGSEGV on mmap'd JIT. */
__attribute__((noinline)) int64_t AppleInvokeI64(int64_t (*Fn)(const int64_t *, std::size_t),
                                                 const int64_t *Values, std::size_t Count) {
	register const int64_t *Arg0 asm("x0") = Values;
	register std::size_t Arg1 asm("x1") = Count;
	register const void *Entry asm("x16") = reinterpret_cast<const void *>(Fn);
	(void)Arg0;
	(void)Arg1;
	(void)Entry;
	int64_t Out = 0;
	__asm__ volatile("blr x16\n"
	                 "mov %0, x0\n"
	                 : "=r"(Out)
	                 :
	                 : "x0", "x1", "x16", "x30", "memory", "cc");
	return Out;
}

__attribute__((noinline)) std::size_t AppleInvokeFilter(
    std::size_t (*Fn)(const int64_t *, std::size_t, int64_t, std::size_t *), const int64_t *Values,
    std::size_t Count, int64_t Literal, std::size_t *Out) {
	register const int64_t *Arg0 asm("x0") = Values;
	register std::size_t Arg1 asm("x1") = Count;
	register int64_t Arg2 asm("x2") = Literal;
	register std::size_t *Arg3 asm("x3") = Out;
	register const void *Entry asm("x16") = reinterpret_cast<const void *>(Fn);
	(void)Arg0;
	(void)Arg1;
	(void)Arg2;
	(void)Arg3;
	(void)Entry;
	std::size_t Ret = 0;
	__asm__ volatile("blr x16\n"
	                 "mov %0, x0\n"
	                 : "=r"(Ret)
	                 :
	                 : "x0", "x1", "x2", "x3", "x16", "x30", "memory", "cc");
	return Ret;
}
#endif

} // namespace

#if defined(__clang__)
#define ASTRALDB_JIT_INVOKE_ATTR __attribute__((noinline, optnone))
#elif defined(__GNUC__)
#define ASTRALDB_JIT_INVOKE_ATTR __attribute__((noinline))
#else
#define ASTRALDB_JIT_INVOKE_ATTR
#endif

ASTRALDB_JIT_INVOKE_ATTR
std::size_t JitInvokeFilter(std::size_t (*Fn)(const int64_t *, std::size_t, int64_t, std::size_t *),
                            const int64_t *Values, std::size_t Count, int64_t Literal, std::size_t *Out) {
#if defined(__APPLE__)
	AppleJitEnsureExecute();
#endif
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
	return AppleInvokeFilter(Fn, Values, Count, Literal, Out);
#else
	return Fn(Values, Count, Literal, Out);
#endif
}

ASTRALDB_JIT_INVOKE_ATTR
int64_t JitInvokeSum(int64_t (*Fn)(const int64_t *, std::size_t), const int64_t *Values, std::size_t Count) {
#if defined(__APPLE__)
	AppleJitEnsureExecute();
#endif
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
	return AppleInvokeI64(Fn, Values, Count);
#else
	return Fn(Values, Count);
#endif
}

ASTRALDB_JIT_INVOKE_ATTR
int64_t JitInvokeMin(int64_t (*Fn)(const int64_t *, std::size_t), const int64_t *Values, std::size_t Count) {
#if defined(__APPLE__)
	AppleJitEnsureExecute();
#endif
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
	return AppleInvokeI64(Fn, Values, Count);
#else
	return Fn(Values, Count);
#endif
}

ASTRALDB_JIT_INVOKE_ATTR
int64_t JitInvokeMax(int64_t (*Fn)(const int64_t *, std::size_t), const int64_t *Values, std::size_t Count) {
#if defined(__APPLE__)
	AppleJitEnsureExecute();
#endif
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
	return AppleInvokeI64(Fn, Values, Count);
#else
	return Fn(Values, Count);
#endif
}

} // namespace SQL
} // namespace AstralDB
