#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

namespace AstralDB {

/** Bump-pointer arena over a fixed slab. Alignment-aware; overflows \c std::bad_alloc. */
class ArenaAllocator {
	std::vector<char> Data_;
	std::size_t Offset_ = 0;

public:
	explicit ArenaAllocator(std::size_t Size) : Data_(Size) {}

	[[nodiscard]] void *Allocate(std::size_t Size, std::size_t Align = alignof(std::max_align_t)) {
		const std::uintptr_t Base = reinterpret_cast<std::uintptr_t>(Data_.data());
		std::uintptr_t P = Base + Offset_;
		const std::uintptr_t Aligned = (P + Align - 1u) & ~(static_cast<std::uintptr_t>(Align) - 1u);
		const std::size_t NewOff = static_cast<std::size_t>(Aligned - Base + Size);
		if(NewOff > Data_.size())
			throw std::bad_alloc{};
		Offset_ = NewOff;
		return reinterpret_cast<void *>(Aligned);
	}

	char *data() noexcept { return Data_.data(); }
	const char *data() const noexcept { return Data_.data(); }
	std::size_t capacity() const noexcept { return Data_.size(); }

	void reset() noexcept { Offset_ = 0; }
};

} // namespace AstralDB
