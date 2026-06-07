#pragma once

#include <IO/Spinlock.hxx>

#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

namespace AstralDB {

template<class F>
struct Job {
	F Task_;

	Job() = default;
	explicit Job(F T) : Task_(std::move(T)) {}

	void Execute() { Task_(); }

	explicit operator bool() const { return static_cast<bool>(Task_); }
};

template<typename R>
struct AsyncJob {
	std::function<R()> Task_;
	std::shared_ptr<std::promise<R>> Promise_;

	explicit AsyncJob(std::function<R()> T)
	    : Task_(std::move(T)), Promise_(std::make_shared<std::promise<R>>()) {}

	std::future<R> Future() { return Promise_->get_future(); }

	void Execute() {
		try {
			if constexpr(std::is_void_v<R>) {
				Task_();
				Promise_->set_value();
			} else {
				Promise_->set_value(Task_());
			}
		} catch(...) {
			Promise_->set_exception(std::current_exception());
		}
	}
};

template<class JobType>
struct ThreadJobQueue {
	std::deque<JobType> Queue_;
	Spinlock Lock_;

	ThreadJobQueue() = default;
	ThreadJobQueue(const ThreadJobQueue &) = delete;
	ThreadJobQueue &operator=(const ThreadJobQueue &) = delete;
	ThreadJobQueue(ThreadJobQueue &&) noexcept = default;
	ThreadJobQueue &operator=(ThreadJobQueue &&) noexcept = default;

	void PushLocal(JobType &&Job) {
		SpinlockGuard G(Lock_);
		Queue_.push_back(std::move(Job));
	}

	bool PopLocal(JobType &OutJob) {
		SpinlockGuard G(Lock_);
		if(Queue_.empty())
			return false;
		OutJob = std::move(Queue_.back());
		Queue_.pop_back();
		return true;
	}

	bool Steal(JobType &OutJob) {
		SpinlockGuard G(Lock_);
		if(Queue_.empty())
			return false;
		OutJob = std::move(Queue_.front());
		Queue_.pop_front();
		return true;
	}
};

class JobSystem {
public:
	static JobSystem &Instance();

	void Initialize(unsigned int WorkerCount = 0);
	void Shutdown();
	bool IsRunning() const;

	template<typename F>
	void Submit(F &&Task) {
		using VoidJob = Job<std::function<void()>>;
		if(!Running_.load(std::memory_order_acquire) || Queues_.empty())
			return;
		const auto Idx = NextQueueIndex_.fetch_add(1, std::memory_order_relaxed) % Queues_.size();
		Queues_[Idx]->PushLocal(VoidJob(std::function<void()>(std::forward<F>(Task))));
	}

	template<typename F>
	auto SubmitAsync(F &&Task) -> std::future<decltype(Task())> {
		using R = decltype(Task());
		if(!Running_.load(std::memory_order_acquire) || Queues_.empty()) {
			std::promise<R> P;
			if constexpr(std::is_void_v<R>)
				P.set_value();
			else
				P.set_value(Task());
			return P.get_future();
		}
		AsyncJob<R> Aj(std::function<R()>(std::forward<F>(Task)));
		auto Fut = Aj.Future();
		const auto Idx = NextQueueIndex_.fetch_add(1, std::memory_order_relaxed) % Queues_.size();
		Queues_[Idx]->PushLocal(Job<std::function<void()>>([Job = std::move(Aj)]() mutable { Job.Execute(); }));
		return Fut;
	}

private:
	JobSystem() = default;
	~JobSystem();
	JobSystem(const JobSystem &) = delete;
	JobSystem &operator=(const JobSystem &) = delete;

	void WorkerLoop(unsigned int ThreadIndex);

	std::vector<std::thread> Workers_;
	std::vector<std::unique_ptr<ThreadJobQueue<Job<std::function<void()>>>>> Queues_;
	std::atomic<std::size_t> NextQueueIndex_{0};
	std::atomic<bool> Running_{false};
};

unsigned int JobWorkerCountFromEnv();

} // namespace AstralDB
