#include <IO/MemoryGuard.hxx>
#include <IO/RuntimeLimits.hxx>

#include <atomic>
#include <chrono>
#include <thread>



namespace AstralDB {
namespace {

std::atomic<std::size_t> EstimatedBytes{0};
std::atomic<bool> SpikeLatch{false};

thread_local unsigned SpikeDepth = 0;

std::size_t NowMs() {
	using Clock = std::chrono::steady_clock;
	return static_cast<std::size_t>(
	    std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count()
	);
}

std::atomic<std::size_t> LastSampleMs{0};
std::atomic<std::size_t> LastSampleBytes{0};

} // namespace

bool MemoryGuard::GuardsActive() noexcept {
	if(SpikeDepth > 0)
		return true;

	if(!SpikeLatch.load(std::memory_order_relaxed))
		return false;

	return EstimatedBytes.load(std::memory_order_relaxed) >= SoftDisableBelowBytes;
}

void MemoryGuard::NoteAlloc(const std::size_t Bytes) noexcept {
	if(Bytes == 0)
		return;

	const std::size_t Prev = EstimatedBytes.fetch_add(Bytes, std::memory_order_relaxed);
	const std::size_t Now = Prev + Bytes;

	if(SpikeDepth > 0 || Bytes >= SpikeSingleAllocBytes) {
		SpikeLatch.store(true, std::memory_order_relaxed);
		return;

	}

	const std::size_t T = NowMs();
	const std::size_t LastT = LastSampleMs.exchange(T, std::memory_order_relaxed);
	const std::size_t LastB = LastSampleBytes.exchange(Now, std::memory_order_relaxed);

	if(T > LastT && T - LastT <= 100 && Now > LastB && Now - LastB >= SpikeGrowthBytes)
		SpikeLatch.store(true, std::memory_order_relaxed);

	if(Now < SoftDisableBelowBytes / 2)
		SpikeLatch.store(false, std::memory_order_relaxed);
}

void MemoryGuard::ReleaseBytes(const std::size_t Bytes) noexcept {
	if(Bytes == 0)
		return;

	std::size_t Cur = EstimatedBytes.load(std::memory_order_relaxed);
	while(Cur > 0 && !EstimatedBytes.compare_exchange_weak(Cur, Cur > Bytes ? Cur - Bytes : 0,
	                                                       std::memory_order_relaxed)) {
	}
}

bool MemoryGuard::AllowAlloc(const std::size_t Bytes) {
	const std::size_t Cap = RuntimeLimits::HardSessionCapBytes();

	if(Bytes > 0 && !SecureBoundsCheck(Bytes, Cap))
		return false;

	const std::size_t Cur = EstimatedBytes.load(std::memory_order_relaxed);

	if(GuardsActive() && Cur + Bytes > Cap)
		return false;

	NoteAlloc(Bytes);
	return true;
}

bool MemoryGuard::SecureBoundsCheck(const std::size_t Index, const std::size_t Size,

                                    const std::size_t ElementBytes) noexcept {
	if(ElementBytes == 0 || Size == 0)
		return Index < Size;

	if(Index >= Size)
		return false;

	if(ElementBytes > 1 && Index > (std::size_t(-1) / ElementBytes))
		return false;

	return Index * ElementBytes <= Size * ElementBytes;
}

bool MemoryGuard::BufferRangeFits(const std::size_t Offset, const std::size_t Bytes,

                                  const std::size_t Capacity) noexcept {
	if(Bytes == 0)
		return true;

	if(Offset > Capacity)
		return false;

	const std::size_t End = Offset + Bytes;
	return End >= Offset && End <= Capacity;
}

void MemoryGuard::ResetSession() noexcept {
	EstimatedBytes.store(0, std::memory_order_relaxed);
	SpikeLatch.store(false, std::memory_order_relaxed);
	LastSampleMs.store(0, std::memory_order_relaxed);
	LastSampleBytes.store(0, std::memory_order_relaxed);
}

std::size_t MemoryGuard::EstimatedSessionBytes() noexcept {
	return EstimatedBytes.load(std::memory_order_relaxed);
}

MemoryGuard::SpikeScope::SpikeScope() noexcept { ++SpikeDepth; }

MemoryGuard::SpikeScope::~SpikeScope() {
	if(SpikeDepth > 0)
		--SpikeDepth;
}

} // namespace AstralDB

