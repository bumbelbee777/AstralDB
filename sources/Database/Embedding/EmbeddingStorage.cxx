#include <Database/EmbeddingStorage.hxx>

#include <Database/Database.hxx>
#include <Database/MathSciComplex.hxx>
#include <Database/MathSciEmbeddings.hxx>
#include <IO/Error.hxx>

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace AstralDB {
namespace {

constexpr std::string_view kEmbeddingSnapshotMarkerSv = "<<<ASTRAL_DB_EMBEDDINGS>>>\n";

static const char *const kWalB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[noreturn]] void FailEmbeddingStore(std::string Message) {
	throw std::runtime_error(Err::Prefixed("embedding store", std::move(Message)));
}

std::string WalEncodeBlob(std::string_view Plain) {
	std::string Out;
	Out.reserve(((Plain.size() + 2) / 3) * 4);
	uint32_t Acc = 0;
	int Bits = 0;
	for(unsigned char Ch : Plain) {
		Acc = (Acc << 8) | Ch;
		Bits += 8;
		while(Bits >= 6) {
			Bits -= 6;
			Out.push_back(kWalB64[(Acc >> Bits) & 63]);
		}
	}
	if(Bits) {
		Acc <<= (6 - Bits);
		Out.push_back(kWalB64[Acc & 63]);
	}
	while(Out.size() % 4)
		Out.push_back('=');
	return Out;
}

std::string WalDecodeBlob(std::string_view In) {
	std::string Out;
	Out.reserve(In.size() * 3 / 4);
	int Acc = 0;
	int Bits = -8;
	for(unsigned char C : In) {
		if(C == '=')
			break;
		int D = -1;
		if(C >= 'A' && C <= 'Z')
			D = static_cast<int>(C - 'A');
		else if(C >= 'a' && C <= 'z')
			D = static_cast<int>(C - 'a') + 26;
		else if(C >= '0' && C <= '9')
			D = static_cast<int>(C - '0') + 52;
		else if(C == '+')
			D = 62;
		else if(C == '/')
			D = 63;
		if(D < 0)
			continue;
		Acc = (Acc << 6) | D;
		Bits += 6;
		if(Bits >= 0) {
			Out.push_back(static_cast<char>((Acc >> Bits) & 255));
			Bits -= 8;
		}
	}
	return Out;
}

std::string EscapeField(std::string_view S) {
	std::string Out;
	Out.reserve(S.size());
	for(char C : S) {
		if(C == '|' || C == '\n' || C == '\r')
			Out.push_back('_');
		else
			Out.push_back(C);
	}
	return Out;
}

void WriteEntryLine(std::string &Raw, const EmbeddingCatalogEntry &Entry) {
	Raw += EscapeField(Entry.Name);
	Raw.push_back('|');
	Raw += EscapeField(Entry.SourceTable);
	Raw.push_back('|');
	Raw += EscapeField(Entry.TokenColumn);
	Raw.push_back('|');
	Raw += EscapeField(Entry.VectorColumn);
	Raw.push_back('|');
	Raw += Entry.IsComplex ? '1' : '0';
	Raw.push_back('|');
	Raw += WalEncodeBlob(Entry.WireCell);
	Raw.push_back('\n');
}

EmbeddingCatalogEntry ParseEntryLine(std::string_view Line) {
	EmbeddingCatalogEntry Entry;
	std::string_view Rem = Line;
	auto Take = [&]() -> std::string {
		const size_t P = Rem.find('|');
		std::string Out;
		if(P == std::string_view::npos) {
			Out.assign(Rem.begin(), Rem.end());
			Rem = {};
		} else {
			Out.assign(Rem.begin(), Rem.begin() + static_cast<std::ptrdiff_t>(P));
			Rem.remove_prefix(P + 1);
		}
		return Out;
	};
	Entry.Name = Take();
	Entry.SourceTable = Take();
	Entry.TokenColumn = Take();
	Entry.VectorColumn = Take();
	Entry.IsComplex = Take() == "1";
	const std::string WireB64 = Take();
	if(!WireB64.empty())
		Entry.WireCell = WalDecodeBlob(WireB64);
	if(Entry.WireCell.empty() || !MathSciEmbeddings::Deserialize(Entry.WireCell))
		FailEmbeddingStore("corrupt embedding catalog wire cell for \"" + Entry.Name + "\"");
	return Entry;
}

} // namespace

std::string EmbeddingWalLineRegister(const EmbeddingCatalogEntry &Entry) {
	std::ostringstream O;
	O << "ER|" << EscapeField(Entry.Name) << '|' << EscapeField(Entry.SourceTable) << '|'
	  << EscapeField(Entry.TokenColumn) << '|' << EscapeField(Entry.VectorColumn) << '|'
	  << (Entry.IsComplex ? 1 : 0) << '|' << WalEncodeBlob(Entry.WireCell);
	return O.str();
}

std::string EmbeddingWalLineDrop(const std::string &Name) {
	return std::string("ED|") + EscapeField(Name);
}

EmbeddingCatalogEntry ParseEmbeddingWalRegisterTokens(const std::vector<std::string> &Tok) {
	if(Tok.size() < 7)
		FailEmbeddingStore("corrupt WAL embedding register record");
	EmbeddingCatalogEntry Entry;
	Entry.Name = Tok[1];
	Entry.SourceTable = Tok[2];
	Entry.TokenColumn = Tok[3];
	Entry.VectorColumn = Tok[4];
	Entry.IsComplex = Tok[5] == "1";
	Entry.WireCell = WalDecodeBlob(Tok[6]);
	if(Entry.WireCell.empty())
		FailEmbeddingStore("corrupt WAL embedding wire payload for \"" + Entry.Name + "\"");
	return Entry;
}

void InstallEmbeddingCatalogEntryAssumeLocked(Database &Db, EmbeddingCatalogEntry Entry) {
	if(Entry.Name.empty())
		FailEmbeddingStore("embedding name must be non-empty.");
	if(!MathSciEmbeddings::Deserialize(Entry.WireCell))
		FailEmbeddingStore("invalid embedding wire cell for \"" + Entry.Name + "\".");
	Db.Embeddings_[Entry.Name] = std::move(Entry);
}

void InstallEmbeddingCatalogAssumeLocked(Database &Db, std::vector<EmbeddingCatalogEntry> Entries) {
	Db.Embeddings_.clear();
	for(EmbeddingCatalogEntry &Entry : Entries)
		InstallEmbeddingCatalogEntryAssumeLocked(Db, std::move(Entry));
}

void ReplayWalEmbeddingRegisterAssumeLocked(Database &Db, EmbeddingCatalogEntry Entry) {
	InstallEmbeddingCatalogEntryAssumeLocked(Db, std::move(Entry));
}

void ReplayWalEmbeddingDropAssumeLocked(Database &Db, const std::string &Name) {
	Db.Embeddings_.erase(Name);
}

void AppendEmbeddingCatalogSnapshotTrailer(
    std::string &RawData, const std::unordered_map<std::string, EmbeddingCatalogEntry> &Embeddings) {
	if(Embeddings.empty())
		return;
	RawData.append(kEmbeddingSnapshotMarkerSv.data(), kEmbeddingSnapshotMarkerSv.size());
	RawData += std::to_string(Embeddings.size());
	RawData.push_back('\n');
	for(const auto &[_, Entry] : Embeddings)
		WriteEntryLine(RawData, Entry);
}

bool StripAndParseEmbeddingCatalogSnapshotTrailer(std::string &RawData,
                                                  std::vector<EmbeddingCatalogEntry> &OutEntries) {
	OutEntries.clear();
	const size_t Mp = RawData.find(kEmbeddingSnapshotMarkerSv.data(), 0, kEmbeddingSnapshotMarkerSv.size());
	if(Mp == std::string_view::npos)
		return true;
	std::string_view Tail(RawData.data() + Mp + kEmbeddingSnapshotMarkerSv.size(),
	                     RawData.size() - Mp - kEmbeddingSnapshotMarkerSv.size());
	const size_t NL = Tail.find('\n');
	if(NL == std::string_view::npos)
		return false;
	std::uint64_t NV = 0;
	for(unsigned char Ch : Tail.substr(0, NL)) {
		if(Ch < '0' || Ch > '9')
			return false;
		NV = NV * 10 + static_cast<unsigned>(Ch - '0');
	}
	Tail = Tail.substr(NL + 1);
	OutEntries.reserve(static_cast<size_t>(NV));
	for(std::uint64_t I = 0; I < NV; ++I) {
		const size_t Ln = Tail.find('\n');
		if(Ln == std::string_view::npos)
			return false;
		OutEntries.push_back(ParseEntryLine(Tail.substr(0, Ln)));
		Tail = Tail.substr(Ln + 1);
	}
	RawData.erase(Mp);
	return true;
}

EmbeddingCatalogEntry BuildEmbeddingCatalogEntryFromTable(Database &Db, const std::string &Name,
                                                          const std::string &TableName,
                                                          const std::string &TokenColumn,
                                                          const std::string &VectorColumn) {
	if(Db.Embeddings_.contains(Name))
		FailEmbeddingStore("CREATE EMBEDDING: embedding \"" + Name + "\" already exists.");
	const auto SchIt = Db.TableSchemas_.find(TableName);
	if(SchIt == Db.TableSchemas_.end())
		FailEmbeddingStore("CREATE EMBEDDING: source table \"" + TableName + "\" does not exist.");
	bool TokColFound = false;
	bool VecColFound = false;
	for(const Database::Column &Co : SchIt->second) {
		if(Co.Name == TokenColumn)
			TokColFound = true;
		if(Co.Name == VectorColumn)
			VecColFound = true;
	}
	if(!TokColFound)
		FailEmbeddingStore("CREATE EMBEDDING: column \"" + TokenColumn + "\" not found on table \"" + TableName +
		                   "\".");
	if(!VecColFound)
		FailEmbeddingStore("CREATE EMBEDDING: column \"" + VectorColumn + "\" not found on table \"" + TableName +
		                   "\".");
	const auto TblIt = Db.Tables_.find(TableName);
	if(TblIt == Db.Tables_.end())
		FailEmbeddingStore("CREATE EMBEDDING: table \"" + TableName + "\" has no row storage.");
	const Database::Table &Live = TblIt->second.RowsForRead(std::nullopt, false);
	std::vector<std::string> Tokens;
	std::vector<std::vector<double>> Vectors;
	bool IsComplex = false;
	bool ComplexSet = false;
	Tokens.reserve(Live.size());
	Vectors.reserve(Live.size());
	for(const Database::Item &Row : Live) {
		const auto TokIt = Row.find(TokenColumn);
		const auto VecIt = Row.find(VectorColumn);
		if(TokIt == Row.end())
			FailEmbeddingStore("CREATE EMBEDDING: row missing token column \"" + TokenColumn + "\".");
		if(VecIt == Row.end())
			FailEmbeddingStore("CREATE EMBEDDING: row missing vector column \"" + VectorColumn + "\".");
		const std::string &Tok = TokIt->second;
		if(Tok.empty())
			FailEmbeddingStore("CREATE EMBEDDING: empty token in column \"" + TokenColumn + "\".");
		if(std::find(Tokens.begin(), Tokens.end(), Tok) != Tokens.end())
			FailEmbeddingStore("CREATE EMBEDDING: duplicate token \"" + Tok + "\".");
		const auto Nv = MathSciComplex::ParseNumericVec(VecIt->second);
		if(!Nv)
			FailEmbeddingStore("CREATE EMBEDDING: invalid vector cell for token \"" + Tok + "\".");
		const bool RowComplex = Nv->IsComplex();
		if(!ComplexSet) {
			IsComplex = RowComplex;
			ComplexSet = true;
		} else if(RowComplex != IsComplex) {
			FailEmbeddingStore("CREATE EMBEDDING: mixed real and complex vectors in \"" + VectorColumn + "\".");
		}
		Tokens.push_back(Tok);
		Vectors.emplace_back(Nv->Values.begin(), Nv->Values.end());
	}
	if(Tokens.empty())
		FailEmbeddingStore("CREATE EMBEDDING: source table \"" + TableName + "\" has no rows.");
	const auto Built = MathSciEmbeddings::BuildFromTokensAndVectors(Tokens, Vectors, IsComplex);
	if(!Built)
		FailEmbeddingStore("CREATE EMBEDDING: failed to build embedding table from \"" + TableName + "\".");
	EmbeddingCatalogEntry Entry;
	Entry.Name = Name;
	Entry.SourceTable = TableName;
	Entry.TokenColumn = TokenColumn;
	Entry.VectorColumn = VectorColumn;
	Entry.IsComplex = IsComplex;
	Entry.WireCell = MathSciEmbeddings::Serialize(*Built);
	return Entry;
}

} // namespace AstralDB
