#include <IO/Job.hxx>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>

namespace AstralDB {

unsigned int JobWorkerCountFromEnv() {
	if(const char *Env = std::getenv("ASTRALDB_JOB_WORKERS")) {
		const int N = std::atoi(Env);
		if(N > 0)
			return static_cast<unsigned int>(N);
	}
	const unsigned Hw = std::thread::hardware_concurrency();
	return Hw > 0 ? Hw : 4u;
}

JobSystem &JobSystem::Instance() {
	static JobSystem Instance;
	return Instance;
}

JobSystem::~JobSystem() { Shutdown(); }

void JobSystem::Initialize(unsigned int WorkerCount) {
	if(Running_.load(std::memory_order_acquire))
		return;
	if(WorkerCount == 0)
		WorkerCount = JobWorkerCountFromEnv();
	WorkerCount = (std::max)(1u, WorkerCount);
	Running_.store(true, std::memory_order_release);
	Queues_.reserve(WorkerCount);
	for(unsigned int I = 0; I < WorkerCount; ++I) {
		Queues_.push_back(std::make_unique<ThreadJobQueue<Job<std::function<void()>>>>());
		Workers_.emplace_back([this, I] { WorkerLoop(I); });
	}
}

void JobSystem::Shutdown() {
	if(!Running_.load(std::memory_order_acquire) && Workers_.empty())
		return;
	Running_.store(false, std::memory_order_release);
	for(auto &Worker : Workers_) {
		if(Worker.joinable())
			Worker.join();
	}
	Workers_.clear();
	Queues_.clear();
}

bool JobSystem::IsRunning() const { return Running_.load(std::memory_order_acquire); }

void JobSystem::WorkerLoop(unsigned int ThreadIndex) {
	const std::size_t QueueIndex = ThreadIndex % Queues_.size();
	unsigned StealAttempts = 0;

	while(Running_.load(std::memory_order_acquire)) {
		Job<std::function<void()>> Work;
		bool Found = false;

		if(!Queues_.empty()) {
			if(Queues_[QueueIndex]->PopLocal(Work)) {
				Found = true;
				StealAttempts = 0;
			} else {
				const std::size_t Start = (QueueIndex + 1) % Queues_.size();
				for(std::size_t I = 0; I + 1 < Queues_.size(); ++I) {
					const std::size_t StealIndex = (Start + I) % Queues_.size();
					if(Queues_[StealIndex]->Steal(Work)) {
						Found = true;
						StealAttempts = 0;
						break;
					}
				}
			}
		}

		if(Found && Work) {
			Work.Execute();
		} else {
			++StealAttempts;
			if(StealAttempts < 4) {
				std::this_thread::yield();
			} else if(StealAttempts < 16) {
				std::this_thread::sleep_for(std::chrono::microseconds(1));
			} else {
				std::this_thread::sleep_for(std::chrono::microseconds(10));
				StealAttempts = 0;
			}
		}
	}
}

} // namespace AstralDB
