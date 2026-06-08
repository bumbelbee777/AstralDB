#pragma once

#include <cstddef>

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/mman.h>
#ifndef MAP_JIT
#define MAP_JIT 0x0800
#endif
#endif

namespace AstralDB {
namespace SQL {

#if defined(__APPLE__)

/** MAP_JIT + pthread toggle (Apple Silicon with working supported_np). */
inline bool AppleJitUsesMapJitToggle() noexcept {
	return pthread_jit_write_protect_supported_np() != 0;
}

inline int AppleJitMmapProt() noexcept {
#if defined(__aarch64__) || defined(__arm64__)
	return PROT_READ | PROT_WRITE | PROT_EXEC;
#else
	return PROT_READ | PROT_WRITE;
#endif
}

inline void AppleJitBeginWrite() noexcept {
	if(AppleJitUsesMapJitToggle())
		pthread_jit_write_protect_np(0);
}

inline void AppleJitEndWrite() noexcept {
	if(AppleJitUsesMapJitToggle()) {
		pthread_jit_write_protect_np(1);
#if defined(__aarch64__) || defined(__arm64__)
		__asm__ __volatile__("isb" ::: "memory");
#endif
	}
}

inline void AppleJitEnsureExecute() noexcept {
	if(AppleJitUsesMapJitToggle())
		pthread_jit_write_protect_np(1);
}

inline bool AppleMprotectWritable(void *Base, std::size_t Size) noexcept {
	return ::mprotect(Base, Size, PROT_READ | PROT_WRITE) == 0;
}

inline bool AppleMprotectExecutable(void *Base, std::size_t Size) noexcept {
	return ::mprotect(Base, Size, PROT_READ | PROT_EXEC) == 0;
}

#endif

} // namespace SQL
} // namespace AstralDB
