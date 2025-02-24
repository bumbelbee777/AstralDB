#pragma once

#include <future>
#include <coroutine>

namespace AstralDB {
template <typename Function, typename... Args> auto RunAsync(Function&& f, Args&&... args) {
    return std::async(std::launch::async, std::forward<Function>(f), std::forward<Args>(args)...);
}

struct Task {
    struct promise_type {
        Task get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
};
}