#include <SQL/JIT/JitInvoke.hxx>

namespace AstralDB {
namespace SQL {

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
#if defined(__aarch64__) || defined(__arm64__)
	__asm__ __volatile__("" ::: "memory");
#endif
	return Fn(Values, Count, Literal, Out);
}

ASTRALDB_JIT_INVOKE_ATTR
int64_t JitInvokeSum(int64_t (*Fn)(const int64_t *, std::size_t), const int64_t *Values, std::size_t Count) {
#if defined(__aarch64__) || defined(__arm64__)
	__asm__ __volatile__("" ::: "memory");
#endif
	return Fn(Values, Count);
}

ASTRALDB_JIT_INVOKE_ATTR
int64_t JitInvokeMin(int64_t (*Fn)(const int64_t *, std::size_t), const int64_t *Values, std::size_t Count) {
#if defined(__aarch64__) || defined(__arm64__)
	__asm__ __volatile__("" ::: "memory");
#endif
	return Fn(Values, Count);
}

ASTRALDB_JIT_INVOKE_ATTR
int64_t JitInvokeMax(int64_t (*Fn)(const int64_t *, std::size_t), const int64_t *Values, std::size_t Count) {
#if defined(__aarch64__) || defined(__arm64__)
	__asm__ __volatile__("" ::: "memory");
#endif
	return Fn(Values, Count);
}

} // namespace SQL
} // namespace AstralDB
