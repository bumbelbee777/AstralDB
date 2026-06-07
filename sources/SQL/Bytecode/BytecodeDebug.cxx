#include <SQL/BytecodeDebug.hxx>
#include <SQL/BytecodeInspect.hxx>
#include <algorithm>
#include <iostream>

namespace AstralDB {
namespace SQL {

namespace {

class CompositeVmDebugListener final : public VmDebugListener {
public:
	CompositeVmDebugListener(std::ostream &TraceOut, const VmDebugConfig &Config) : TraceOut_(TraceOut), Config_(Config) {}

	bool OnBeforeStep(const VmTraceEvent &Event) override {
		if(Config_.MaxTraceSteps > 0 && Event.StepNumber > Config_.MaxTraceSteps)
			return false;
		if(!Config_.BreakpointIps.empty()) {
			const bool Hit =
			    std::find(Config_.BreakpointIps.begin(), Config_.BreakpointIps.end(), Event.Ip) !=
			    Config_.BreakpointIps.end();
			if(Hit && !Config_.TraceToStderr)
				TraceOut_ << "[breakpoint] ip=" << Event.Ip << "\n";
		}
		if(Config_.TraceToStderr) {
			TraceOut_ << "step=" << Event.StepNumber << " ip=" << Event.Ip << " op=" << OpcodeName(Event.Op)
			          << " stack=" << Event.StackDepth << "\n";
		}
		return true;
	}

private:
	std::ostream &TraceOut_;
	VmDebugConfig Config_;
};

} // namespace

std::unique_ptr<VmDebugListener> MakeVmDebugListener(std::ostream &TraceOut, const VmDebugConfig &Config) {
	return std::make_unique<CompositeVmDebugListener>(TraceOut, Config);
}

VmDebugSession::VmDebugSession(VmDebugConfig Config) : Config_(std::move(Config)) {}

void VmDebugSession::AttachListener(std::unique_ptr<VmDebugListener> Listener) {
	Listener_ = std::move(Listener);
}

void VmDebugSession::NotifyBeforeStep(const VmTraceEvent &Event) {
	++Report_.StepsExecuted;
	if(Event.Ip < Report_.InstructionsVisited || Report_.InstructionsVisited == 0)
		Report_.InstructionsVisited = Event.Ip + 1;
	else if(Event.Ip + 1 > Report_.InstructionsVisited)
		Report_.InstructionsVisited = Event.Ip + 1;
	if(Listener_ && !Listener_->OnBeforeStep(Event)) {
		Report_.HaltedEarly = true;
		Report_.HaltedAtIp = Event.Ip;
		if(Report_.HaltReason.empty())
			Report_.HaltReason = "debug listener halted execution";
	}
}

void VmDebugSession::NotifyCompleted() {
	Report_.Completed = !Report_.HaltedEarly;
}

void VmDebugSession::NotifyHalted(std::string Reason) {
	Report_.HaltedEarly = true;
	Report_.HaltReason = std::move(Reason);
}

} // namespace SQL
} // namespace AstralDB
