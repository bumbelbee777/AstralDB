#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace AstralDB {

/** Simple HyperLogLog sketch for approximate COUNT DISTINCT. */
class HyperLogLog {
public:
	static constexpr std::size_t kRegisterBits = 14;
	static constexpr std::size_t kRegisterCount = 1u << kRegisterBits;

	HyperLogLog();

	void Add(std::string_view Value);
	void Merge(const HyperLogLog &Other);
	[[nodiscard]] std::uint64_t Estimate() const;
	[[nodiscard]] const std::vector<std::uint8_t> &Registers() const noexcept { return Registers_; }
	void LoadRegisters(const std::vector<std::uint8_t> &Data);

	[[nodiscard]] std::vector<std::byte> Serialize() const;
	bool Deserialize(const std::byte *Data, std::size_t Size);

private:
	[[nodiscard]] std::uint64_t Hash(std::string_view Value) const;
	[[nodiscard]] std::size_t RegisterIndex(std::uint64_t Hash) const noexcept;
	[[nodiscard]] std::uint8_t Rank(std::uint64_t Hash) const noexcept;

	std::vector<std::uint8_t> Registers_;
};

} // namespace AstralDB
