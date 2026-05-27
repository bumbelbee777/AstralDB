#pragma once

#include <cstddef>
#include <list>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AstralDB {
class Database;

namespace MathSciEmbeddings {

constexpr std::size_t MaxEmbeddingDim = 4096;
constexpr std::size_t MaxEmbeddingVocab = 1u << 20;
constexpr std::size_t MaxEmbeddingCacheEntries = 4096;
constexpr std::size_t MaxEditDistLen = 4096;

struct EmbeddingTable {
	bool IsComplex = false;
	std::size_t Dim = 0;
	std::vector<float> UnkVector;
	std::vector<std::string> Tokens;
	std::unordered_map<std::string, std::size_t> TokenToRow;
	std::vector<std::vector<float>> Rows;
	std::vector<float> RowsFlat;
};

struct EmbeddingCache {
	explicit EmbeddingCache(std::size_t Capacity = MaxEmbeddingCacheEntries);

	std::optional<std::vector<float>> Lookup(std::string_view Token) const;
	void Insert(std::string Token, std::vector<float> Vec);

private:
	std::size_t Capacity_;
	using Entry = std::pair<std::string, std::vector<float>>;
	using EntryList = std::list<Entry>;
	std::unordered_map<std::string, EntryList::iterator> Map_;
	EntryList Order_;
};

std::optional<EmbeddingTable> Deserialize(std::string_view Cell);
std::string Serialize(const EmbeddingTable &Table);
std::string Fingerprint(const EmbeddingTable &Table);

std::optional<EmbeddingTable> BuildFromTokensAndMatrix(const std::vector<std::string> &Tokens,
                                                       const std::vector<double> &MatrixFlat, std::size_t Rows,
                                                       std::size_t Cols, bool IsComplex);
std::optional<EmbeddingTable> BuildFromTokensAndVectors(const std::vector<std::string> &Tokens,
                                                        const std::vector<std::vector<double>> &Vectors, bool IsComplex);

std::vector<float> LookupVector(const EmbeddingTable &Table, std::string_view Token);
std::vector<std::vector<double>> BatchLookup(const EmbeddingTable &Table, const std::vector<std::string> &Tokens);
std::vector<double> MeanPool(const EmbeddingTable &Table, const std::vector<std::string> &Tokens);

std::optional<std::string> ResolveTableCell(std::string_view Ref, Database *Db);
std::optional<std::string> BuildCellFromReal(const std::string &TokenListCell, const std::string &VectorCell);
std::optional<std::string> LookupCellFromReal(const std::string &TableCell, std::string_view Token, Database *Db);
std::optional<std::string> BatchCellFromReal(const std::string &TableCell, const std::string &TokenListCell,
                                              Database *Db);
std::optional<std::string> SerializeCellFromReal(const std::string &TableCell);
std::optional<std::string> LoadCellFromReal(const std::string &TableCell);
std::optional<std::string> FingerprintCellFromReal(const std::string &TableCell);
std::optional<std::string> MeanCellFromReal(const std::string &TokenListCell, const std::string &TableCell,
                                            Database *Db);

} // namespace MathSciEmbeddings
} // namespace AstralDB
