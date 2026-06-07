#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace AstralDB {

/** Bump allocator for typed aggregate slabs during partition workers. */
class AggArena {
public:
	explicit AggArena(std::size_t BlockSize = 1u << 20) : BlockSize_(BlockSize) { PushBlock(); }

	~AggArena() = default;
	AggArena(const AggArena &) = delete;
	AggArena &operator=(const AggArena &) = delete;

	template<typename T>
	T *Alloc(std::size_t Count) {
		const std::size_t Bytes = Count * sizeof(T);
		const std::size_t Align = alignof(T);
		if(Cur_ + Bytes + Align > End_) {
			PushBlock();
			Cur_ = Blocks_.back().data();
			End_ = Cur_ + BlockSize_;
		}
		const std::size_t Pad = (Align - (reinterpret_cast<std::uintptr_t>(Cur_) % Align)) % Align;
		Cur_ += Pad;
		T *Out = reinterpret_cast<T *>(Cur_);
		Cur_ += Bytes;
		std::memset(Out, 0, Bytes);
		return Out;
	}

	void Reset() {
		Blocks_.clear();
		PushBlock();
		Cur_ = Blocks_.back().data();
		End_ = Cur_ + BlockSize_;
	}

private:
	void PushBlock() { Blocks_.emplace_back(BlockSize_); }

	std::size_t BlockSize_;
	std::vector<std::vector<std::byte>> Blocks_;
	std::byte *Cur_ = nullptr;
	std::byte *End_ = nullptr;
};

} // namespace AstralDB
