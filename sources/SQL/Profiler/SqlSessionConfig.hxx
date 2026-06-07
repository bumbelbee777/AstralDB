#pragma once

#include <SQL/Bytecode/Bytecode.hxx>

#include <cstdint>
#include <optional>
#include <string>

namespace AstralDB {
namespace SQL {

struct SqlSessionConfig {
	std::optional<std::string> ProfileName;
	std::optional<std::string> RegionName;
	std::uint32_t VectorBatchSize = 1024;
	enum class JoinStrategy : std::uint8_t { Auto, Radix, Hash };
	JoinStrategy JoinStrategy_ = JoinStrategy::Auto;
	bool ZoneMapAdaptive = true;
	bool LazyMaterialization = true;
	enum class VmChoice : std::uint8_t { Auto, Dbvm, Silly, Jit };
	VmChoice Vm = VmChoice::Auto;
	OptimizationLevel OptLevel = OptimizationLevel::Advanced;
	bool VerifyFastPath = false;
	enum class ExplainMode : std::uint8_t { Off, On, Analyze };
	ExplainMode Explain = ExplainMode::Off;
	std::optional<std::string> ProfileDumpPath;
	double OptConfidenceFloor = 0.95;
	bool UsePrecomputed = true;
	bool JitEnabled = true;
	bool ForeignKeys = true;
	bool QueryCheckpointOnSignal = false;
	std::size_t QueryCheckpointInterval = 0;
	bool ResumeQueryCheckpoint = false;

	static SqlSessionConfig FromEnvironment();
};

} // namespace SQL
} // namespace AstralDB
