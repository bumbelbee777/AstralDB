#pragma once

#include <atomic>
#include <functional>
#include <thread>

namespace AstralDB {
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
}