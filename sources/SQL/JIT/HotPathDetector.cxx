#include <SQL/JIT/JitCompiler.hxx>

namespace AstralDB {
namespace SQL {

void HotPathDetector::Reset(std::size_t CodeSize) {
	Size_ = CodeSize;
	if(CodeSize == 0) {
		Counters_.reset();
		return;
	}
	Counters_ = std::make_unique<std::atomic<std::uint64_t>[]>(CodeSize);
	for(std::size_t I = 0; I < CodeSize; ++I)
		Counters_[I].store(0, std::memory_order_relaxed);
}

void HotPathDetector::RecordHit(std::size_t Ip) {
	if(Ip < Size_ && Counters_)
		Counters_[Ip].fetch_add(1, std::memory_order_relaxed);
}

bool HotPathDetector::IsHot(std::size_t Ip, std::uint64_t Threshold) const {
	if(Ip >= Size_ || !Counters_)
		return false;
	return Counters_[Ip].load(std::memory_order_relaxed) >= Threshold;
}

void RecordHotPathHit(HotPathDetector &Detector, std::size_t Ip) {
	Detector.RecordHit(Ip);
}

} // namespace SQL
} // namespace AstralDB
