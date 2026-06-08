#include <SQL/JIT/JitInvoke.hxx>

#include <SQL/JIT/JitAppleMap.hxx>

namespace AstralDB {
namespace SQL {

namespace {

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
__attribute__((noinline)) int64_t AppleBlrI64(const void *Target, const int64_t *Values, std::size_t Count) {
	int64_t Out = 0;
	__asm__ volatile("mov x0, %2\n"
	                 "mov x1, %3\n"
	                 "blr %1\n"
	                 "mov %0, x0\n"
	                 : "=r"(Out)
	                 : "r"(Target), "r"(Values), "r"(Count)
	                 : "x0", "x1", "x30", "memory", "cc");
	return Out;
}

__attribute__((noinline)) std::size_t AppleBlrFilter(const void *Target, const int64_t *Values, std::size_t Count,
                                                      int64_t Literal, std::size_t *Out) {
	std::size_t Ret = 0;
	__asm__ volatile("mov x0, %2\n"
	                 "mov x1, %3\n"
	                 "mov x2, %4\n"
	                 "mov x3, %5\n"
	                 "blr %1\n"
	                 "mov %0, x0\n"
	                 : "=r"(Ret)
	                 : "r"(Target), "r"(Values), "r"(Count), "r"(Literal), "r"(Out)
	                 : "x0", "x1", "x2", "x3", "x30", "memory", "cc");
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
	return AppleBlrFilter(reinterpret_cast<const void *>(Fn), Values, Count, Literal, Out);
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
	return AppleBlrI64(reinterpret_cast<const void *>(Fn), Values, Count);
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
	return AppleBlrI64(reinterpret_cast<const void *>(Fn), Values, Count);
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
	return AppleBlrI64(reinterpret_cast<const void *>(Fn), Values, Count);
#else
	return Fn(Values, Count);
#endif
}

} // namespace SQL
} // namespace AstralDB
