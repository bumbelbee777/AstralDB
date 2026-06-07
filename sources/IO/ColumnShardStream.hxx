#pragma once

#include <IO/ColumnShardConfig.hxx>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace AstralDB {

struct ColumnShardStreamCapabilities {
	bool MmapRead = false;
	bool IoUringAsync = false;
	bool IocpAsync = false;
};

[[nodiscard]] ColumnShardStreamCapabilities ProbeColumnShardStreamCapabilities() noexcept;

/** Column-major shard on disk (mmap / IOCP reads; optional LZX/LZ4 compression). */
class ColumnShardWriter {
public:
	explicit ColumnShardWriter(std::filesystem::path RootDir,
	                           ColumnShardConfig Config = ColumnShardConfig::FromEnvironment());
	~ColumnShardWriter();

	ColumnShardWriter(const ColumnShardWriter &) = delete;
	ColumnShardWriter &operator=(const ColumnShardWriter &) = delete;

	void BeginTable(const std::string &TableName, const std::vector<std::string> &Columns);
	void AppendRow(int64_t RowId, int64_t Step, const std::vector<std::string> &ColNames,
	               const std::vector<std::string> &Values);
	void FinalizeTable();

	[[nodiscard]] std::size_t RowsWritten() const noexcept;
	[[nodiscard]] const std::filesystem::path &Root() const noexcept;
	[[nodiscard]] const ColumnShardConfig &Config() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> Impl_;
};

class ColumnShardReader {
public:
	explicit ColumnShardReader(std::filesystem::path ShardDir,
	                           ColumnShardConfig Config = ColumnShardConfig::FromEnvironment());

	[[nodiscard]] std::size_t RowCount() const noexcept;
	[[nodiscard]] const std::vector<std::string> &ColumnNames() const noexcept;
	[[nodiscard]] std::string Cell(std::size_t RowIndex, const std::string &Column) const;

private:
	struct Impl;
	std::unique_ptr<Impl> Impl_;
};

} // namespace AstralDB
