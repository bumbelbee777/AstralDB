/* macOS/arm64 JIT probe: publish strategies + trampoline invoke (JitInvokeAarch64.S). */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#ifndef MAP_JIT
#define MAP_JIT 0x0800
#endif
#ifndef CS_OPS_STATUS
#define CS_OPS_STATUS 0
#endif
#ifndef CS_RUNTIME
#define CS_RUNTIME 0x00010000u
#endif
extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#endif

#ifndef __APPLE__
#include <sys/mman.h>
#include <unistd.h>
#endif

/* BTI c; movz x0,#55; ret */
static const uint8_t kConst55Kernel[] = {0x5f, 0x24, 0x03, 0xd5, 0xe0, 0x06, 0x80, 0xd2, 0xc0, 0x03, 0x5f, 0xd6};
static const uint8_t kSumKernel[] = {
	0x5f, 0x24, 0x03, 0xd5, 0xe2, 0x03, 0x1f, 0xaa, 0xc1, 0x00, 0x00, 0xb4, 0x03, 0x00, 0x40, 0xf9,
	0x42, 0x00, 0x03, 0x8b, 0x00, 0x20, 0x00, 0x91, 0x21, 0x04, 0x00, 0xd1, 0x81, 0xff, 0xff, 0x54,
	0xe0, 0x03, 0x02, 0xaa, 0xc0, 0x03, 0x5f, 0xd6,
};

static const int64_t kProbeSample[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

enum { kMapBytes = 4096, kDataOff = 256 };

typedef int64_t (*sum_fn)(const int64_t *, size_t);

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
int64_t astraldb_jit_invoke_i64(const void *entry, const int64_t *values, size_t count);
#endif

static void log_errno(const char *step) {
	fprintf(stderr, "  FAIL %s: errno=%d (%s)\n", step, errno, strerror(errno));
	fflush(stderr);
}

static void flush_icache(void *ptr, size_t size) {
	__builtin___clear_cache((char *)ptr, (char *)ptr + size);
}

static void hexdump(const char *label, const void *ptr, size_t size) {
	const uint8_t *B = (const uint8_t *)ptr;
	fprintf(stderr, "  %s (%zu bytes):\n", label, size);
	for(size_t I = 0; I < size; ++I) {
		if(I % 16 == 0)
			fprintf(stderr, "    %04zx:", I);
		fprintf(stderr, " %02x", B[I]);
		if(I % 16 == 15 || I + 1 == size)
			fprintf(stderr, "\n");
	}
	fflush(stderr);
}

#if defined(__APPLE__)
static void print_host(void) {
	char ver[256] = {0};
	FILE *F = popen("sw_vers -productVersion 2>/dev/null", "r");
	if(F) {
		if(fgets(ver, sizeof ver, F))
			fprintf(stderr, "[probe] macOS=%s", ver);
		pclose(F);
	}
	fprintf(stderr, "[probe] pid=%d ppid=%d arch=", (int)getpid(), (int)getppid());
#if defined(__aarch64__) || defined(__arm64__)
	fprintf(stderr, "arm64");
#else
	fprintf(stderr, "other");
#endif
	fprintf(stderr, " supported_np=%d\n", pthread_jit_write_protect_supported_np());
	uint32_t flags = 0;
	if(csops(getpid(), CS_OPS_STATUS, &flags, sizeof(flags)) == 0)
		fprintf(stderr, "[probe] csops_status=0x%x hardened_runtime=%d\n", flags, (flags & CS_RUNTIME) != 0);
	else
		log_errno("csops");
#if defined(__aarch64__) || defined(__arm64__)
	fprintf(stderr, "[probe] trampoline=%p\n", (void *)astraldb_jit_invoke_i64);
#endif
	fflush(stderr);
}

static void print_vm_prot(void *page) {
	vm_address_t addr = (vm_address_t)(uintptr_t)page;
	vm_size_t region_size = 0;
	natural_t depth = 0;
	struct vm_region_submap_info_64 info;
	mach_msg_type_number_t info_count = VM_REGION_SUBMAP_INFO_COUNT_64;
	memset(&info, 0, sizeof info);
	const kern_return_t kr =
	    vm_region_recurse_64(mach_task_self(), &addr, &region_size, &depth, (vm_region_info_t)&info, &info_count);
	if(kr != KERN_SUCCESS) {
		fprintf(stderr, "  vm_region_recurse_64 kr=%d for page=%p\n", kr, page);
		fflush(stderr);
		return;
	}
	fprintf(stderr,
	        "  vm page=%p region=[%llx..%llx) cur_prot=0x%x max_prot=0x%x user_tag=%u\n", page,
	        (unsigned long long)addr, (unsigned long long)(addr + region_size), info.protection, info.max_protection,
	        info.user_tag);
	fflush(stderr);
}
#endif

#if defined(__aarch64__) || defined(__arm64__)
/* In-process only: fork()+libc in child deadlocks after parent fprintf/mmap (async-signal-unsafe). */
__attribute__((noinline)) static int64_t invoke_i64_raw(const void *entry, const int64_t *values, size_t count) {
	int64_t out = 0;
	__asm__ volatile("mov x16, %3\n"
	                 "mov x0, %1\n"
	                 "mov x1, %2\n"
	                 "blr x16\n"
	                 "mov %0, x0\n"
	                 : "=r"(out)
	                 : "r"(values), "r"(count), "r"(entry)
	                 : "x0", "x1", "x16", "x30", "memory", "cc");
	return out;
}
#endif

static int invoke_i64(const void *entry, const int64_t *values, size_t count, int64_t expect, int64_t *out_got) {
	*out_got = -1;
	fflush(NULL);
#if defined(__aarch64__) || defined(__arm64__)
	const int64_t got = invoke_i64_raw(entry, values, count);
#else
	const int64_t got = ((sum_fn)entry)(values, count);
#endif
	*out_got = got;
	if(got == expect)
		return 0;
	fprintf(stderr, "  invoke got=%lld expect=%lld entry=%p values=%p\n", (long long)got, (long long)expect, entry,
	        (const void *)values);
	fflush(stderr);
	return 12;
}

typedef struct {
	const char *name;
	int (*publish)(void **page, size_t map_bytes, const uint8_t *code, size_t code_size, sum_fn *out_fn);
} strategy_t;

#if defined(__APPLE__)
static int publish_mapjit_toggle(void **out_page, size_t map_bytes, const uint8_t *code, size_t code_size,
                                 sum_fn *out_fn) {
	errno = 0;
	void *page = mmap(NULL, map_bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
	if(page == MAP_FAILED) {
		log_errno("mmap(MAP_JIT|RWE)");
		return 1;
	}
	pthread_jit_write_protect_np(0);
	memcpy(page, code, code_size);
	flush_icache(page, code_size);
	pthread_jit_write_protect_np(1);
	__asm__ __volatile__("isb" ::: "memory");
	*out_page = page;
	*out_fn = (sum_fn)page;
	return 0;
}

static int publish_mapjit_rwx(void **out_page, size_t map_bytes, const uint8_t *code, size_t code_size, sum_fn *out_fn) {
	errno = 0;
	void *page = mmap(NULL, map_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
	if(page == MAP_FAILED) {
		log_errno("mmap(MAP_JIT|RW)");
		return 2;
	}
	memcpy(page, code, code_size);
	flush_icache(page, code_size);
	errno = 0;
	if(mprotect(page, map_bytes, PROT_READ | PROT_EXEC) != 0) {
		log_errno("mprotect(RX) after MAP_JIT|RW");
		munmap(page, map_bytes);
		return 4;
	}
#if defined(__aarch64__) || defined(__arm64__)
	__asm__ __volatile__("isb" ::: "memory");
#endif
	*out_page = page;
	*out_fn = (sum_fn)page;
	return 0;
}
#endif

static int publish_mprotect(void **out_page, size_t map_bytes, const uint8_t *code, size_t code_size, sum_fn *out_fn) {
	errno = 0;
	void *page = mmap(NULL, map_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS
#if defined(__APPLE__)
	                      | MAP_JIT
#endif
	                  ,
	                  -1, 0);
	if(page == MAP_FAILED) {
#if defined(__APPLE__)
		page = mmap(NULL, map_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if(page == MAP_FAILED) {
#endif
			log_errno("mmap(anon|RW)");
			return 3;
#if defined(__APPLE__)
		}
#endif
	}
	memcpy(page, code, code_size);
	flush_icache(page, code_size);
	errno = 0;
	if(mprotect(page, map_bytes, PROT_READ | PROT_EXEC) != 0) {
		log_errno("mprotect(RX)");
		munmap(page, map_bytes);
		return 4;
	}
#if defined(__aarch64__) || defined(__arm64__)
	__asm__ __volatile__("isb" ::: "memory");
#endif
	*out_page = page;
	*out_fn = (sum_fn)page;
	return 0;
}

static int publish_mprotect_with_sample(void **out_page, sum_fn *out_fn, const int64_t **out_values) {
	errno = 0;
	void *page = mmap(NULL, kMapBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS
#if defined(__APPLE__)
	                      | MAP_JIT
#endif
	                  ,
	                  -1, 0);
	if(page == MAP_FAILED) {
#if defined(__APPLE__)
		page = mmap(NULL, kMapBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if(page == MAP_FAILED) {
#endif
			log_errno("mmap(anon|RW)");
			return 3;
#if defined(__APPLE__)
		}
#endif
	}
	memcpy(page, kSumKernel, sizeof kSumKernel);
	memcpy((uint8_t *)page + kDataOff, kProbeSample, sizeof kProbeSample);
	flush_icache(page, sizeof kSumKernel);
	errno = 0;
	if(mprotect(page, kMapBytes, PROT_READ | PROT_EXEC) != 0) {
		log_errno("mprotect(RX)");
		munmap(page, kMapBytes);
		return 4;
	}
#if defined(__aarch64__) || defined(__arm64__)
	__asm__ __volatile__("isb" ::: "memory");
#endif
	*out_page = page;
	*out_fn = (sum_fn)page;
	*out_values = (const int64_t *)((uint8_t *)page + kDataOff);
	return 0;
}

static int run_kernel_test(const strategy_t *st, const uint8_t *code, size_t code_size, const char *label,
                           int64_t expect, const int64_t *values, size_t count) {
	void *page = NULL;
	sum_fn fn = NULL;
	fprintf(stderr, "[probe] %s strategy=%s\n", label, st->name);
	fflush(stderr);

	const int prc = st->publish(&page, kMapBytes, code, code_size, &fn);
	if(prc != 0) {
		fprintf(stderr, "  publish rc=%d\n", prc);
		fflush(stderr);
		return prc;
	}
#if defined(__APPLE__)
	print_vm_prot(page);
#endif
	fprintf(stderr, "  entry=%p values=%p count=%zu trampoline=%p\n", (void *)fn, (const void *)values, count,
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
	        (void *)astraldb_jit_invoke_i64);
#else
	        (void *)0);
#endif
	fflush(stderr);

	int64_t got = 0;
	const int irc = invoke_i64((const void *)fn, values, count, expect, &got);
	if(irc == 0) {
		fprintf(stderr, "  %s ok got=%lld\n", label, (long long)got);
		fflush(stderr);
		munmap(page, kMapBytes);
		return 0;
	}
	fprintf(stderr, "  %s failed rc=%d\n", label, irc);
	munmap(page, kMapBytes);
	fflush(stderr);
	return irc;
}

static int try_strategy(const strategy_t *st) {
	fprintf(stderr, "[probe] try strategy=%s\n", st->name);
	fflush(stderr);

	/* 1) Trampoline + execute path (no memory loads). */
	const int c55 = run_kernel_test(st, kConst55Kernel, sizeof kConst55Kernel, "const55", 55, NULL, 0);
	if(c55 != 0)
		return c55;

	/* 2) Sum kernel; sample in same JIT mapping (written before first mprotect RX). */
	void *page = NULL;
	sum_fn fn = NULL;
	const int64_t *in_page = NULL;
	int prc = 0;
	if(st->publish == publish_mprotect)
		prc = publish_mprotect_with_sample(&page, &fn, &in_page);
	else {
		prc = st->publish(&page, kMapBytes, kSumKernel, sizeof kSumKernel, &fn);
		if(prc == 0) {
			errno = 0;
			if(mprotect(page, kMapBytes, PROT_READ | PROT_WRITE) != 0) {
				log_errno("mprotect(RW) sample");
				prc = 4;
			} else {
				memcpy((uint8_t *)page + kDataOff, kProbeSample, sizeof kProbeSample);
				if(mprotect(page, kMapBytes, PROT_READ | PROT_EXEC) != 0) {
					log_errno("mprotect(RX) sample");
					prc = 4;
				}
				in_page = (const int64_t *)((uint8_t *)page + kDataOff);
			}
		}
	}
	if(prc != 0) {
		fprintf(stderr, "  sum publish rc=%d\n", prc);
		fflush(stderr);
		if(page)
			munmap(page, kMapBytes);
		return prc;
	}
	hexdump("kernel", page, sizeof kSumKernel);
#if defined(__APPLE__)
	print_vm_prot(page);
#endif

	int64_t got = 0;
	int irc = invoke_i64((const void *)fn, in_page, 10, 55, &got);
	if(irc == 0) {
		fprintf(stderr, "  sum(in-page) ok got=%lld\n", (long long)got);
		fflush(stderr);
		/* Also verify heap/.bss sample — same path as JitCompiler::VerifySum. */
		irc = invoke_i64((const void *)fn, kProbeSample, 10, 55, &got);
		if(irc != 0)
			fprintf(stderr, "  sum(external sample) failed rc=%d\n", irc);
	}
	munmap(page, kMapBytes);
	if(irc == 0) {
		printf("ok strategy=%s sum=%lld\n", st->name, (long long)got);
		fflush(stdout);
		return 0;
	}
	fprintf(stderr, "  sum failed rc=%d\n", irc);
	fflush(stderr);
	return irc;
}

#if defined(__APPLE__)
static void probe_timeout_handler(int sig) {
	(void)sig;
	const char msg[] = "[probe] TIMEOUT — probe hung (check fork/invoke deadlock)\n";
	(void)write(STDERR_FILENO, msg, sizeof msg - 1);
	_exit(124);
}
#endif

int main(void) {
#if defined(__APPLE__)
	signal(SIGALRM, probe_timeout_handler);
	alarm(60);
	print_host();
#endif

	strategy_t strategies[3];
	size_t n = 0;
#if defined(__APPLE__)
	if(pthread_jit_write_protect_supported_np()) {
		strategies[n++] = (strategy_t){"MAP_JIT+pthread", publish_mapjit_toggle};
		strategies[n++] = (strategy_t){"MAP_JIT+RWX", publish_mapjit_rwx};
		strategies[n++] = (strategy_t){"MAP_JIT+mprotect", publish_mprotect};
	} else {
		fprintf(stderr, "[probe] supported_np=0: MAP_JIT+mprotect (VMAPPLE/CI)\n");
		fflush(stderr);
		strategies[n++] = (strategy_t){"MAP_JIT+mprotect", publish_mprotect};
		strategies[n++] = (strategy_t){"MAP_JIT+RWX", publish_mapjit_rwx};
	}
#else
	strategies[n++] = (strategy_t){"MAP_JIT+mprotect", publish_mprotect};
#endif

	int last_rc = 1;
	for(size_t I = 0; I < n; ++I) {
		const int rc = try_strategy(&strategies[I]);
		if(rc == 0) {
#if defined(__APPLE__)
			alarm(0);
#endif
			return 0;
		}
		last_rc = rc;
		fprintf(stderr, "[probe] strategy %s failed rc=%d\n", strategies[I].name, rc);
		fflush(stderr);
	}

	fprintf(stderr, "[probe] ALL STRATEGIES FAILED last_rc=%d\n", last_rc);
	fprintf(stderr, "[probe] rc key: 1=mmap_MAP_JIT_RWE 2=mmap_MAP_JIT_RW 3=mmap_anon 4=mprotect "
	                "10=fork 11=waitpid 12=wrong_sum 13=signal 14=wait_unknown\n");
	fflush(stderr);
	return last_rc;
}
