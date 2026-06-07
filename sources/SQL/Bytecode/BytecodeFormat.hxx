#pragma once

#include <SQL/Bytecode/Bytecode.hxx>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace AstralDB {
namespace SQL {

/** On-disk `.abc` container version (binary layout). Product release stays v1.0. */
static constexpr std::uint32_t kAbcContainerVersion = 1u;

/** Optional trailer magic introducing a deduplicated string operand pool. */
static constexpr std::uint32_t kAbcStringPoolTrailerMagic = 0xABC01001u;

struct LoadedAbcFile {
	Bytecode Instructions;
	std::vector<std::string> StringPool;
	std::uint32_t ContainerVersion = kAbcContainerVersion;
};

LoadedAbcFile LoadAbcFile(const std::filesystem::path &Path);

void SaveAbcFile(const std::filesystem::path &Path, const Bytecode &Code,
                 const std::vector<std::string> *StringPool = nullptr);

void SaveAbcFile(const std::filesystem::path &Path, const CompiledBytecode &Compiled);

} // namespace SQL
} // namespace AstralDB
