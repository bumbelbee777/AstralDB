#pragma once

#include <IO/Job.hxx>

#include <coroutine>
#include <future>
#include <utility>

namespace AstralDB {

template<typename Function, typename... Args>
auto RunAsync(Function &&F, Args &&...Arguments) {
	return JobSystem::Instance().SubmitAsync(
	    [Fn = std::forward<Function>(F), ... Captured = std::forward<Args>(Arguments)]() mutable {
		    return Fn(Captured...);
	    });
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

} // namespace AstralDB
