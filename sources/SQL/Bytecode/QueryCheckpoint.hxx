#pragma once

#include <SQL/Bytecode/Bytecode.hxx>
#include <SQL/Bytecode/BytecodeInterpreter.hxx>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace AstralDB {
namespace SQL {

/** Fused-executor resume slice (warehouse megafusion phases). */
struct DominantExecutorCheckpoint {
	static constexpr const char *kWarehouseMegafusionId = "warehouse_megafusion";

	std::string ExecutorId;
	std::uint32_t PhaseIndex = 0;
	std::uint64_t RowCursor = 0;
	std::uint64_t PartialScanned = 0;
	std::size_t PartialResultRows = 0;
};

/** On-disk query execution checkpoint (encrypted). */
struct QueryCheckpointPayload {
	std::array<std::uint8_t, 32> BytecodeDigest{};
	std::filesystem::path DatabasePath;
	std::size_t InstructionPointer = 0;
	std::size_t StackPointer = 0;
	std::size_t BasePointer = 0;
	std::uint32_t Flags = 0;
	std::vector<std::uint64_t> Registers;
	std::vector<std::uint64_t> StackWords;
	std::size_t StepsExecuted = 0;
	BytecodeInterpreter::TimeSqlStats PartialStats{};
	std::optional<DominantExecutorCheckpoint> Dominant;
	bool ExecutionCompleted = false;
};

/** Global signal-driven checkpoint request (Program.cxx). */
extern std::atomic<bool> gRequestQueryCheckpoint;

[[nodiscard]] std::array<std::uint8_t, 32> HashBytecodeDigest(const Bytecode &Code);

[[nodiscard]] std::filesystem::path DefaultQueryCheckpointPath(const std::filesystem::path &DbPath);

void SaveQueryCheckpoint(const QueryCheckpointPayload &Payload, const std::filesystem::path &Path);

[[nodiscard]] std::optional<QueryCheckpointPayload> LoadQueryCheckpoint(const std::filesystem::path &Path);

/** Persist VM + optional dominant state; returns false if no checkpoint was requested. */
[[nodiscard]] bool MaybeSaveQueryCheckpoint(BytecodeInterpreter &Vm, const Bytecode &Code,
                                            const DominantExecutorCheckpoint *Dominant = nullptr);

[[nodiscard]] bool ResumeQueryCheckpoint(BytecodeInterpreter &Vm, const Bytecode &Code,
                                       const QueryCheckpointPayload &Loaded);

void ClearDominantExecutorCheckpoint() noexcept;
[[nodiscard]] std::optional<DominantExecutorCheckpoint> GetDominantExecutorCheckpoint() noexcept;
void SetDominantExecutorCheckpoint(DominantExecutorCheckpoint State) noexcept;

[[nodiscard]] std::size_t QueryCheckpointIntervalSteps() noexcept;
[[nodiscard]] bool QueryCheckpointOnSignalEnabled() noexcept;

} // namespace SQL
} // namespace AstralDB
