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

std::size_t AlignUp(std::size_t Value, std::size_t Align) {
	return (Value + Align - 1) & ~(Align - 1);
}

void FlushIcache(void *Ptr, std::size_t Size) {
	if(!Ptr || Size == 0)
		return;
#if defined(_WIN32)
	FlushInstructionCache(GetCurrentProcess(), Ptr, Size);
#elif defined(__APPLE__)
	/* Apple silicon: invalidate after RX transition; no dcache flush (can SIGBUS). */
	sys_icache_invalidate(Ptr, Size);
#else
	__builtin___clear_cache(static_cast<char *>(Ptr), static_cast<char *>(Ptr) + Size);
#endif
}

#if defined(__APPLE__)
void SetJitWriteProtect(bool Writable) { pthread_jit_write_protect_np(Writable ? 0 : 1); }
#endif

void *MapFreshPage(std::size_t Size) {
#if defined(_WIN32)
	return VirtualAlloc(nullptr, Size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#elif defined(__APPLE__)
	void *P = ::mmap(nullptr, Size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
	if(P == MAP_FAILED)
		return nullptr;
	return P;
#else
	void *P = ::mmap(nullptr, Size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if(P == MAP_FAILED)
		return nullptr;
	return P;
#endif
}

bool MakeExecutable(void *Base, std::size_t Size) {
#if defined(_WIN32)
	DWORD Old = 0;
	return VirtualProtect(Base, Size, PAGE_EXECUTE_READ, &Old) != 0;
#elif defined(__APPLE__)
	(void)Base;
	(void)Size;
	SetJitWriteProtect(false);
	return true;
#else
	return ::mprotect(Base, Size, PROT_READ | PROT_EXEC) == 0;
#endif
}

bool MakeWritable(void *Base, std::size_t Size) {
#if defined(_WIN32)
	DWORD Old = 0;
	return VirtualProtect(Base, Size, PAGE_READWRITE, &Old) != 0;
#elif defined(__APPLE__)
	(void)Base;
	(void)Size;
	SetJitWriteProtect(true);
	return true;
#else
	return ::mprotect(Base, Size, PROT_READ | PROT_WRITE) == 0;
#endif
}

void UnmapPage(void *Base, std::size_t Size) {
	if(!Base || Size == 0)
		return;
#if defined(__APPLE__)
	/* munmap(MAP_JIT) requires the region to be execute-only first. */
	SetJitWriteProtect(false);
#endif
#if defined(_WIN32)
	VirtualFree(Base, 0, MEM_RELEASE);
#else
	::munmap(Base, Size);
#endif
}

} // namespace

JitExecPage::~JitExecPage() {
	for(const Region &R : Regions_)
		UnmapPage(R.Base, R.Mapped);
}

bool JitExecPage::AppendRegion(const std::size_t MinMapped) {
	const std::size_t NewMap = (std::max)(PageSize, AlignUp(MinMapped, PageSize));
	void *Fresh = MapFreshPage(NewMap);
	if(!Fresh)
		return false;
	Regions_.push_back(Region{Fresh, NewMap, 0});
	return true;
}

void *JitExecPage::BumpInCurrent(const std::uint8_t *Code, const std::size_t Size) {
	if(Regions_.empty() && !AppendRegion(PageSize))
		return nullptr;
	Region &R = Regions_.back();
	const std::size_t Need = AlignUp(R.Used + Size, 16);
	if(Need > R.Mapped) {
		if(!AppendRegion(Need))
			return nullptr;
		return BumpInCurrent(Code, Size);
	}
	void *Entry = static_cast<std::uint8_t *>(R.Base) + R.Used;
	if(!MakeWritable(R.Base, R.Mapped))
		return nullptr;
	std::memcpy(Entry, Code, Size);
	if(!MakeExecutable(R.Base, R.Mapped))
		return nullptr;
	FlushIcache(Entry, Size);
	R.Used = Need;
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
