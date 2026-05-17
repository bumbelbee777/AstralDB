#pragma once

#include <SQL/Bytecode.hxx>
#include <cstddef>
#include <iosfwd>
#include <memory>
#include <vector>

namespace AstralDB {
namespace SQL {

struct VmTraceEvent {
	std::size_t Ip = 0;
	Opcode Op = Opcode::NOP;
	std::size_t StackDepth = 0;
	std::size_t StepNumber = 0;
};

/** VM invokes this before each instruction when debugging is enabled. Return false to halt early. */
class VmDebugListener {
public:
	virtual ~VmDebugListener() = default;
	virtual bool OnBeforeStep(const VmTraceEvent &Event) = 0;
};

struct VmDebugConfig {
	bool TraceToStderr = false;
	std::size_t MaxTraceSteps = 0; /** 0 = unlimited */
	std::vector<std::size_t> BreakpointIps;
	bool HaltOnSideEffect = false;
};

std::unique_ptr<VmDebugListener> MakeVmDebugListener(std::ostream &TraceOut, const VmDebugConfig &Config);

struct VmDebugReport {
	std::size_t StepsExecuted = 0;
	std::size_t InstructionsVisited = 0;
	std::size_t HaltedAtIp = 0;
	bool Completed = false;
	bool HaltedEarly = false;
	std::string HaltReason;
};

class VmDebugSession {
public:
	explicit VmDebugSession(VmDebugConfig Config);

	void AttachListener(std::unique_ptr<VmDebugListener> Listener);
	VmDebugListener *Listener() const { return Listener_.get(); }

	const VmDebugConfig &Config() const { return Config_; }
	VmDebugReport &Report() { return Report_; }
	const VmDebugReport &Report() const { return Report_; }

	void NotifyBeforeStep(const VmTraceEvent &Event);
	void NotifyCompleted();
	void NotifyHalted(std::string Reason);

private:
	VmDebugConfig Config_;
	std::unique_ptr<VmDebugListener> Listener_;
	VmDebugReport Report_;
};

} // namespace SQL
} // namespace AstralDB
