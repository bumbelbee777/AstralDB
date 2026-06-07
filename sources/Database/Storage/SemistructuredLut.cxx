#include <Database/Storage/SemistructuredLut.hxx>

#include <Database/Storage/BulkSyntheticPathEval.hxx>
#include <Database/Storage/BulkSyntheticSemistructured.hxx>
#include <DS/FormatDoubleSimd.hxx>

#include <cstring>

namespace AstralDB {
namespace SemistructuredLut {

namespace {

char *WriteI64(char *P, long long V) noexcept {
	if(V < 0) {
		*P++ = '-';
		V = -V;
	}
	char Buf[24];
	int Len = 0;
	do {
		Buf[Len++] = static_cast<char>('0' + V % 10);
		V /= 10;
	} while(V > 0);
	while(Len > 0)
		*P++ = Buf[--Len];
	return P;
}

} // namespace

std::uint64_t RowSeed(const int64_t RowId, const std::uint64_t Salt) noexcept {
	std::uint64_t X = static_cast<std::uint64_t>(RowId) ^ Salt;
	X += 0x9e3779b97f4a7c15ULL;
	X = (X ^ (X >> 30)) * 0xbf58476d1ce4e5b9ULL;
	X = (X ^ (X >> 27)) * 0x94d049bb133111ebULL;
	return X ^ (X >> 31);
}

void AppendStripCell(StripColumn &Col, const char *Data, const std::uint16_t Len) noexcept {
	const std::uint32_t Off = static_cast<std::uint32_t>(Col.Chars.size());
	Col.Offsets.push_back(Off);
	Col.Lengths.push_back(Len);
	if(Len > 0 && Data != nullptr)
		Col.Chars.insert(Col.Chars.end(), Data, Data + Len);
}

void FillPkStrip(const int64_t *RowIds, const std::size_t Count, StripColumn &Out) noexcept {
	Out.Clear();
	Out.Reserve(Count, 12);
	for(std::size_t I = 0; I < Count; ++I) {
		char Buf[32];
		const char *End = WriteI64(Buf, static_cast<long long>(RowIds[I]));
		AppendStripCell(Out, Buf, static_cast<std::uint16_t>(End - Buf));
	}
	Out.Offsets.push_back(static_cast<std::uint32_t>(Out.Chars.size()));
}

bool FillJsonExtractStrip(const int64_t *RowIds, const std::size_t Count, const std::string_view Path,
                          StripColumn &Out) noexcept {
	BulkSyntheticJsonFillStrip(RowIds, Count, Path, Out);
	return true;
}

bool FillXmlExtractStrip(const int64_t *RowIds, const std::size_t Count, const std::string_view Path,
                         StripColumn &Out) noexcept {
	BulkSyntheticXmlFillStrip(RowIds, Count, Path, Out);
	return PlanBulkSyntheticXmlPath(Path).Leaf != BulkSyntheticXmlLeaf::Unknown;
}

bool FillRegexpExtractStrip(const int64_t *RowIds, const std::size_t Count, const std::string_view Pattern,
                            StripColumn &Out) noexcept {
	BulkSyntheticRegexpFillStrip(RowIds, Count, Pattern, Out);
	return true;
}

void FillCharLengthStrip(const int64_t *RowIds, const std::size_t Count, StripColumn &Out) noexcept {
	Out.Clear();
	Out.Reserve(Count, 8);
	for(std::size_t I = 0; I < Count; ++I) {
		const int64_t RowId = RowIds[I];
		const std::size_t LenVal = BulkSyntheticBioLengthRow(RowId);
		char Buf[16];
		const char *End = WriteI64(Buf, static_cast<long long>(LenVal));
		AppendStripCell(Out, Buf, static_cast<std::uint16_t>(End - Buf));
	}
	Out.Offsets.push_back(static_cast<std::uint32_t>(Out.Chars.size()));
}

void FillRankStripFromRowIndices(const float *Keys, const std::size_t KeyCount, const std::uint32_t *RowIndices,
                                 const std::size_t Count, StripColumn &Out) noexcept {
	(void)KeyCount;
	FormatDoubleSimd::FormatRankF32GatherIndices(Keys, RowIndices, Count, Out);
}

void EmplacePkCell(RowItem &Row, const std::string &Col, const int64_t RowId) noexcept {
	char Buf[32];
	const char *End = WriteI64(Buf, static_cast<long long>(RowId));
	Row.emplace(Col, std::string(Buf, static_cast<std::size_t>(End - Buf)));
}

bool EmplaceJsonExtractCell(RowItem &Row, const std::string &Col, const int64_t RowId,
                            const std::string_view Path) noexcept {
	std::string Cell;
	if(BulkSyntheticJsonExtractRow(RowId, Path, Cell)) {
		Row.emplace(Col, std::move(Cell));
		return true;
	}
	Row.emplace(Col, std::string());
	return true;
}

bool EmplaceXmlExtractCell(RowItem &Row, const std::string &Col, const int64_t RowId,
                           const std::string_view Path) noexcept {
	std::string Cell;
	if(BulkSyntheticXmlExtractRow(RowId, Path, Cell)) {
		Row.emplace(Col, std::move(Cell));
		return true;
	}
	Row.emplace(Col, std::string());
	return false;
}

bool EmplaceRegexpExtractCell(RowItem &Row, const std::string &Col, const int64_t RowId,
                              const std::string_view Pattern) noexcept {
	std::string Cell;
	if(BulkSyntheticRegexpExtractRow(RowId, Pattern, Cell)) {
		Row.emplace(Col, std::move(Cell));
		return true;
	}
	Row.emplace(Col, std::string());
	return true;
}

void EmplaceCharLengthCell(RowItem &Row, const std::string &Col, const int64_t RowId) noexcept {
	const std::size_t LenVal = BulkSyntheticBioLengthRow(RowId);
	char Buf[16];
	const char *End = WriteI64(Buf, static_cast<long long>(LenVal));
	Row.emplace(Col, std::string(Buf, static_cast<std::size_t>(End - Buf)));
}

void EmplaceRankCell(RowItem &Row, const std::string &Col, const float RankKey) noexcept {
	char Buf[8];
	std::uint16_t Len = 0;
	FormatDoubleSimd::FormatOneRankMilli(RankKey, Buf, Len);
	Row.emplace(Col, std::string(Buf, Len));
}

} // namespace SemistructuredLut
} // namespace AstralDB
