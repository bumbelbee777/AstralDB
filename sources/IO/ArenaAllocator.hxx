#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace AstralDB {

/** Round \p Size up to \p Align (power of two). */
[[nodiscard]] inline constexpr std::size_t AlignUp(std::size_t Size, std::size_t Align) noexcept {
	return (Size + Align - 1u) & ~(Align - 1u);
}

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

	template<class T>
	[[nodiscard]] T *Allocate(std::size_t Count = 1, std::size_t Align = alignof(T)) {
		return static_cast<T *>(Allocate(Count * sizeof(T), Align));
	}

	[[nodiscard]] std::size_t checkpoint() const noexcept { return Offset_; }
	void reset_to(std::size_t Mark) noexcept { Offset_ = Mark; }
	void reset() noexcept { Offset_ = 0; }

	char *data() noexcept { return Data_.data(); }
	const char *data() const noexcept { return Data_.data(); }
	[[nodiscard]] std::size_t capacity() const noexcept { return Data_.size(); }
	[[nodiscard]] std::size_t used() const noexcept { return Offset_; }
	[[nodiscard]] std::size_t remaining() const noexcept { return capacity() - used(); }
};

/** RAII checkpoint on an arena; restores offset on destruction. */
class ArenaScope {
	ArenaAllocator &Arena_;
	std::size_t Mark_;

public:
	explicit ArenaScope(ArenaAllocator &Arena) : Arena_(Arena), Mark_(Arena.checkpoint()) {}
	~ArenaScope() { Arena_.reset_to(Mark_); }

	ArenaScope(const ArenaScope &) = delete;
	ArenaScope &operator=(const ArenaScope &) = delete;
};

/** Multi-chunk bump arena; grows by allocating new slabs when the current chunk is full. */
class GrowingArenaAllocator {
	std::vector<std::vector<char>> Chunks_;
	std::size_t CurOffset_ = 0;
	std::size_t ChunkBytes_;

	void PushChunk(std::size_t Bytes) {
		Chunks_.emplace_back(Bytes);
		CurOffset_ = 0;
	}

public:
	explicit GrowingArenaAllocator(std::size_t InitialChunkBytes = 64 * 1024)
	    : ChunkBytes_(InitialChunkBytes > 0 ? InitialChunkBytes : 64 * 1024) {
		PushChunk(ChunkBytes_);
	}

	[[nodiscard]] void *Allocate(std::size_t Size, std::size_t Align = alignof(std::max_align_t)) {
		for(;;) {
			char *Base = Chunks_.back().data();
			const std::uintptr_t BaseU = reinterpret_cast<std::uintptr_t>(Base);
			std::uintptr_t P = BaseU + CurOffset_;
			const std::uintptr_t Aligned = (P + Align - 1u) & ~(static_cast<std::uintptr_t>(Align) - 1u);
			const std::size_t NewOff = static_cast<std::size_t>(Aligned - BaseU + Size);
			if(NewOff <= Chunks_.back().size()) {
				CurOffset_ = NewOff;
				return reinterpret_cast<void *>(Aligned);
			}
			PushChunk(std::max(ChunkBytes_, AlignUp(Size + 64, Align)));
		}
	}

	template<class T>
	[[nodiscard]] T *Allocate(std::size_t Count = 1, std::size_t Align = alignof(T)) {
		return static_cast<T *>(Allocate(Count * sizeof(T), Align));
	}

	/** Contiguous view of the first chunk (for \c std::pmr::monotonic_buffer_resource). */
	char *primary_data() noexcept { return Chunks_.empty() ? nullptr : Chunks_.front().data(); }
	const char *primary_data() const noexcept { return Chunks_.empty() ? nullptr : Chunks_.front().data(); }
	[[nodiscard]] std::size_t primary_capacity() const noexcept {
		return Chunks_.empty() ? 0 : Chunks_.front().size();
	}

	void reset() {
		if(Chunks_.size() > 1)
			Chunks_.resize(1);
		CurOffset_ = 0;
	}

	[[nodiscard]] std::size_t chunk_count() const noexcept { return Chunks_.size(); }
	[[nodiscard]] std::size_t total_reserved() const noexcept {
		std::size_t Sum = 0;
		for(const auto &C : Chunks_)
			Sum += C.size();
		return Sum;
	}
};

/** Fixed-capacity free-list pool for same-sized objects (no destructors on release). */
template<class T>
class ObjectPool {
	static_assert(std::is_trivially_destructible_v<T>);

	std::vector<T> Storage_;
	std::vector<T *> Free_;

public:
	explicit ObjectPool(std::size_t Capacity = 64) : Storage_(Capacity) {
		Free_.reserve(Capacity);
		for(auto &Slot : Storage_)
			Free_.push_back(&Slot);
	}

	[[nodiscard]] T *Acquire() {
		if(Free_.empty())
			return nullptr;
		T *P = Free_.back();
		Free_.pop_back();
		return P;
	}

	void Release(T *P) noexcept {
		if(P)
			Free_.push_back(P);
	}

	[[nodiscard]] std::size_t available() const noexcept { return Free_.size(); }
};

/** \c std::vector backed by a bump arena (no individual frees; arena owns storage). */
template<class T>
class ArenaVector {
	ArenaAllocator &Arena_;
	T *Data_ = nullptr;
	std::size_t Size_ = 0;
	std::size_t Cap_ = 0;

public:
	explicit ArenaVector(ArenaAllocator &Arena) : Arena_(Arena) {}

	void reserve(std::size_t N) {
		if(N <= Cap_)
			return;
		Data_ = Arena_.Allocate<T>(N);
		Cap_ = N;
	}

	void push_back(const T &V) {
		if(Size_ >= Cap_)
			reserve(Cap_ == 0 ? 8 : Cap_ * 2);
		Data_[Size_++] = V;
	}

	void push_back(T &&V) {
		if(Size_ >= Cap_)
			reserve(Cap_ == 0 ? 8 : Cap_ * 2);
		Data_[Size_++] = std::move(V);
	}

	[[nodiscard]] std::size_t size() const noexcept { return Size_; }
	[[nodiscard]] bool empty() const noexcept { return Size_ == 0; }
	T *data() noexcept { return Data_; }
	const T *data() const noexcept { return Data_; }
	T &operator[](std::size_t I) { return Data_[I]; }
	const T &operator[](std::size_t I) const { return Data_[I]; }
};

} // namespace AstralDB
