#include <SQL/JIT/JitExecPage.hxx>

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#ifndef MAP_JIT
#define MAP_JIT 0x0800
#endif
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace AstralDB {
namespace SQL {

namespace {

/** Apple allows only one MAP_JIT region per process; one bump allocator backs all kernels. */
#if defined(__APPLE__)
constexpr std::size_t AppleJitMapBytes = 65536;
void *AppleJitBase = nullptr;
std::size_t AppleJitMapped = 0;
std::size_t AppleJitUsed = 0;
#endif

std::size_t AlignUp(std::size_t Value, std::size_t Align) {
	return (Value + Align - 1) & ~(Align - 1);
}

void FlushIcache(void *Ptr, std::size_t Size) {
	if(!Ptr || Size == 0)
		return;
#if defined(_WIN32)
	FlushInstructionCache(GetCurrentProcess(), Ptr, Size);
#elif defined(__APPLE__)
	__builtin___clear_cache(static_cast<char *>(Ptr), static_cast<char *>(Ptr) + Size);
	sys_icache_invalidate(Ptr, Size);
#if defined(__aarch64__) || defined(__arm64__)
	__asm__ __volatile__("isb" ::: "memory");
#endif
#else
	__builtin___clear_cache(static_cast<char *>(Ptr), static_cast<char *>(Ptr) + Size);
#endif
}

#if defined(__APPLE__)

bool AppleEnsureJitRegion() {
	if(AppleJitBase)
		return true;
	void *P = ::mmap(nullptr, AppleJitMapBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
	if(P == MAP_FAILED)
		return false;
	AppleJitBase = P;
	AppleJitMapped = AppleJitMapBytes;
	AppleJitUsed = 0;
	return true;
}

bool ApplePublishBytes(void *Entry, const std::uint8_t *Code, std::size_t Size) {
	pthread_jit_write_protect_np(0);
	std::memcpy(Entry, Code, Size);
	pthread_jit_write_protect_np(1);
	FlushIcache(Entry, Size);
	return true;
}

void AppleReleaseJitRegion() {
	if(!AppleJitBase)
		return;
	pthread_jit_write_protect_np(1);
	::munmap(AppleJitBase, AppleJitMapped);
	AppleJitBase = nullptr;
	AppleJitMapped = 0;
	AppleJitUsed = 0;
}

#endif

#if !defined(__APPLE__)
void *MapFreshPage(std::size_t Size) {
#if defined(_WIN32)
	return VirtualAlloc(nullptr, Size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
	void *P = ::mmap(nullptr, Size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if(P == MAP_FAILED)
		return nullptr;
	return P;
#endif
}
#endif

#if !defined(__APPLE__)
bool MakeExecutable(void *Base, std::size_t Size) {
#if defined(_WIN32)
	DWORD Old = 0;
	return VirtualProtect(Base, Size, PAGE_EXECUTE_READ, &Old) != 0;
#else
	return ::mprotect(Base, Size, PROT_READ | PROT_EXEC) == 0;
#endif
}

bool MakeWritable(void *Base, std::size_t Size) {
#if defined(_WIN32)
	DWORD Old = 0;
	return VirtualProtect(Base, Size, PAGE_READWRITE, &Old) != 0;
#else
	return ::mprotect(Base, Size, PROT_READ | PROT_WRITE) == 0;
#endif
}
void UnmapPage(void *Base, std::size_t Size) {
	if(!Base || Size == 0)
		return;
#if defined(_WIN32)
	VirtualFree(Base, 0, MEM_RELEASE);
#else
	::munmap(Base, Size);
#endif
}

#endif

} // namespace

JitExecPage::~JitExecPage() {
#if defined(__APPLE__)
	if(!Regions_.empty())
		AppleReleaseJitRegion();
	Regions_.clear();
#else
	for(const Region &R : Regions_)
		UnmapPage(R.Base, R.Mapped);
#endif
}

bool JitExecPage::AppendRegion(const std::size_t MinMapped) {
#if defined(__APPLE__)
	(void)MinMapped;
	if(!Regions_.empty())
		return true;
	if(!AppleEnsureJitRegion())
		return false;
	Regions_.push_back(Region{AppleJitBase, AppleJitMapped, AppleJitUsed});
	return true;
#else
	const std::size_t NewMap = (std::max)(PageSize, AlignUp(MinMapped, PageSize));
	void *Fresh = MapFreshPage(NewMap);
	if(!Fresh)
		return false;
	Regions_.push_back(Region{Fresh, NewMap, 0});
	return true;
#endif
}

void *JitExecPage::BumpInCurrent(const std::uint8_t *Code, const std::size_t Size) {
	if(Regions_.empty() && !AppendRegion(PageSize))
		return nullptr;
	Region &R = Regions_.back();
	const std::size_t Need = AlignUp(R.Used + Size, 16);
#if defined(__APPLE__)
	if(Need > R.Mapped)
		return nullptr;
#else
	if(Need > R.Mapped) {
		if(!AppendRegion(Need))
			return nullptr;
		return BumpInCurrent(Code, Size);
	}
#endif
	void *Entry = static_cast<std::uint8_t *>(R.Base) + R.Used;
#if defined(__APPLE__)
	if(!ApplePublishBytes(Entry, Code, Size))
		return nullptr;
#else
	if(!MakeWritable(R.Base, R.Mapped))
		return nullptr;
	std::memcpy(Entry, Code, Size);
	if(!MakeExecutable(R.Base, R.Mapped))
		return nullptr;
	FlushIcache(Entry, Size);
#endif
	R.Used = Need;
#if defined(__APPLE__)
	AppleJitUsed = R.Used;
#endif
	return Entry;
}

void *JitExecPage::Publish(const std::uint8_t *Code, const std::size_t Size) {
	if(!Code || Size == 0)
		return nullptr;
	return BumpInCurrent(Code, Size);
}

void *JitExecPage::PublishOwned(std::vector<std::uint8_t> &Code, std::unique_ptr<JitExecPage> &Out) {
	if(Code.empty())
		return nullptr;
	if(!Out)
		Out = std::make_unique<JitExecPage>();
	return Out->Publish(Code.data(), Code.size());
}

} // namespace SQL
} // namespace AstralDB
