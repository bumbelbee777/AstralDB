#include <SQL/BytecodeProcedures.hxx>
#include <SQL/BytecodeInspect.hxx>
#include <SQL/SQL.hxx>
#include <DS/JSON.hxx>
#include <Database/Database.hxx>
#include <IO/Error.hxx>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>

namespace AstralDB {
namespace SQL {

namespace {

static constexpr int kCatalogVersion = 1;

std::filesystem::path ResolveBesideCatalog(const std::filesystem::path &CatalogPath,
                                           const std::filesystem::path &Stored) {
	if(Stored.is_absolute())
		return Stored;
	return CatalogPath.parent_path() / Stored;
}

std::string NormalizeAbcKey(const std::filesystem::path &P) {
	std::error_code Ec;
	return std::filesystem::weakly_canonical(P, Ec).string();
}

void LoadJsonStringArray(const DS::JSONObject &O, const char *Key, std::vector<std::string> &Out) {
	const auto It = O.find(Key);
	if(It == O.end() || !It->second.IsArray())
		return;
	for(const auto &J : It->second.AsArray())
		if(J.IsString())
			Out.push_back(J.AsString());
}

void SaveJsonStringArray(DS::JSONObject &O, const char *Key, const std::vector<std::string> &Vals) {
	if(Vals.empty())
		return;
	DS::JSONArray Arr;
	for(const auto &S : Vals)
		Arr.push_back(DS::JSON(S));
	O.emplace(Key, DS::JSON(std::move(Arr)));
}

} // namespace

std::vector<std::string> ScanProcedureCallsInSql(std::string_view BodySql) {
	std::vector<std::string> Out;
	std::string Upper(BodySql);
	std::transform(Upper.begin(), Upper.end(), Upper.begin(),
	               [](unsigned char C) { return static_cast<char>(std::toupper(C)); });
	for(std::size_t I = 0; I + 4 < Upper.size(); ++I) {
		if(Upper.substr(I, 4) != "CALL")
			continue;
		if(I > 0 && std::isalnum(static_cast<unsigned char>(Upper[I - 1])))
			continue;
		std::size_t J = I + 4;
		while(J < Upper.size() && std::isspace(static_cast<unsigned char>(Upper[J])))
			++J;
		std::size_t K = J;
		while(K < Upper.size() && (std::isalnum(static_cast<unsigned char>(Upper[K])) || Upper[K] == '_'))
			++K;
		if(K > J) {
			const std::string Name(BodySql.substr(J, K - J));
			if(std::find(Out.begin(), Out.end(), Name) == Out.end())
				Out.push_back(Name);
		}
	}
	for(std::size_t I = 0; I + 17 < Upper.size(); ++I) {
		if(Upper.substr(I, 8) != "EXECUTE ")
			continue;
		if(Upper.substr(I + 8, 9) != "PROCEDURE")
			continue;
		std::size_t J = I + 17;
		while(J < Upper.size() && std::isspace(static_cast<unsigned char>(Upper[J])))
			++J;
		std::size_t K = J;
		while(K < Upper.size() && (std::isalnum(static_cast<unsigned char>(Upper[K])) || Upper[K] == '_'))
			++K;
		if(K > J) {
			const std::string Name(BodySql.substr(J, K - J));
			if(std::find(Out.begin(), Out.end(), Name) == Out.end())
				Out.push_back(Name);
		}
	}
	return Out;
}

std::string HashProcedureSource(std::string_view BodySql) {
	return std::to_string(std::hash<std::string_view>{}(BodySql));
}

std::filesystem::path DefaultProcedureCatalogPath(const std::filesystem::path &SessionDbPath) {
	std::error_code Ec;
	const auto Db = std::filesystem::absolute(SessionDbPath, Ec);
	const auto Base = Ec || Db.empty() ? SessionDbPath : Db;
	const auto Parent = Base.parent_path();
	return (Parent.empty() ? std::filesystem::path(".") : Parent) / "astraldb_procs.json";
}

std::filesystem::path DefaultProcedureCacheDir(const std::filesystem::path &SessionDbPath) {
	std::error_code Ec;
	const auto Db = std::filesystem::absolute(SessionDbPath, Ec);
	const auto Base = Ec || Db.empty() ? SessionDbPath : Db;
	const auto Parent = Base.parent_path();
	return (Parent.empty() ? std::filesystem::path(".") : Parent) / "astraldb_procs_cache";
}

ProcedureCatalog LoadProcedureCatalog(const std::filesystem::path &CatalogPath) {
	ProcedureCatalog Catalog;
	Catalog.CatalogPath = CatalogPath;
	std::ifstream In(CatalogPath);
	if(!In)
		return Catalog;
	std::string Text((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
	if(Text.empty())
		return Catalog;
	const DS::JSON Root = DS::DecodeJSONStrict(Text);
	if(!Root.IsObject())
		throw std::runtime_error(AstralDB::Err::Prefixed("PROC", "Procedure catalog root must be a JSON object."));
	const auto &Obj = Root.AsObject();
	if(const auto Ver = Obj.find("version"); Ver != Obj.end() && Ver->second.IsNumber()) {
		if(static_cast<int>(Ver->second.AsNumber()) != kCatalogVersion)
			throw std::runtime_error(AstralDB::Err::Prefixed("PROC", "Unsupported procedure catalog version."));
	}
	const auto ProcIt = Obj.find("procedures");
	if(ProcIt != Obj.end() && ProcIt->second.IsObject()) {
		for(const auto &[Name, Entry] : ProcIt->second.AsObject()) {
			if(!Entry.IsObject())
				continue;
			StoredProcedureEntry E;
			E.Name = Name;
			const auto &EO = Entry.AsObject();
			if(const auto PathIt = EO.find("abc_path"); PathIt != EO.end() && PathIt->second.IsString())
				E.AbcPath = ResolveBesideCatalog(CatalogPath, PathIt->second.AsString());
			else if(const auto Legacy = EO.find("path"); Legacy != EO.end() && Legacy->second.IsString())
				E.AbcPath = ResolveBesideCatalog(CatalogPath, Legacy->second.AsString());
			if(const auto SqlIt = EO.find("sql_path"); SqlIt != EO.end() && SqlIt->second.IsString())
				E.SqlPath = ResolveBesideCatalog(CatalogPath, SqlIt->second.AsString());
			if(const auto DescIt = EO.find("description"); DescIt != EO.end() && DescIt->second.IsString())
				E.Description = DescIt->second.AsString();
			if(const auto SrcIt = EO.find("source_sql"); SrcIt != EO.end() && SrcIt->second.IsString())
				E.SourceSql = SrcIt->second.AsString();
			if(const auto HashIt = EO.find("source_hash"); HashIt != EO.end() && HashIt->second.IsString())
				E.SourceHash = HashIt->second.AsString();
			LoadJsonStringArray(EO, "tables", E.ReferencedTables);
			LoadJsonStringArray(EO, "depends_on", E.DependsOn);
			LoadJsonStringArray(EO, "called_by", E.CalledBy);
			if(!E.AbcPath.empty())
				Catalog.Procedures.push_back(std::move(E));
		}
	}
	if(const auto IdxIt = Obj.find("abc_index"); IdxIt != Obj.end() && IdxIt->second.IsObject()) {
		for(const auto &[Path, Names] : IdxIt->second.AsObject()) {
			if(!Names.IsArray())
				continue;
			std::vector<std::string> List;
			for(const auto &N : Names.AsArray())
				if(N.IsString())
					List.push_back(N.AsString());
			Catalog.AbcIndex[Path] = std::move(List);
		}
	}
	if(Catalog.AbcIndex.empty())
		RebuildProcedureRelations(Catalog);
	return Catalog;
}

void RebuildProcedureRelations(ProcedureCatalog &Catalog) {
	for(auto &E : Catalog.Procedures)
		E.CalledBy.clear();
	Catalog.AbcIndex.clear();
	for(StoredProcedureEntry &E : Catalog.Procedures) {
		if(!E.AbcPath.empty())
			Catalog.AbcIndex[NormalizeAbcKey(E.AbcPath)].push_back(E.Name);
	}
	for(StoredProcedureEntry &E : Catalog.Procedures) {
		if(E.DependsOn.empty() && !E.SourceSql.empty())
			E.DependsOn = ScanProcedureCallsInSql(E.SourceSql);
		for(const auto &Dep : E.DependsOn) {
			for(StoredProcedureEntry &Other : Catalog.Procedures) {
				if(Other.Name == Dep) {
					if(std::find(Other.CalledBy.begin(), Other.CalledBy.end(), E.Name) == Other.CalledBy.end())
						Other.CalledBy.push_back(E.Name);
				}
			}
		}
	}
}

void SaveProcedureCatalog(const ProcedureCatalog &Catalog) {
	DS::JSONObject Root;
	Root.emplace("version", DS::JSON(kCatalogVersion));
	DS::JSONObject Procs;
	const auto RelPath = [&](const std::filesystem::path &P) {
		std::error_code Ec;
		const auto R = std::filesystem::relative(P, Catalog.CatalogPath.parent_path(), Ec);
		return Ec ? P.string() : R.string();
	};
	for(const StoredProcedureEntry &E : Catalog.Procedures) {
		DS::JSONObject Entry;
		Entry.emplace("abc_path", DS::JSON(RelPath(E.AbcPath)));
		if(!E.SqlPath.empty())
			Entry.emplace("sql_path", DS::JSON(RelPath(E.SqlPath)));
		if(!E.Description.empty())
			Entry.emplace("description", DS::JSON(E.Description));
		if(!E.SourceSql.empty())
			Entry.emplace("source_sql", DS::JSON(E.SourceSql));
		if(!E.SourceHash.empty())
			Entry.emplace("source_hash", DS::JSON(E.SourceHash));
		SaveJsonStringArray(Entry, "tables", E.ReferencedTables);
		SaveJsonStringArray(Entry, "depends_on", E.DependsOn);
		SaveJsonStringArray(Entry, "called_by", E.CalledBy);
		Procs.emplace(E.Name, DS::JSON(std::move(Entry)));
	}
	Root.emplace("procedures", DS::JSON(std::move(Procs)));
	DS::JSONObject Idx;
	for(const auto &[Path, Names] : Catalog.AbcIndex) {
		DS::JSONArray Arr;
		for(const auto &N : Names)
			Arr.push_back(DS::JSON(N));
		Idx.emplace(Path, DS::JSON(std::move(Arr)));
	}
	Root.emplace("abc_index", DS::JSON(std::move(Idx)));
	{
		std::error_code Ec;
		std::filesystem::create_directories(Catalog.CatalogPath.parent_path(), Ec);
	}
	std::ofstream Out(Catalog.CatalogPath);
	if(!Out)
		throw std::runtime_error(
		    AstralDB::Err::Prefixed("PROC", "Cannot write procedure catalog: " + Catalog.CatalogPath.string()));
	Out << DS::SerializeJSON(DS::JSON(Root));
}

void RegisterProcedure(ProcedureCatalog &Catalog, std::string Name, const std::filesystem::path &AbcPath,
                       std::string Description, std::string SourceSql) {
	UnregisterProcedure(Catalog, Name);
	StoredProcedureEntry E;
	E.Name = std::move(Name);
	E.AbcPath = AbcPath;
	E.Description = std::move(Description);
	E.SourceSql = std::move(SourceSql);
	if(!E.SourceSql.empty())
		E.SourceHash = HashProcedureSource(E.SourceSql);
	const auto Loaded = LoadAbcFile(AbcPath);
	const auto Analysis = AnalyzeBytecode(Loaded.Instructions);
	E.ReferencedTables = Analysis.ReferencedTables;
	E.DependsOn = ScanProcedureCallsInSql(E.SourceSql);
	Catalog.Procedures.push_back(std::move(E));
	RebuildProcedureRelations(Catalog);
}

bool UnregisterProcedure(ProcedureCatalog &Catalog, std::string_view Name) {
	const auto It = std::find_if(Catalog.Procedures.begin(), Catalog.Procedures.end(),
	                              [&](const StoredProcedureEntry &E) { return E.Name == Name; });
	if(It == Catalog.Procedures.end())
		return false;
	DropProcedureCacheFiles(*It);
	Catalog.Procedures.erase(It);
	RebuildProcedureRelations(Catalog);
	return true;
}

std::optional<StoredProcedureEntry> FindProcedure(const ProcedureCatalog &Catalog, std::string_view Name) {
	for(const StoredProcedureEntry &E : Catalog.Procedures)
		if(E.Name == Name)
			return E;
	return std::nullopt;
}

LoadedAbcFile LoadProcedureBytecode(const StoredProcedureEntry &Entry, const ProcedureCatalog &Catalog) {
	const auto Resolved = ResolveBesideCatalog(Catalog.CatalogPath, Entry.AbcPath);
	return LoadAbcFile(Resolved);
}

void DropProcedureCacheFiles(const StoredProcedureEntry &Entry) {
	std::error_code Ec;
	if(!Entry.AbcPath.empty())
		std::filesystem::remove(Entry.AbcPath, Ec);
	if(!Entry.SqlPath.empty())
		std::filesystem::remove(Entry.SqlPath, Ec);
}

StoredProcedureEntry CacheProcedureFromSql(ProcedureCatalog &Catalog, const std::filesystem::path &SessionDbPath,
                                           std::string Name, std::string BodySql, Logger *Logger,
                                           OptimizationLevel OptLevel, const Database *CatalogDb, bool IfNotExists) {
	if(auto Existing = FindProcedure(Catalog, Name)) {
		if(IfNotExists)
			return *Existing;
		throw std::runtime_error(AstralDB::Err::Prefixed("PROC", "Procedure \"" + Name + "\" already exists."));
	}
	const std::filesystem::path CacheDir = DefaultProcedureCacheDir(SessionDbPath);
	std::error_code Ec;
	std::filesystem::create_directories(CacheDir, Ec);
	AstralDB::SQL::Parser Parser(BodySql);
	const auto Compiled = BuildCompiledBytecode(Logger, OptLevel, CatalogDb);
	const std::filesystem::path AbcPath = CacheDir / (Name + ".abc");
	const std::filesystem::path SqlPath = CacheDir / (Name + ".sql");
	SaveAbcFile(AbcPath, Compiled);
	{
		std::ofstream SqlOut(SqlPath);
		if(!SqlOut)
			throw std::runtime_error(AstralDB::Err::Prefixed("PROC", "Cannot write procedure SQL cache: " + SqlPath.string()));
		SqlOut << BodySql;
	}
	UnregisterProcedure(Catalog, Name);
	StoredProcedureEntry E;
	E.Name = std::move(Name);
	E.AbcPath = AbcPath;
	E.SqlPath = SqlPath;
	E.SourceSql = std::move(BodySql);
	E.SourceHash = HashProcedureSource(E.SourceSql);
	const auto Analysis = AnalyzeBytecode(Compiled.Instructions);
	E.ReferencedTables = Analysis.ReferencedTables;
	E.DependsOn = ScanProcedureCallsInSql(E.SourceSql);
	Catalog.Procedures.push_back(std::move(E));
	RebuildProcedureRelations(Catalog);
	return Catalog.Procedures.back();
}

std::string FormatProcedureEntrySummary(const StoredProcedureEntry &Entry) {
	std::ostringstream Out;
	Out << "name=" << Entry.Name << "\nabc=" << Entry.AbcPath.string() << "\n";
	if(!Entry.SqlPath.empty())
		Out << "sql=" << Entry.SqlPath.string() << "\n";
	if(!Entry.SourceHash.empty())
		Out << "source_hash=" << Entry.SourceHash << "\n";
	auto Emit = [&Out](const char *Label, const std::vector<std::string> &V) {
		Out << Label << "=";
		if(V.empty()) {
			Out << "(none)\n";
			return;
		}
		for(std::size_t I = 0; I < V.size(); ++I) {
			if(I)
				Out << ",";
			Out << V[I];
		}
		Out << "\n";
	};
	Emit("tables", Entry.ReferencedTables);
	Emit("depends_on", Entry.DependsOn);
	Emit("called_by", Entry.CalledBy);
	return Out.str();
}

std::string FormatProcedureRelations(const ProcedureCatalog &Catalog) {
	std::ostringstream Out;
	Out << "procedures=" << Catalog.Procedures.size() << "\n";
	for(const auto &[Abc, Names] : Catalog.AbcIndex) {
		Out << "abc\t" << Abc << "\t";
		for(std::size_t I = 0; I < Names.size(); ++I) {
			if(I)
				Out << ",";
			Out << Names[I];
		}
		Out << "\n";
	}
	for(const StoredProcedureEntry &E : Catalog.Procedures) {
		Out << "edge\t" << E.Name << "\tdepends\t";
		if(E.DependsOn.empty())
			Out << "(none)";
		else {
			for(std::size_t I = 0; I < E.DependsOn.size(); ++I) {
				if(I)
					Out << ",";
				Out << E.DependsOn[I];
			}
		}
		Out << "\n";
	}
	return Out.str();
}

} // namespace SQL
} // namespace AstralDB
