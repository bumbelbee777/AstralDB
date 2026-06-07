#include <DS/HyperLogLog.hxx>

#include <DS/SimdHash.hxx>
#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace AstralDB {

namespace {
constexpr double kAlphaM14 = 0.7213 / (1.0 + 1.079 / static_cast<double>(HyperLogLog::kRegisterCount));
constexpr std::uint32_t kHllMagic = 0x484C4C31u; // HLL1
} // namespace

HyperLogLog::HyperLogLog() : Registers_(kRegisterCount, 0) {}

std::uint64_t HyperLogLog::Hash(std::string_view Value) const { return SimdHash::Hash64(Value); }

std::size_t HyperLogLog::RegisterIndex(std::uint64_t Hash) const noexcept {
	return static_cast<std::size_t>(Hash >> (64 - kRegisterBits));
}

std::uint8_t HyperLogLog::Rank(std::uint64_t Hash) const noexcept {
	const std::uint64_t W = (Hash << kRegisterBits) | (UINT64_C(1) << kRegisterBits);
#if defined(__GNUC__) || defined(__clang__)
	return static_cast<std::uint8_t>(__builtin_clzll(W) + 1);
#elif defined(_MSC_VER)
	unsigned long Index = 0;
	if(_BitScanReverse64(&Index, W))
		return static_cast<std::uint8_t>(63ul - Index + 1ul);
	return 1;
#else
	std::uint8_t Pos = 1;
	while((W & (UINT64_C(1) << (63 - Pos + 1))) == 0 && Pos < 64 - kRegisterBits)
		++Pos;
	return Pos;
#endif
}

void HyperLogLog::Add(std::string_view Value) {
	const std::uint64_t H = Hash(Value);
	const std::size_t Idx = RegisterIndex(H);
	const std::uint8_t R = Rank(H);
	if(R > Registers_[Idx])
		Registers_[Idx] = R;
}

void HyperLogLog::Merge(const HyperLogLog &Other) {
	for(std::size_t I = 0; I < kRegisterCount; ++I)
		Registers_[I] = std::max(Registers_[I], Other.Registers_[I]);
}

std::uint64_t HyperLogLog::Estimate() const {
	double Sum = 0.0;
	int ZeroCount = 0;
	for(std::uint8_t R : Registers_) {
		Sum += std::pow(2.0, -static_cast<double>(R));
		if(R == 0)
			++ZeroCount;
	}
	double Est = kAlphaM14 * static_cast<double>(kRegisterCount) * static_cast<double>(kRegisterCount) / Sum;
	if(Est <= 2.5 * static_cast<double>(kRegisterCount) && ZeroCount > 0)
		Est = static_cast<double>(kRegisterCount) *
		      std::log(static_cast<double>(kRegisterCount) / static_cast<double>(ZeroCount));
	return static_cast<std::uint64_t>(Est + 0.5);
}

void HyperLogLog::LoadRegisters(const std::vector<std::uint8_t> &Data) {
	if(Data.size() >= kRegisterCount)
		Registers_.assign(Data.begin(), Data.begin() + static_cast<std::ptrdiff_t>(kRegisterCount));
}

std::vector<std::byte> HyperLogLog::Serialize() const {
	std::vector<std::byte> Out(4 + Registers_.size());
	const std::uint32_t Magic = kHllMagic;
	std::memcpy(Out.data(), &Magic, 4);
	for(std::size_t I = 0; I < Registers_.size(); ++I)
		Out[4 + I] = static_cast<std::byte>(Registers_[I]);
	return Out;
}

bool HyperLogLog::Deserialize(const std::byte *Data, std::size_t Size) {
	if(!Data || Size < 4 + kRegisterCount)
		return false;
	std::uint32_t Magic = 0;
	std::memcpy(&Magic, Data, 4);
	if(Magic != kHllMagic)
		return false;
	Registers_.resize(kRegisterCount);
	for(std::size_t I = 0; I < kRegisterCount; ++I)
		Registers_[I] = static_cast<std::uint8_t>(Data[4 + I]);
	return true;
}

} // namespace AstralDB
