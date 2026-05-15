#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <optional>
#include <memory>
#include <DS/JSON.hxx>

namespace AstralDB {
namespace DS {

// Configuration for format handling
struct FormatOptions {
    char Delimiter = ',';           // ',' for CSV, '\t' for TSV
    char QuoteChar = '"';           // Quote character for fields
    char EscapeChar = '"';          // Escape character (doubled quotes)
    bool HasHeader = true;          // First row as column names
    std::string LineEnding = "\n";  // Line ending style
    bool TrimWhitespace = false;    // Trim surrounding whitespace from fields
    bool AllowNulls = true;         // Empty fields become null
    std::string NullRepresentation = "";  // String that represents NULL
};

// Forward declarations
class TableReader;
class TableWriter;

// Represents a single row of data
struct Row {
    std::vector<std::string> Fields;
    
    Row() = default;
    Row(const std::vector<std::string>& Fields) : Fields(Fields) {}
    Row(std::vector<std::string>&& Fields) : Fields(std::move(Fields)) {}
    
    size_t Size() const { return Fields.size(); }
    bool Empty() const { return Fields.empty(); }
    const std::string& operator[](size_t Index) const { return Fields[Index]; }
    std::string& operator[](size_t Index) { return Fields[Index]; }
};

// Tabular data structure
struct Table {
    std::vector<std::string> Headers;
    std::vector<Row> Rows;
    
    Table() = default;
    Table(const std::vector<std::string>& Headers) : Headers(Headers) {}
    
    size_t ColumnCount() const { return Headers.size(); }
    size_t RowCount() const { return Rows.size(); }
    bool Empty() const { return Rows.empty(); }
    
    // Convert to JSON (array of objects)
    JSON ToJSON() const {
        JSONArray Arr;
        for (const auto& Row : Rows) {
            JSONObject Obj;
            for (size_t i = 0; i < Headers.size() && i < Row.Size(); ++i) {
                if (!Row.Fields[i].empty() || !FormatOptions().AllowNulls) {
                    Obj[Headers[i]] = JSON(Row.Fields[i]);
                } else {
                    Obj[Headers[i]] = JSON(nullptr);  // null
                }
            }
            Arr.push_back(JSON(Obj));
        }
        return JSON(Arr);
    }
    
    // Convert from JSON (array of objects)
    static Table FromJSON(const JSON& Json, const std::vector<std::string>& ExpectedHeaders = {}) {
        Table Result;
        if (!Json.IsArray()) return Result;
        
        const auto& Arr = Json.AsArray();
        if (Arr.empty()) return Result;
        
        // Extract headers from first object
        if (Arr[0].IsObject()) {
            const auto& FirstObj = Arr[0].AsObject();
            if (ExpectedHeaders.empty()) {
                for (const auto& [Key, _] : FirstObj) {
                    Result.Headers.push_back(Key);
                }
            } else {
                Result.Headers = ExpectedHeaders;
            }
        }
        
        // Extract rows
        for (const auto& Item : Arr) {
            if (!Item.IsObject()) continue;
            const auto& Obj = Item.AsObject();
            Row NewRow;
            for (const auto& Header : Result.Headers) {
                auto It = Obj.find(Header);
                if (It != Obj.end() && !It->second.IsNull()) {
                    NewRow.Fields.push_back(It->second.AsString());
                } else {
                    NewRow.Fields.push_back("");
                }
            }
            Result.Rows.push_back(NewRow);
        }
        
        return Result;
    }
};

// CSV/TSV Reader
class TableReader {
public:
    TableReader() = default;
    explicit TableReader(const FormatOptions& Options) : Options(Options) {}

    /** Read one logical table ending at EOF / next section (caller may truncate input). */
    std::optional<Table> Read(std::istream& Stream) { return ReadFromStream(Stream); }
    
    std::optional<Table> ReadFromFile(const std::string& FilePath) {
        std::ifstream File(FilePath);
        if (!File.is_open()) return std::nullopt;
        return ReadFromStream(File);
    }
    
    std::optional<Table> ReadFromString(const std::string& Content) {
        std::stringstream SS(Content);
        return ReadFromStream(SS);
    }
    
private:
    FormatOptions Options;
    
    std::optional<Table> ReadFromStream(std::istream& Stream) {
        Table Result;
        std::string Line;
        bool IsFirstRow = true;
        
        while (std::getline(Stream, Line)) {
            if (Line.empty()) continue;
            
            // Handle different line endings
            if (!Line.empty() && Line.back() == '\r') {
                Line.pop_back();
            }
            
            std::vector<std::string> Fields = ParseLine(Line);
            
            if (IsFirstRow && Options.HasHeader) {
                Result.Headers = Fields;
                IsFirstRow = false;
            } else {
                if (IsFirstRow && !Options.HasHeader) {
                    // Generate default headers
                    for (size_t i = 0; i < Fields.size(); ++i) {
                        Result.Headers.push_back("Column" + std::to_string(i + 1));
                    }
                    IsFirstRow = false;
                }
                Result.Rows.emplace_back(Fields);
            }
        }
        
        return Result;
    }
    
    std::vector<std::string> ParseLine(const std::string& Line) {
        std::vector<std::string> Fields;
        std::string CurrentField;
        bool InQuotes = false;
        size_t Pos = 0;
        
        while (Pos < Line.length()) {
            char Ch = Line[Pos];
            
            if (Ch == Options.QuoteChar) {
                if (InQuotes && Pos + 1 < Line.length() && Line[Pos + 1] == Options.QuoteChar) {
                    // Escaped quote
                    CurrentField += Ch;
                    Pos += 2;
                    continue;
                }
                InQuotes = !InQuotes;
                Pos++;
            } else if (Ch == Options.Delimiter && !InQuotes) {
                // End of field
                if (Options.TrimWhitespace) {
                    CurrentField = Trim(CurrentField);
                }
                Fields.push_back(CurrentField);
                CurrentField.clear();
                Pos++;
            } else {
                CurrentField += Ch;
                Pos++;
            }
        }
        
        // Last field
        if (Options.TrimWhitespace) {
            CurrentField = Trim(CurrentField);
        }
        Fields.push_back(CurrentField);
        
        return Fields;
    }
    
    static std::string Trim(const std::string& Str) {
        size_t First = Str.find_first_not_of(" \t\n\r");
        if (First == std::string::npos) return "";
        size_t Last = Str.find_last_not_of(" \t\n\r");
        return Str.substr(First, Last - First + 1);
    }
};

// CSV/TSV Writer
class TableWriter {
public:
    TableWriter() = default;
    explicit TableWriter(const FormatOptions& Options) : Options(Options) {}

    /** Write a single logical table including header row (when Options.HasHeader). */
    bool Write(std::ostream& Stream, const Table& Data) { return WriteToStream(Data, Stream); }
    
    bool WriteToFile(const Table& Data, const std::string& FilePath) {
        std::ofstream File(FilePath);
        if (!File.is_open()) return false;
        return WriteToStream(Data, File);
    }
    
    std::string WriteToString(const Table& Data) {
        std::stringstream SS;
        WriteToStream(Data, SS);
        return SS.str();
    }
    
private:
    FormatOptions Options;
    
    bool WriteToStream(const Table& Data, std::ostream& Stream) {
        // Write headers
        if (Options.HasHeader && !Data.Headers.empty()) {
            for (size_t i = 0; i < Data.Headers.size(); ++i) {
                if (i > 0) Stream << Options.Delimiter;
                WriteEscapedField(Stream, Data.Headers[i]);
            }
            Stream << Options.LineEnding;
        }
        
        // Write rows
        for (const auto& Row : Data.Rows) {
            for (size_t i = 0; i < Row.Size(); ++i) {
                if (i > 0) Stream << Options.Delimiter;
                
                const std::string& Field = Row.Fields[i];
                if (Field.empty() && Options.AllowNulls) {
                    // Write nothing (null)
                } else {
                    WriteEscapedField(Stream, Field);
                }
            }
            Stream << Options.LineEnding;
        }
        
        return true;
    }
    
    void WriteEscapedField(std::ostream& Stream, const std::string& Field) {
        bool NeedsQuoting = false;
        
        // Check if field needs quoting
        if (Field.find(Options.Delimiter) != std::string::npos ||
            Field.find(Options.QuoteChar) != std::string::npos ||
            Field.find('\n') != std::string::npos ||
            (Options.TrimWhitespace && !Field.empty() &&
             (Field.front() == ' ' || Field.back() == ' '))) {
            NeedsQuoting = true;
        }
        
        if (!NeedsQuoting) {
            Stream << Field;
            return;
        }
        
        // Write quoted field with escaping
        Stream << Options.QuoteChar;
        for (char Ch : Field) {
            if (Ch == Options.QuoteChar) {
                Stream << Options.EscapeChar << Options.EscapeChar;
            } else {
                Stream << Ch;
            }
        }
        Stream << Options.QuoteChar;
    }
};

// Convenience functions for seamless conversion
inline Table ReadCSV(const std::string& Path, bool HasHeader = true) {
    FormatOptions Opts;
    Opts.Delimiter = ',';
    Opts.HasHeader = HasHeader;
    TableReader Reader(Opts);
    auto Result = Reader.ReadFromFile(Path);
    return Result.value_or(Table());
}

inline Table ReadTSV(const std::string& Path, bool HasHeader = true) {
    FormatOptions Opts;
    Opts.Delimiter = '\t';
    Opts.HasHeader = HasHeader;
    TableReader Reader(Opts);
    auto Result = Reader.ReadFromFile(Path);
    return Result.value_or(Table());
}

inline bool WriteCSV(const Table& Data, const std::string& Path, bool IncludeHeader = true) {
    FormatOptions Opts;
    Opts.Delimiter = ',';
    Opts.HasHeader = IncludeHeader;
    TableWriter Writer(Opts);
    return Writer.WriteToFile(Data, Path);
}

inline bool WriteTSV(const Table& Data, const std::string& Path, bool IncludeHeader = true) {
    FormatOptions Opts;
    Opts.Delimiter = '\t';
    Opts.HasHeader = IncludeHeader;
    TableWriter Writer(Opts);
    return Writer.WriteToFile(Data, Path);
}

// Convert between formats
inline Table ConvertCSVToTSV(const std::string& CSVPath, const std::string& TSVPath, bool HasHeader = true) {
    Table Data = ReadCSV(CSVPath, HasHeader);
    WriteTSV(Data, TSVPath, HasHeader);
    return Data;
}

inline Table ConvertTSVToCSV(const std::string& TSVPath, const std::string& CSVPath, bool HasHeader = true) {
    Table Data = ReadTSV(TSVPath, HasHeader);
    WriteCSV(Data, CSVPath, HasHeader);
    return Data;
}

// JSON integration
inline JSON TableToJSON(const Table& Data) {
    return Data.ToJSON();
}

inline Table JSONToTable(const JSON& Json, const std::vector<std::string>& Headers = {}) {
    return Table::FromJSON(Json, Headers);
}

} // namespace DS
} // namespace AstralDB