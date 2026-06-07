#pragma once

#include <Database/Storage/BulkSyntheticSemistructured.hxx>
#include <Database/Storage/ColumnarStorage.hxx>
#include <Database/Database.hxx>
#include <SQL/SemistructuredBytecode.hxx>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace AstralDB {
namespace SQL {

class SemistructuredVM {
public:
	bool Execute(const SSProgram &Prog, Database &Db, const std::string &Table, const BulkWhereDnf &FilterDnf,
	             const std::vector<SemistructuredProjectionSpec> &Projections, const std::string &OrderCol,
	             bool OrderAscending, std::size_t Limit, RowTable &Out, std::uint64_t *RowsScannedOut) noexcept;

private:
	void ExecuteFused(const SSProgram &Prog, const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
	                  const BulkWhereDnf &FilterDnf, const std::vector<SemistructuredProjectionSpec> &Projections,
	                  const std::string &OrderCol, bool OrderAscending, std::size_t Limit, std::size_t StartRow,
	                  std::size_t EndRow);

	void ExecuteParallel(const SSProgram &Prog, const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
	                     const BulkWhereDnf &FilterDnf, const std::vector<SemistructuredProjectionSpec> &Projections,
	                     const std::string &OrderCol, bool OrderAscending, std::size_t Limit);

	void ProcessBatch(const SSProgram &Prog, const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
	                  const BulkWhereDnf &FilterDnf, std::size_t BatchStart, std::size_t BatchEnd);

	void ApplyPredicatesBatch(const SSProgram &Prog, const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
	                          const BulkWhereDnf &FilterDnf, std::size_t BatchStart, std::size_t BatchEnd,
	                          std::uint8_t *Mask);

	void FillRankBatch(const ColumnarTable &Col, const SemistructuredProjectionSpec *OrderSpec, std::size_t BatchStart,
	                   std::size_t BatchEnd, const std::uint8_t *Mask, std::vector<float> &Ranks,
	                   std::vector<std::uint64_t> &RowIds);

	void TopKInsertBatch(const float *Ranks, const std::uint64_t *Ids, std::size_t N);
	void MaterializeBatch(const ColumnarTable &Col, const std::vector<Database::Column> &Schema,
	                      const std::vector<SemistructuredProjectionSpec> &Projections, RowTable &Out);
	void MergeTopKHeaps();

	std::vector<std::uint8_t> PredicateMasks_;
	std::vector<float> Ranks_;
	std::vector<std::uint64_t> RowIds_;
	std::vector<std::size_t> PassingRows_;
	std::vector<std::pair<float, std::uint64_t>> TopKHeap_;
	std::vector<std::vector<std::pair<float, std::uint64_t>>> WorkerTopK_;
	std::size_t TopK_ = 0;
	bool OrderAscending_ = false;
	std::size_t BatchSize_ = 1024;
	const SemistructuredProjectionSpec *OrderSpec_ = nullptr;
};

[[nodiscard]] bool EnvSemistructuredVmEnabled() noexcept;

} // namespace SQL
} // namespace AstralDB
