/* Minimal ARM64 sum-kernel probe (same bytecode as AstralDB JIT CompileSum on Apple Silicon).
 * Linux: mmap + mprotect. macOS: MAP_JIT (toggle or RWX per pthread_jit_write_protect_supported_np). */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/mman.h>
#ifndef MAP_JIT
#define MAP_JIT 0x0800
#endif
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

static const uint8_t kSumKernel[] = {
	0x5f, 0x24, 0x03, 0xd5, 0xe2, 0x03, 0x1f, 0xaa, 0xc1, 0x00, 0x00, 0xb4, 0x03, 0x00, 0x40, 0xf9,
	0x42, 0x00, 0x03, 0x8b, 0x00, 0x20, 0x00, 0x91, 0x21, 0x04, 0x00, 0xd1, 0x81, 0xff, 0xff, 0x54,
	0xe0, 0x03, 0x02, 0xaa, 0xc0, 0x03, 0x5f, 0xd6,
};

typedef int64_t (*sum_fn)(const int64_t *, size_t);

static void flush_icache(void *ptr, size_t size) {
	__builtin___clear_cache((char *)ptr, (char *)ptr + size);
}

static int publish_code(void **out_page, size_t map_bytes, const uint8_t *code, size_t code_size, sum_fn *out_fn) {
#if defined(__APPLE__)
	const int toggle = pthread_jit_write_protect_supported_np();
	printf("jit_write_protect_toggle=%d\n", toggle);
	int prot = PROT_READ | PROT_WRITE;
	if(!toggle)
		prot |= PROT_EXEC;
	void *page = mmap(NULL, map_bytes, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
	if(page == MAP_FAILED) {
		perror("mmap MAP_JIT");
		return 1;
	}
	if(toggle)
		pthread_jit_write_protect_np(0);
	memcpy(page, code, code_size);
	flush_icache(page, code_size);
	if(toggle) {
		pthread_jit_write_protect_np(1);
		__asm__ __volatile__("isb" ::: "memory");
	}
	*out_page = page;
	*out_fn = (sum_fn)page;
	return 0;
#else
	void *page = mmap(NULL, map_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if(page == MAP_FAILED)
		return 1;
	memcpy(page, code, code_size);
	if(mprotect(page, map_bytes, PROT_READ | PROT_EXEC) != 0)
		return 2;
	flush_icache(page, code_size);
	*out_page = page;
	*out_fn = (sum_fn)page;
	return 0;
#endif
}

int main(void) {
	void *page = NULL;
	sum_fn fn = NULL;
	const int rc = publish_code(&page, 4096, kSumKernel, sizeof kSumKernel, &fn);
	if(rc != 0) {
		fprintf(stderr, "publish failed rc=%d\n", rc);
		return rc;
	}
#if defined(__APPLE__)
	if(pthread_jit_write_protect_supported_np()) {
		pthread_jit_write_protect_np(1);
		__asm__ __volatile__("isb" ::: "memory");
	}
#endif
	const int64_t sample[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
	const int64_t got = fn(sample, 10);
	printf("sum=%lld expect=55\n", (long long)got);
	munmap(page, 4096);
	return got == 55 ? 0 : 3;
}
