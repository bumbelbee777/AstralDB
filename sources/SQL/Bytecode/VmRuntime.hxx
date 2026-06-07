#pragma once

#include <IO/Job.hxx>
#include <SQL/Bytecode/BytecodeInterpreter.hxx>

#include <cstdlib>
#include <functional>
#include <future>
#include <vector>

namespace AstralDB {
namespace SQL {

/** Parallel VM coordinator: main thread runs sequential ops; workers steal partition units. */
class VmCoordinator {
public:
	explicit VmCoordinator(BytecodeInterpreter &Interpreter);

	[[nodiscard]] bool ParallelEnabled() const noexcept { return ParallelEnabled_; }
	void ExecuteParallel(std::function<void()> MainThreadWork, std::vector<std::function<void()>> Partitions);
	void Drain();

private:
	bool ParallelEnabled_ = false;
	std::vector<std::future<void>> Pending_;
};

bool VmParallelEnabledFromEnv();

} // namespace SQL
} // namespace AstralDB
