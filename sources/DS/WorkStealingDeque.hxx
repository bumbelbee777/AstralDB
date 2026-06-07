#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace AstralDB {

/** Chase-Lev work-stealing deque (optional backend for ThreadJobQueue). */
template<typename T>
class WorkStealingDeque {
public:
	explicit WorkStealingDeque(std::size_t Capacity = 4096) : Buffer_(Capacity), Mask_(Capacity - 1) {}

	bool PushBottom(T Item) {
		const std::size_t B = Bottom_.load(std::memory_order_relaxed);
		const std::size_t T = Top_.load(std::memory_order_acquire);
		if(B - T >= Buffer_.size())
			return false;
		Buffer_[B & Mask_] = std::move(Item);
		Bottom_.store(B + 1, std::memory_order_release);
		return true;
	}

	bool PopBottom(T &Out) {
		const std::size_t B = Bottom_.load(std::memory_order_relaxed) - 1;
		Bottom_.store(B, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_seq_cst);
		const std::size_t T = Top_.load(std::memory_order_relaxed);
		if(T <= B) {
			Out = std::move(Buffer_[B & Mask_]);
			if(T == B) {
				if(!Top_.compare_exchange_strong(T, T + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
					Bottom_.store(B + 1, std::memory_order_relaxed);
					return false;
				}
				Bottom_.store(B + 1, std::memory_order_relaxed);
			}
			return true;
		}
		Bottom_.store(B + 1, std::memory_order_relaxed);
		return false;
	}

	bool Steal(T &Out) {
		std::size_t T = Top_.load(std::memory_order_acquire);
		std::atomic_thread_fence(std::memory_order_seq_cst);
		const std::size_t B = Bottom_.load(std::memory_order_acquire);
		if(T >= B)
			return false;
		Out = Buffer_[T & Mask_];
		if(!Top_.compare_exchange_strong(T, T + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
			return false;
		return true;
	}

private:
	std::vector<T> Buffer_;
	std::size_t Mask_;
	std::atomic<std::size_t> Top_{0};
	std::atomic<std::size_t> Bottom_{0};
};

} // namespace AstralDB
