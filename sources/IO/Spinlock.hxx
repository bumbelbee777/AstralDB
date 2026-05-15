#pragma once

#include <IO/Limits.hxx>
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace AstralDB {

namespace Detail {
inline void YieldForContention(unsigned &Spins) noexcept {
	if(++Spins > Limits::SpinYieldSpinsThreshold) {
		std::this_thread::yield();
		Spins = 0;
	}
}
} // namespace Detail

/** Atomic word reader–writer lock (no STL); compatible with \c std::scoped_lock / \c std::shared_lock. */
class SharedMutex {
	std::atomic<int32_t> State_{0};

public:
	void lock() noexcept {
		unsigned Spins = 0;
		for(;;) {
			int32_t St = State_.load(std::memory_order_relaxed);
			if(St != 0) {
				Detail::YieldForContention(Spins);
				continue;
			}
			if(State_.compare_exchange_weak(St, -1, std::memory_order_acquire, std::memory_order_relaxed))
				return;
		}
	}

	void unlock() noexcept { State_.store(0, std::memory_order_release); }

	bool try_lock() noexcept {
		int32_t St = 0;
		return State_.compare_exchange_strong(St, -1, std::memory_order_acquire, std::memory_order_relaxed);
	}

	void lock_shared() noexcept {
		unsigned Spins = 0;
		for(;;) {
			int32_t St = State_.load(std::memory_order_relaxed);
			if(St < 0) {
				Detail::YieldForContention(Spins);
				continue;
			}
			if(State_.compare_exchange_weak(St, St + 1, std::memory_order_acquire, std::memory_order_relaxed))
				return;
		}
	}

	void unlock_shared() noexcept { State_.fetch_sub(1, std::memory_order_release); }

	bool try_lock_shared() noexcept {
		int32_t St = State_.load(std::memory_order_relaxed);
		for(;;) {
			if(St < 0)
				return false;
			if(State_.compare_exchange_weak(St, St + 1, std::memory_order_acquire, std::memory_order_relaxed))
				return true;
		}
	}
};

class Spinlock {
    std::atomic_flag Locked_ = ATOMIC_FLAG_INIT;

public:
    void Lock() {  
        while(Locked_.test_and_set(std::memory_order_acquire)) std::this_thread::yield();
    }

    void Unlock() { 
        Locked_.clear(std::memory_order_release); 
    }

    bool Locked() const { 
        return Locked_.test(std::memory_order_relaxed); 
    }

    template<class ReturnType, class... Args> 
    void OnUnlock(std::function<ReturnType(Args...)> Callback, Args... Arguments) {
        while(Locked()) std::this_thread::yield();
        Callback(std::forward<Args>(Arguments)...);
    }
};

class SpinlockGuard {
    Spinlock& Lock_;

public:
    explicit SpinlockGuard(Spinlock& Lock) : Lock_(Lock) { 
        Lock_.Lock(); 
    }

    ~SpinlockGuard() { 
        Lock_.Unlock(); 
    }

    bool Locked() const { 
        return Lock_.Locked(); 
    }

    template<class ReturnType, class... Args> void OnUnlock(std::function<ReturnType(Args...)> Callback, Args... Arguments) {
        Lock_.OnUnlock(Callback, std::forward<Args>(Arguments)...);
    }
};

class Mutex {
    Spinlock Lock_;
public:
    void Lock() { Lock_.Lock(); }
    void Unlock() { Lock_.Unlock(); }
    void lock() { Lock_.Lock(); }
    void unlock() { Lock_.Unlock(); }

    bool Locked() const { return Lock_.Locked(); }

    template<class ReturnType, class... Args> void OnUnlock(std::function<ReturnType(Args...)> Callback, Args... Arguments) {
        Lock_.OnUnlock(Callback, std::forward<Args>(Arguments)...);
    }
};
}