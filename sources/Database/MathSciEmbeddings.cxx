#include <Database/MathSciEmbeddings.hxx>

#include <Database/AdvancedTypes.hxx>
#include <Database/Database.hxx>
#include <Database/MathSciComplex.hxx>
#include <IO/SIMD.hxx>

#include <bit>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string_view>

namespace AstralDB {
namespace MathSciEmbeddings {
namespace {

std::optional<double> ParseFloat(std::string_view S) {
	if(S.empty())
		return std::nullopt;
	try {
		return std::stod(std::string(S));
	} catch(...) {
		return std::nullopt;
	}
}

void FlattenRows(EmbeddingTable &Table) {
	const std::size_t Width = Table.IsComplex ? Table.Dim * 2 : Table.Dim;
	if(Table.Dim == 0 || Table.Rows.empty()) {
		Table.RowsFlat.clear();
		return;
	}
	Table.RowsFlat.resize(Table.Rows.size() * Width);
	for(std::size_t R = 0; R < Table.Rows.size(); ++R) {
		const auto &Row = Table.Rows[R];
		if(Row.size() != Width)
			continue;
		Simd::Memcpy(Table.RowsFlat.data() + R * Width, Row.data(), Width * sizeof(float));
	}
}

static std::size_t RowWidth(const EmbeddingTable &Table) { return Table.IsComplex ? Table.Dim * 2 : Table.Dim; }

std::optional<std::vector<double>> ParseRowVec(std::string_view Body, std::size_t Dim) {
	std::vector<double> Out;
	Out.reserve(Dim);
	size_t Pos = 0;
	while(Pos < Body.size() && Out.size() < Dim) {
		size_t End = Pos;
		while(End < Body.size() && Body[End] != ',')
			++End;
		const auto N = ParseFloat(Body.substr(Pos, End - Pos));
		if(!N)
			return std::nullopt;
		Out.push_back(*N);
		Pos = End + (End < Body.size() ? 1 : 0);
	}
	if(Out.size() != Dim)
		return std::nullopt;
	return Out;
}

} // namespace

EmbeddingCache::EmbeddingCache(std::size_t Capacity) : Capacity_(Capacity > 0 ? Capacity : 1) {}

std::optional<std::vector<float>> EmbeddingCache::Lookup(std::string_view Token) const {
	const auto It = Map_.find(std::string(Token));
	if(It == Map_.end())
		return std::nullopt;
	return It->second->second;
}

void EmbeddingCache::Insert(std::string Token, std::vector<float> Vec) {
	const auto It = Map_.find(Token);
	if(It != Map_.end()) {
		It->second->second = std::move(Vec);
		Order_.splice(Order_.begin(), Order_, It->second);
		return;
	}
	if(Order_.size() >= Capacity_) {
		const auto &Back = Order_.back();
		Map_.erase(Back.first);
		Order_.pop_back();
	}
	Order_.emplace_front(Token, std::move(Vec));
	Map_[Order_.front().first] = Order_.begin();
}

std::optional<EmbeddingTable> Deserialize(std::string_view Cell) {
	const bool Complex = Cell.size() >= 3 && Cell[0] == 'E' && Cell[1] == 'C' && Cell[2] == '[';
	const bool Real = Cell.size() >= 2 && Cell[0] == 'E' && Cell[1] == '[';
	if(!Complex && !Real)
		return std::nullopt;
	const std::size_t HeadOff = Complex ? 3 : 2;
	const std::size_t Close = Cell.find(']');
	if(Close == std::string_view::npos || Close + 2 >= Cell.size() || Cell[Close + 1] != ':')
		return std::nullopt;
	const std::string_view Head = Cell.substr(HeadOff, Close - HeadOff);
	const std::size_t Comma = Head.find(',');
	if(Comma == std::string_view::npos)
		return std::nullopt;
	const auto DimOpt = ParseFloat(Head.substr(0, Comma));
	const auto CountOpt = ParseFloat(Head.substr(Comma + 1));
	if(!DimOpt || !CountOpt || *DimOpt <= 0 || *CountOpt < 0)
		return std::nullopt;
	const std::size_t Dim = static_cast<std::size_t>(*DimOpt);
	const std::size_t Count = static_cast<std::size_t>(*CountOpt);
	if(Dim > MaxEmbeddingDim || Count > MaxEmbeddingVocab)
		return std::nullopt;

	EmbeddingTable Table;
	Table.IsComplex = Complex;
	Table.Dim = Dim;
	Table.UnkVector.assign(RowWidth(Table), 0.f);
	Table.Rows.reserve(Count);

	const std::size_t Width = RowWidth(Table);
	std::string_view Body = Cell.substr(Close + 2);
	while(!Body.empty()) {
		std::size_t Semi = Body.find(';');
		const std::string_view Row = (Semi == std::string_view::npos) ? Body : Body.substr(0, Semi);
		const std::size_t Pipe = Row.find('|');
		if(Pipe == std::string_view::npos)
			return std::nullopt;
		const std::string Token(Row.substr(0, Pipe));
		const auto Vec = ParseRowVec(Row.substr(Pipe + 1), Width);
		if(!Vec)
			return std::nullopt;
		if(Table.TokenToRow.contains(Token))
			return std::nullopt;
		std::vector<float> Fv(Width);
		for(std::size_t I = 0; I < Width; ++I)
			Fv[I] = static_cast<float>((*Vec)[I]);
		const std::size_t Idx = Table.Rows.size();
		Table.TokenToRow.emplace(Token, Idx);
		Table.Tokens.push_back(Token);
		Table.Rows.push_back(std::move(Fv));
		if(Semi == std::string_view::npos)
			break;
		Body.remove_prefix(Semi + 1);
	}
	if(Table.Rows.size() != Count)
		return std::nullopt;
	FlattenRows(Table);
	return Table;
}

std::string Serialize(const EmbeddingTable &Table) {
	std::ostringstream O;
	O << (Table.IsComplex ? "EC[" : "E[") << Table.Dim << ',' << Table.Rows.size() << "]:";
	const std::size_t Width = RowWidth(Table);
	for(std::size_t R = 0; R < Table.Rows.size(); ++R) {
		if(R)
			O << ';';
		O << Table.Tokens[R] << '|';
		const auto &Row = Table.Rows[R];
		for(std::size_t D = 0; D < Width; ++D) {
			if(D)
				O << ',';
			O << Row[D];
		}
	}
	return std::move(O).str();
}

std::string Fingerprint(const EmbeddingTable &Table) {
	std::uint64_t H = 1469598103934665603ull;
	auto Mix = [&](std::uint64_t V) {
		H ^= V;
		H *= 1099511628211ull;
	};
	Mix(static_cast<std::uint64_t>(Table.IsComplex));
	Mix(static_cast<std::uint64_t>(Table.Dim));
	Mix(static_cast<std::uint64_t>(Table.Rows.size()));
	for(const auto &Tok : Table.Tokens) {
		for(unsigned char C : Tok)
			Mix(C);
	}
	for(float V : Table.RowsFlat)
		Mix(static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(V)));
	std::ostringstream O;
	O << std::hex << H;
	return std::move(O).str();
}

std::optional<EmbeddingTable> BuildFromTokensAndMatrix(const std::vector<std::string> &Tokens,
                                                       const std::vector<double> &MatrixFlat, std::size_t Rows,
                                                       std::size_t Cols, bool IsComplex) {
	if(Tokens.size() != Rows || Rows == 0 || Cols == 0 || Cols > MaxEmbeddingDim || Rows > MaxEmbeddingVocab)
		return std::nullopt;
	const std::size_t Width = IsComplex ? Cols * 2 : Cols;
	if(MatrixFlat.size() != Rows * Width)
		return std::nullopt;
	EmbeddingTable Table;
	Table.IsComplex = IsComplex;
	Table.Dim = Cols;
	Table.UnkVector.assign(Width, 0.f);
	Table.Rows.reserve(Rows);
	for(std::size_t R = 0; R < Rows; ++R) {
		if(Table.TokenToRow.contains(Tokens[R]))
			return std::nullopt;
		std::vector<float> Row(Width);
		for(std::size_t C = 0; C < Width; ++C)
			Row[C] = static_cast<float>(MatrixFlat[R * Width + C]);
		Table.TokenToRow.emplace(Tokens[R], R);
		Table.Tokens.push_back(Tokens[R]);
		Table.Rows.push_back(std::move(Row));
	}
	FlattenRows(Table);
	return Table;
}

std::optional<EmbeddingTable> BuildFromTokensAndVectors(const std::vector<std::string> &Tokens,
                                                        const std::vector<std::vector<double>> &Vectors,
                                                        bool IsComplex) {
	if(Tokens.size() != Vectors.size() || Tokens.empty())
		return std::nullopt;
	const std::size_t Dim = IsComplex ? Vectors.front().size() / 2 : Vectors.front().size();
	if(Dim == 0 || Dim > MaxEmbeddingDim || Tokens.size() > MaxEmbeddingVocab)
		return std::nullopt;
	const std::size_t Width = IsComplex ? Dim * 2 : Dim;
	for(const auto &V : Vectors) {
		if(V.size() != Width)
			return std::nullopt;
	}
	EmbeddingTable Table;
	Table.IsComplex = IsComplex;
	Table.Dim = Dim;
	Table.UnkVector.assign(Width, 0.f);
	Table.Rows.reserve(Tokens.size());
	for(std::size_t I = 0; I < Tokens.size(); ++I) {
		if(Table.TokenToRow.contains(Tokens[I]))
			return std::nullopt;
		std::vector<float> Row(Width);
		for(std::size_t D = 0; D < Width; ++D)
			Row[D] = static_cast<float>(Vectors[I][D]);
		Table.TokenToRow.emplace(Tokens[I], I);
		Table.Tokens.push_back(Tokens[I]);
		Table.Rows.push_back(std::move(Row));
	}
	FlattenRows(Table);
	return Table;
}

std::vector<float> LookupVector(const EmbeddingTable &Table, std::string_view Token) {
	const auto It = Table.TokenToRow.find(std::string(Token));
	if(It == Table.TokenToRow.end())
		return Table.UnkVector;
	return Table.Rows[It->second];
}

std::vector<std::vector<double>> BatchLookup(const EmbeddingTable &Table, const std::vector<std::string> &Tokens) {
	std::vector<std::size_t> Order(Tokens.size());
	for(std::size_t I = 0; I < Tokens.size(); ++I)
		Order[I] = I;
	std::sort(Order.begin(), Order.end(), [&](std::size_t A, std::size_t B) { return Tokens[A] < Tokens[B]; });

	std::vector<std::vector<double>> Out(Tokens.size());
	for(std::size_t Oi : Order) {
		const auto Vec = LookupVector(Table, Tokens[Oi]);
		const std::size_t Width = RowWidth(Table);
		std::vector<double> Row(Width);
		for(std::size_t D = 0; D < Width; ++D)
			Row[D] = static_cast<double>(Vec[D]);
		Out[Oi] = std::move(Row);
	}
	return Out;
}

std::vector<double> MeanPool(const EmbeddingTable &Table, const std::vector<std::string> &Tokens) {
	if(Table.Dim == 0 || Tokens.empty())
		return {};
	const std::size_t Width = RowWidth(Table);
	std::vector<float> Acc(Width, 0.f);
	for(const auto &Tok : Tokens) {
		const auto &Vec = LookupVector(Table, Tok);
		if(Vec.size() != Width)
			continue;
		Simd::AddF32(Acc.data(), Acc.data(), Vec.data(), Width);
	}
	const float Inv = 1.f / static_cast<float>(Tokens.size());
	Simd::ScaleF32(Acc.data(), Acc.data(), Inv, Width);
	std::vector<double> Out(Width);
	for(std::size_t I = 0; I < Width; ++I)
		Out[I] = static_cast<double>(Acc[I]);
	return Out;
}

std::optional<std::string> ResolveTableCell(std::string_view Ref, Database *Db) {
	if(Ref.empty())
		return std::nullopt;
	std::string_view Name = Ref;
	if(Ref.size() > 9 && Ref.substr(0, 9) == "catalog:")
		Name = Ref.substr(9);
	else if(!Ref.empty() && Ref.front() == '@')
		Name = Ref.substr(1);
	else
		return std::string(Ref);
	if(!Db || Name.empty())
		return std::nullopt;
	return Db->EmbeddingWireCellAssumeDbMutexHeld(std::string(Name));
}

std::optional<std::string> BuildCellFromReal(const std::string &TokenListCell, const std::string &VectorCell) {
	const auto Tokens = AdvancedTypes::ParseListCell(TokenListCell);
	if(!Tokens)
		return std::nullopt;
	if(const auto Mat = MathSciComplex::ParseNumericMat(VectorCell)) {
		std::vector<double> Flat(Mat->Flat.begin(), Mat->Flat.end());
		const auto Table =
		    BuildFromTokensAndMatrix(*Tokens, Flat, Mat->Rows, Mat->Cols, Mat->Kind == MathSciComplex::NumericKind::Complex);
		return Table ? std::optional<std::string>(Serialize(*Table)) : std::nullopt;
	}
	if(const auto List = AdvancedTypes::ParseListCell(VectorCell)) {
		std::vector<std::vector<double>> Vectors;
		bool Complex = false;
		Vectors.reserve(List->size());
		for(const auto &Cell : *List) {
			if(const auto Nv = MathSciComplex::ParseNumericVec(Cell)) {
				Complex = Nv->IsComplex();
				Vectors.emplace_back(Nv->Values.begin(), Nv->Values.end());
				continue;
			}
			return std::nullopt;
		}
		const auto Table = BuildFromTokensAndVectors(*Tokens, Vectors, Complex);
		return Table ? std::optional<std::string>(Serialize(*Table)) : std::nullopt;
	}
	return std::nullopt;
}

static std::optional<std::string> FormatLookupVec(const EmbeddingTable &Table, const std::vector<float> &Vec) {
	MathSciComplex::NumericVec Nv;
	Nv.Kind = Table.IsComplex ? MathSciComplex::NumericKind::Complex : MathSciComplex::NumericKind::Real;
	Nv.Values = Vec;
	return MathSciComplex::FormatNumericVec(Nv);
}

std::optional<std::string> LookupCellFromReal(const std::string &TableCell, std::string_view Token, Database *Db) {
	std::string Resolved = TableCell;
	if(const auto Cat = ResolveTableCell(TableCell, Db))
		Resolved = *Cat;
	const auto Table = Deserialize(Resolved);
	if(!Table)
		return std::nullopt;
	return FormatLookupVec(*Table, LookupVector(*Table, Token));
}

std::optional<std::string> BatchCellFromReal(const std::string &TableCell, const std::string &TokenListCell,
                                            Database *Db) {
	std::string Resolved = TableCell;
	if(const auto Cat = ResolveTableCell(TableCell, Db))
		Resolved = *Cat;
	const auto Table = Deserialize(Resolved);
	if(!Table)
		return std::nullopt;
	const auto Parsed = AdvancedTypes::ParseListCell(TokenListCell);
	if(!Parsed)
		return std::nullopt;
	std::vector<std::string> Cells;
	Cells.reserve(Parsed->size());
	for(const auto &Tok : *Parsed) {
		const auto Cell = FormatLookupVec(*Table, LookupVector(*Table, Tok));
		if(!Cell)
			return std::nullopt;
		Cells.push_back(*Cell);
	}
	return AdvancedTypes::FormatListCell(Cells);
}

std::optional<std::string> SerializeCellFromReal(const std::string &TableCell) {
	const auto Table = Deserialize(TableCell);
	return Table ? std::optional<std::string>(Serialize(*Table)) : std::nullopt;
}

std::optional<std::string> LoadCellFromReal(const std::string &TableCell) {
	return SerializeCellFromReal(TableCell);
}

std::optional<std::string> FingerprintCellFromReal(const std::string &TableCell) {
	const auto Table = Deserialize(TableCell);
	return Table ? std::optional<std::string>(Fingerprint(*Table)) : std::nullopt;
}

std::optional<std::string> MeanCellFromReal(const std::string &TokenListCell, const std::string &TableCell,
                                            Database *Db) {
	std::string Resolved = TableCell;
	if(const auto Cat = ResolveTableCell(TableCell, Db))
		Resolved = *Cat;
	const auto Table = Deserialize(Resolved);
	if(!Table)
		return std::nullopt;
	const auto Parsed = AdvancedTypes::ParseListCell(TokenListCell);
	if(!Parsed)
		return std::nullopt;
	const auto Pooled = MeanPool(*Table, *Parsed);
	MathSciComplex::NumericVec Nv;
	Nv.Kind = Table->IsComplex ? MathSciComplex::NumericKind::Complex : MathSciComplex::NumericKind::Real;
	Nv.Values.reserve(Pooled.size());
	for(double V : Pooled)
		Nv.Values.push_back(static_cast<float>(V));
	return MathSciComplex::FormatNumericVec(Nv);
}

} // namespace MathSciEmbeddings
} // namespace AstralDB
