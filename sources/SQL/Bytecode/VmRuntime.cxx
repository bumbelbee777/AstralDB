#include <SQL/Bytecode/VmRuntime.hxx>

namespace AstralDB {
namespace SQL {

bool VmParallelEnabledFromEnv() {
	if(const char *V = std::getenv("ASTRALDB_VM_PARALLEL")) {
		if(V[0] == '1' || V[0] == 'y' || V[0] == 'Y')
			return true;
	}
	return false;
}

VmCoordinator::VmCoordinator(BytecodeInterpreter & /*Interpreter*/) {
	ParallelEnabled_ = VmParallelEnabledFromEnv() && JobSystem::Instance().IsRunning();
}

void VmCoordinator::ExecuteParallel(std::function<void()> MainThreadWork,
                                    std::vector<std::function<void()>> Partitions) {
	if(!ParallelEnabled_ || Partitions.empty()) {
		MainThreadWork();
		for(auto &P : Partitions)
			P();
		return;
	}
	MainThreadWork();
	for(auto &P : Partitions)
		Pending_.push_back(JobSystem::Instance().SubmitAsync(std::move(P)));
	Drain();
}

void VmCoordinator::Drain() {
	for(auto &F : Pending_)
		F.get();
	Pending_.clear();
}

} // namespace SQL
} // namespace AstralDB
