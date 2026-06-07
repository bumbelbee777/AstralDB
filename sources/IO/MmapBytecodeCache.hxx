#pragma once

#include <SQL/Bytecode/BytecodeFormat.hxx>

#include <filesystem>
#include <functional>
#include <string>

namespace AstralDB {
namespace SQL {

/** mmap-backed `.abc` bytecode cache keyed by SQL text hash. */
class MmapBytecodeCache {
public:
	explicit MmapBytecodeCache(std::filesystem::path CacheDir);

	LoadedAbcFile LoadOrCompile(const std::string &Sql, const std::function<CompiledBytecode()> &CompileFn);
	void Invalidate(const std::string &Sql);
	[[nodiscard]] bool LastLoadWasCacheHit() const noexcept { return LastHit_; }

private:
	[[nodiscard]] std::filesystem::path PathForSql(const std::string &Sql) const;
	[[nodiscard]] LoadedAbcFile LoadMapped(const std::filesystem::path &Path) const;

	std::filesystem::path CacheDir_;
	bool LastHit_ = false;
};

LoadedAbcFile LoadAbcMapped(const std::filesystem::path &Path);

} // namespace SQL
} // namespace AstralDB
