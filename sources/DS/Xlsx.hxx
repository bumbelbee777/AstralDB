#pragma once

#include <DS/TabularData.hxx>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace AstralDB::DS {

/** One worksheet in an Office Open XML (\c .xlsx) workbook. */
struct XlsxSheet {
	std::string Name;
	Table Data;
};

/** Write one or more sheets (SQL Server–style workbook: one table per sheet). */
bool WriteXlsx(const std::filesystem::path &Path, const std::vector<XlsxSheet> &Sheets);

/** Read the first sheet, or the sheet whose name matches \a SheetName (case-insensitive). */
std::optional<Table> ReadXlsx(const std::filesystem::path &Path, std::string_view SheetName = {});

/** List worksheet names from workbook metadata. */
std::vector<std::string> ListXlsxSheetNames(const std::filesystem::path &Path);

} // namespace AstralDB::DS
