/* macOS/arm64 JIT probe: tries publish strategies; verbose stderr on every failure. */
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
#include <sys/wait.h>
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

static const uint8_t kRetKernel[] = {0x5f, 0x24, 0x03, 0xd5, 0xc0, 0x03, 0x5f, 0xd6};
static const uint8_t kSumKernel[] = {
	0x5f, 0x24, 0x03, 0xd5, 0xe2, 0x03, 0x1f, 0xaa, 0xc1, 0x00, 0x00, 0xb4, 0x03, 0x00, 0x40, 0xf9,
	0x42, 0x00, 0x03, 0x8b, 0x00, 0x20, 0x00, 0x91, 0x21, 0x04, 0x00, 0xd1, 0x81, 0xff, 0xff, 0x54,
	0xe0, 0x03, 0x02, 0xaa, 0xc0, 0x03, 0x5f, 0xd6,
};

typedef int64_t (*sum_fn)(const int64_t *, size_t);

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

static const int64_t kProbeSample[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
extern int64_t astraldb_jit_invoke_i64(const void *entry, const int64_t *values, size_t count);
#endif

static int invoke_sum(sum_fn fn, int64_t *out_got) {
	*out_got = -1;
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
	const int64_t got = astraldb_jit_invoke_i64((const void *)fn, kProbeSample, 10);
#else
	const int64_t got = fn(kProbeSample, 10);
#endif
	*out_got = got;
	if(got == 55)
		return 0;
	fprintf(stderr, "  sum invoke got=%lld (expected 55) sample=%p fn=%p\n", (long long)got, (void *)kProbeSample,
	        (void *)fn);
	fflush(stderr);
	return 12;
}

#if defined(__APPLE__)
static int invoke_ret_in_child(const void *fn) {
	fflush(NULL);
	const pid_t pid = fork();
	if(pid < 0) {
		log_errno("fork");
		return 10;
	}
	if(pid == 0) {
#if defined(__aarch64__) || defined(__arm64__)
		__asm__ volatile("blr %0" ::"r"(fn) : "x30", "memory");
#else
		((void (*)(void))fn)();
#endif
		_exit(0);
	}
	int status = 0;
	if(waitpid(pid, &status, 0) < 0) {
		log_errno("waitpid");
		return 11;
	}
	if(WIFEXITED(status)) {
		const int code = WEXITSTATUS(status);
		if(code == 0)
			return 0;
		fprintf(stderr, "  ret-smoke child exit=%d (expected 0)\n", code);
		fflush(stderr);
		return 12;
	}
	if(WIFSIGNALED(status)) {
		const int sig = WTERMSIG(status);
		fprintf(stderr, "  ret-smoke child signal=%d (%s) at fn=%p\n", sig, strsignal(sig), fn);
		fflush(stderr);
		return 13;
	}
	fprintf(stderr, "  ret-smoke child unknown wait status=0x%x\n", status);
	fflush(stderr);
	return 14;
}
#endif

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

static int republish_code(void *page, size_t map_bytes, const uint8_t *code, size_t code_size) {
	errno = 0;
	if(mprotect(page, map_bytes, PROT_READ | PROT_WRITE) != 0) {
		log_errno("mprotect(RW) republish");
		return 4;
	}
	memcpy(page, code, code_size);
	flush_icache(page, code_size);
	errno = 0;
	if(mprotect(page, map_bytes, PROT_READ | PROT_EXEC) != 0) {
		log_errno("mprotect(RX) republish");
		return 4;
	}
#if defined(__aarch64__) || defined(__arm64__)
	__asm__ __volatile__("isb" ::: "memory");
#endif
	return 0;
}

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

static int try_strategy(const strategy_t *st) {
	void *page = NULL;
	sum_fn fn = NULL;
	fprintf(stderr, "[probe] try strategy=%s\n", st->name);
	fflush(stderr);

	const int prc = st->publish(&page, 4096, kRetKernel, sizeof kRetKernel, &fn);
	if(prc != 0) {
		fprintf(stderr, "  ret publish rc=%d\n", prc);
		fflush(stderr);
		return prc;
	}
	fprintf(stderr, "[probe] ret-smoke strategy=%s\n", st->name);
#if defined(__APPLE__)
	print_vm_prot(page);
	const int smoke = invoke_ret_in_child(fn);
#else
	((void (*)(void))fn)();
	const int smoke = 0;
#endif
	if(smoke != 0) {
		fprintf(stderr, "  skip sum kernel (ret-smoke failed rc=%d)\n", smoke);
		munmap(page, 4096);
		fflush(stderr);
		return smoke;
	}

	const int repub = republish_code(page, 4096, kSumKernel, sizeof kSumKernel);
	if(repub != 0) {
		fprintf(stderr, "  sum republish rc=%d\n", repub);
		munmap(page, 4096);
		fflush(stderr);
		return repub;
	}
	fn = (sum_fn)page;
	fprintf(stderr, "  page=%p fn=%p code_size=%zu\n", page, (void *)fn, sizeof kSumKernel);
	hexdump("kernel", page, sizeof kSumKernel);
#if defined(__APPLE__)
	print_vm_prot(page);
#endif

	int64_t got = 0;
	const int irc = invoke_sum(fn, &got);
	if(irc == 0) {
		printf("ok strategy=%s sum=%lld\n", st->name, (long long)got);
		fflush(stdout);
		munmap(page, 4096);
		return 0;
	}
	if(irc == 12)
		fprintf(stderr, "  sum invoke returned wrong value (execute OK, bytecode/logic issue)\n");
	munmap(page, 4096);
	fflush(stderr);
	return irc;
}

int main(void) {
#if defined(__APPLE__)
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
		fprintf(stderr, "[probe] supported_np=0: prefer MAP_JIT+mprotect (VMAPPLE/CI)\n");
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
		if(rc == 0)
			return 0;
		last_rc = rc;
		fprintf(stderr, "[probe] strategy %s failed rc=%d\n", strategies[I].name, rc);
		fflush(stderr);
	}

	fprintf(stderr, "[probe] ALL STRATEGIES FAILED last_rc=%d\n", last_rc);
	fprintf(stderr, "[probe] rc key: 1=mmap_MAP_JIT_RWE 2=mmap_MAP_JIT_RW 3=mmap_anon 4=mprotect "
	                "10=fork 11=waitpid 12=wrong_sum 13=signal 14=wait_unknown\n");
#if defined(__APPLE__)
	fprintf(stderr, "[probe] if rc=13 and hardened_runtime=1: drop --options runtime from codesign\n");
	fprintf(stderr, "[probe] if rc=13 and hardened_runtime=0: VM may block JIT; check entitlements DER\n");
#endif
	fflush(stderr);
	return last_rc;
}
