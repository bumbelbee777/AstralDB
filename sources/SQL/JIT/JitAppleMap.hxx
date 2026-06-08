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

/** Apple Silicon always needs per-thread W^X toggling even if supported_np() is false. */
inline bool AppleJitUsesWriteToggle() noexcept {
#if defined(__aarch64__) || defined(__arm64__)
	return true;
#else
	return pthread_jit_write_protect_supported_np() != 0;
#endif
}

inline int AppleJitMmapProt() noexcept {
#if defined(__aarch64__) || defined(__arm64__)
	return PROT_READ | PROT_WRITE | PROT_EXEC;
#else
	int Prot = PROT_READ | PROT_WRITE;
	if(!AppleJitUsesWriteToggle())
		Prot |= PROT_EXEC;
	return Prot;
#endif
}

inline void AppleJitBeginWrite() noexcept {
	if(AppleJitUsesWriteToggle())
		pthread_jit_write_protect_np(0);
}

inline void AppleJitEndWrite() noexcept {
	if(AppleJitUsesWriteToggle()) {
		pthread_jit_write_protect_np(1);
#if defined(__aarch64__) || defined(__arm64__)
		__asm__ __volatile__("isb" ::: "memory");
#endif
	}
}

inline void AppleJitEnsureExecute() noexcept {
	if(AppleJitUsesWriteToggle())
		pthread_jit_write_protect_np(1);
}

#endif

} // namespace SQL
} // namespace AstralDB
