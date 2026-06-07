#pragma once

#include <string>

namespace AstralDB {

struct EmbeddingCatalogEntry {
	std::string Name;
	std::string SourceTable;
	std::string TokenColumn;
	std::string VectorColumn;
	std::string WireCell;
	bool IsComplex = false;
};

} // namespace AstralDB
