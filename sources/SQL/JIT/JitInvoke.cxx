#include <SQL/JIT/JitInvoke.hxx>

#include <SQL/JIT/JitAppleMap.hxx>

namespace AstralDB {
namespace SQL {

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
extern "C" int64_t astraldb_jit_invoke_i64(const void *Entry, const int64_t *Values, std::size_t Count);
extern "C" std::size_t astraldb_jit_invoke_filter(const void *Entry, const int64_t *Values, std::size_t Count,
                                                    int64_t Literal, std::size_t *Out);
#endif

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
	return astraldb_jit_invoke_filter(reinterpret_cast<const void *>(Fn), Values, Count, Literal, Out);
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
	return astraldb_jit_invoke_i64(reinterpret_cast<const void *>(Fn), Values, Count);
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
	return astraldb_jit_invoke_i64(reinterpret_cast<const void *>(Fn), Values, Count);
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
	return astraldb_jit_invoke_i64(reinterpret_cast<const void *>(Fn), Values, Count);
#else
	return Fn(Values, Count);
#endif
}

} // namespace SQL
} // namespace AstralDB
