#include <DS/Xlsx.hxx>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace AstralDB::DS {
namespace {

void FoldAsciiUpper(std::string &S) {
	for(char &C : S)
		C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
}

struct ZipEntry {
	std::string Name;
	std::string Data;
};

void AppendLe16(std::string &Out, uint16_t V) {
	Out.push_back(static_cast<char>(V & 0xff));
	Out.push_back(static_cast<char>((V >> 8) & 0xff));
}

void AppendLe32(std::string &Out, uint32_t V) {
	Out.push_back(static_cast<char>(V & 0xff));
	Out.push_back(static_cast<char>((V >> 8) & 0xff));
	Out.push_back(static_cast<char>((V >> 16) & 0xff));
	Out.push_back(static_cast<char>((V >> 24) & 0xff));
}

std::string XmlEscape(std::string_view Text) {
	std::string Out;
	Out.reserve(Text.size());
	for(char C : Text) {
		switch(C) {
		case '&':
			Out += "&amp;";
			break;
		case '<':
			Out += "&lt;";
			break;
		case '>':
			Out += "&gt;";
			break;
		case '"':
			Out += "&quot;";
			break;
		case '\'':
			Out += "&apos;";
			break;
		default:
			Out += C;
		}
	}
	return Out;
}

std::string ColumnRef(int Col, int Row) {
	std::string Ref;
	int C = Col;
	do {
		Ref.insert(Ref.begin(), static_cast<char>('A' + (C % 26)));
		C = C / 26 - 1;
	} while(C >= 0);
	Ref += std::to_string(Row);
	return Ref;
}

std::string BuildSheetXml(const Table &Data) {
	std::ostringstream Xml;
	Xml << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
	    << "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
	    << "<sheetData>\n";
	int RowNum = 1;
	if(!Data.Headers.empty()) {
		Xml << "<row r=\"" << RowNum << "\">";
		for(size_t C = 0; C < Data.Headers.size(); ++C) {
			const std::string Ref = ColumnRef(static_cast<int>(C), RowNum);
			Xml << "<c r=\"" << Ref << "\" t=\"inlineStr\"><is><t>" << XmlEscape(Data.Headers[C])
			    << "</t></is></c>";
		}
		Xml << "</row>\n";
		++RowNum;
	}
	for(const Row &Rw : Data.Rows) {
		Xml << "<row r=\"" << RowNum << "\">";
		const size_t Cols = std::max(Rw.Size(), Data.Headers.size());
		for(size_t C = 0; C < Cols; ++C) {
			const std::string Ref = ColumnRef(static_cast<int>(C), RowNum);
			const std::string Val = C < Rw.Size() ? Rw.Fields[C] : std::string();
			Xml << "<c r=\"" << Ref << "\" t=\"inlineStr\"><is><t>" << XmlEscape(Val) << "</t></is></c>";
		}
		Xml << "</row>\n";
		++RowNum;
	}
	Xml << "</sheetData></worksheet>";
	return Xml.str();
}

std::string SanitizeSheetName(std::string Name) {
	if(Name.empty())
		Name = "Sheet1";
	for(char &C : Name) {
		if(C == ':' || C == '\\' || C == '/' || C == '?' || C == '*' || C == '[' || C == ']')
			C = '_';
	}
	if(Name.size() > 31)
		Name.resize(31);
	return Name;
}

std::string BuildWorkbookXml(const std::vector<XlsxSheet> &Sheets) {
	std::ostringstream Xml;
	Xml << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
	    << "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
	    << "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
	    << "<sheets>";
	for(size_t I = 0; I < Sheets.size(); ++I)
		Xml << "<sheet name=\"" << XmlEscape(SanitizeSheetName(Sheets[I].Name)) << "\" sheetId=\"" << (I + 1)
		    << "\" r:id=\"rId" << (I + 1) << "\"/>";
	Xml << "</sheets></workbook>";
	return Xml.str();
}

std::string BuildWorkbookRels(const std::vector<XlsxSheet> &Sheets) {
	std::ostringstream Xml;
	Xml << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
	    << "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
	for(size_t I = 0; I < Sheets.size(); ++I)
		Xml << "<Relationship Id=\"rId" << (I + 1) << "\" "
		    << "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" "
		    << "Target=\"worksheets/sheet" << (I + 1) << ".xml\"/>";
	Xml << "</Relationships>";
	return Xml.str();
}

std::string BuildRootRels() {
	return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
	       "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
	       "<Relationship Id=\"rId1\" "
	       "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
	       "Target=\"xl/workbook.xml\"/>"
	       "</Relationships>";
}

std::string BuildContentTypes(std::size_t SheetCount) {
	std::ostringstream Xml;
	Xml << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
	    << "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
	    << "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
	    << "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
	    << "<Override PartName=\"/xl/workbook.xml\" "
	       "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>";
	for(std::size_t I = 0; I < SheetCount; ++I)
		Xml << "<Override PartName=\"/xl/worksheets/sheet" << (I + 1)
		    << ".xml\" "
		       "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>";
	Xml << "</Types>";
	return Xml.str();
}

std::string BuildZip(const std::vector<ZipEntry> &Entries) {
	std::string Out;
	std::vector<uint32_t> LocalOffsets;
	LocalOffsets.reserve(Entries.size());
	for(const ZipEntry &E : Entries) {
		LocalOffsets.push_back(static_cast<uint32_t>(Out.size()));
		AppendLe32(Out, 0x04034b50);
		AppendLe16(Out, 20);
		AppendLe16(Out, 0);
		AppendLe16(Out, 0);
		AppendLe16(Out, 0);
		AppendLe16(Out, 0);
		AppendLe32(Out, 0);
		AppendLe32(Out, static_cast<uint32_t>(E.Data.size()));
		AppendLe32(Out, static_cast<uint32_t>(E.Data.size()));
		AppendLe16(Out, static_cast<uint16_t>(E.Name.size()));
		AppendLe16(Out, 0);
		Out += E.Name;
		Out += E.Data;
	}
	const uint32_t CentralStart = static_cast<uint32_t>(Out.size());
	for(std::size_t I = 0; I < Entries.size(); ++I) {
		const ZipEntry &E = Entries[I];
		AppendLe32(Out, 0x02014b50);
		AppendLe16(Out, 20);
		AppendLe16(Out, 20);
		AppendLe16(Out, 0);
		AppendLe16(Out, 0);
		AppendLe16(Out, 0);
		AppendLe16(Out, 0);
		AppendLe32(Out, static_cast<uint32_t>(E.Data.size()));
		AppendLe32(Out, static_cast<uint32_t>(E.Data.size()));
		AppendLe16(Out, static_cast<uint16_t>(E.Name.size()));
		AppendLe16(Out, 0);
		AppendLe16(Out, 0);
		AppendLe16(Out, 0);
		AppendLe16(Out, 0);
		AppendLe16(Out, 0);
		AppendLe32(Out, LocalOffsets[I]);
		Out += E.Name;
	}
	const uint32_t CentralSize = static_cast<uint32_t>(Out.size() - CentralStart);
	AppendLe32(Out, 0x06054b50);
	AppendLe16(Out, 0);
	AppendLe16(Out, 0);
	AppendLe16(Out, static_cast<uint16_t>(Entries.size()));
	AppendLe16(Out, static_cast<uint16_t>(Entries.size()));
	AppendLe32(Out, CentralSize);
	AppendLe32(Out, CentralStart);
	AppendLe16(Out, 0);
	return Out;
}

bool WriteBytes(const std::filesystem::path &Path, const std::string &Bytes) {
	std::ofstream Out(Path, std::ios::binary);
	if(!Out)
		return false;
	Out.write(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
	return static_cast<bool>(Out);
}

std::string ReadAllBytes(const std::filesystem::path &Path) {
	std::ifstream In(Path, std::ios::binary);
	if(!In)
		return {};
	return {std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>()};
}

std::optional<std::string> ZipExtract(const std::string &Zip, std::string_view EntrySuffix) {
	const auto Needle = std::string(EntrySuffix);
	std::size_t Pos = 0;
	while(Pos + 30 < Zip.size()) {
		if(static_cast<unsigned char>(Zip[Pos]) != 0x50 || static_cast<unsigned char>(Zip[Pos + 1]) != 0x4b ||
		   static_cast<unsigned char>(Zip[Pos + 2]) != 0x03 || static_cast<unsigned char>(Zip[Pos + 3]) != 0x04)
			break;
		const uint16_t NameLen = static_cast<uint16_t>(static_cast<unsigned char>(Zip[Pos + 26])) |
		                         (static_cast<uint16_t>(static_cast<unsigned char>(Zip[Pos + 27])) << 8);
		const uint32_t CompSize = static_cast<uint32_t>(static_cast<unsigned char>(Zip[Pos + 18])) |
		                          (static_cast<uint32_t>(static_cast<unsigned char>(Zip[Pos + 19])) << 8) |
		                          (static_cast<uint32_t>(static_cast<unsigned char>(Zip[Pos + 20])) << 16) |
		                          (static_cast<uint32_t>(static_cast<unsigned char>(Zip[Pos + 21])) << 24);
		const std::string Name(Zip.data() + Pos + 30, NameLen);
		const std::size_t DataStart = Pos + 30 + NameLen;
		if(Name.size() >= Needle.size() && Name.compare(Name.size() - Needle.size(), Needle.size(), Needle) == 0)
			return Zip.substr(DataStart, CompSize);
		Pos = DataStart + CompSize;
	}
	return std::nullopt;
}

std::vector<std::string> ParseSheetNamesFromWorkbook(std::string_view WorkbookXml) {
	std::vector<std::string> Out;
	std::size_t Pos = 0;
	while((Pos = WorkbookXml.find("<sheet ", Pos)) != std::string_view::npos) {
		const std::size_t NameKey = WorkbookXml.find("name=\"", Pos);
		if(NameKey == std::string_view::npos)
			break;
		const std::size_t Q0 = NameKey + 6;
		const std::size_t Q1 = WorkbookXml.find('"', Q0);
		if(Q1 == std::string_view::npos)
			break;
		Out.emplace_back(WorkbookXml.substr(Q0, Q1 - Q0));
		Pos = Q1 + 1;
	}
	return Out;
}

std::optional<Table> ParseSheetXml(std::string_view SheetXml) {
	Table Out;
	std::size_t Pos = 0;
	while((Pos = SheetXml.find("<row", Pos)) != std::string_view::npos) {
		const std::size_t RowEnd = SheetXml.find("</row>", Pos);
		if(RowEnd == std::string_view::npos)
			break;
		const std::string_view RowXml = SheetXml.substr(Pos, RowEnd - Pos);
		Row Rw;
		std::size_t CPos = 0;
		while((CPos = RowXml.find("<is><t>", CPos)) != std::string_view::npos) {
			CPos += 7;
			const std::size_t CEnd = RowXml.find("</t></is>", CPos);
			if(CEnd == std::string_view::npos)
				break;
			Rw.Fields.emplace_back(RowXml.substr(CPos, CEnd - CPos));
			CPos = CEnd;
		}
		if(Out.Headers.empty() && !Rw.Fields.empty()) {
			Out.Headers = Rw.Fields;
		} else if(!Rw.Fields.empty()) {
			Out.Rows.push_back(std::move(Rw));
		}
		Pos = RowEnd + 6;
	}
	if(Out.Headers.empty() && Out.Rows.empty())
		return std::nullopt;
	return Out;
}

} // namespace

bool WriteXlsx(const std::filesystem::path &Path, const std::vector<XlsxSheet> &Sheets) {
	if(Sheets.empty())
		return false;
	std::vector<ZipEntry> Entries;
	Entries.push_back({"[Content_Types].xml", BuildContentTypes(Sheets.size())});
	Entries.push_back({"_rels/.rels", BuildRootRels()});
	Entries.push_back({"xl/workbook.xml", BuildWorkbookXml(Sheets)});
	Entries.push_back({"xl/_rels/workbook.xml.rels", BuildWorkbookRels(Sheets)});
	for(size_t I = 0; I < Sheets.size(); ++I) {
		ZipEntry Sheet;
		Sheet.Name = "xl/worksheets/sheet" + std::to_string(I + 1) + ".xml";
		Sheet.Data = BuildSheetXml(Sheets[I].Data);
		Entries.push_back(std::move(Sheet));
	}
	return WriteBytes(Path, BuildZip(Entries));
}

std::vector<std::string> ListXlsxSheetNames(const std::filesystem::path &Path) {
	const std::string Zip = ReadAllBytes(Path);
	if(Zip.empty())
		return {};
	if(const auto Wb = ZipExtract(Zip, "xl/workbook.xml"))
		return ParseSheetNamesFromWorkbook(*Wb);
	return {};
}

std::optional<Table> ReadXlsx(const std::filesystem::path &Path, std::string_view SheetName) {
	const std::string Zip = ReadAllBytes(Path);
	if(Zip.empty())
		return std::nullopt;
	std::size_t SheetIndex = 1;
	if(!SheetName.empty()) {
		const auto Names = ListXlsxSheetNames(Path);
		const std::string Want(SheetName);
		auto It = std::find_if(Names.begin(), Names.end(), [&](const std::string &N) {
			std::string A = N;
			std::string B = Want;
			FoldAsciiUpper(A);
			FoldAsciiUpper(B);
			return A == B;
		});
		if(It == Names.end())
			return std::nullopt;
		SheetIndex = static_cast<std::size_t>(std::distance(Names.begin(), It)) + 1;
	}
	const std::string Suffix = "xl/worksheets/sheet" + std::to_string(SheetIndex) + ".xml";
	if(const auto Sheet = ZipExtract(Zip, Suffix))
		return ParseSheetXml(*Sheet);
	return std::nullopt;
}

} // namespace AstralDB::DS
