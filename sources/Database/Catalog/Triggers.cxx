#include <Database/Catalog/Triggers.hxx>
#include <DS/JSON.hxx>
#include <IO/Error.hxx>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace AstralDB {
namespace {

static constexpr int kCatalogVersion = 1;
static constexpr std::size_t kMaxRecentFires = 128;

std::filesystem::path ResolveBesideCatalog(const std::filesystem::path &CatalogPath,
                                           const std::filesystem::path &Stored) {
	if(Stored.is_absolute())
		return Stored;
	return CatalogPath.parent_path() / Stored;
}

int64_t TimingToInt(TriggerTiming T) {
	switch(T) {
	case TriggerTiming::Before: return 0;
	case TriggerTiming::After: return 1;
	case TriggerTiming::InsteadOf: return 2;
	}
	return 1;
}

TriggerTiming IntToTiming(int64_t V) {
	switch(V) {
	case 0: return TriggerTiming::Before;
	case 2: return TriggerTiming::InsteadOf;
	default: return TriggerTiming::After;
	}
}

int64_t EventToInt(TriggerEvent E) {
	switch(E) {
	case TriggerEvent::Insert: return 0;
	case TriggerEvent::Update: return 1;
	case TriggerEvent::Delete: return 2;
	}
	return 0;
}

TriggerEvent IntToEvent(int64_t V) {
	switch(V) {
	case 1: return TriggerEvent::Update;
	case 2: return TriggerEvent::Delete;
	default: return TriggerEvent::Insert;
	}
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

const char *TriggerTimingTag(TriggerTiming T) {
	switch(T) {
	case TriggerTiming::Before:
		return "before";
	case TriggerTiming::After:
		return "after";
	case TriggerTiming::InsteadOf:
		return "instead_of";
	}
	return "after";
}

const char *TriggerEventTag(TriggerEvent E) {
	switch(E) {
	case TriggerEvent::Insert:
		return "insert";
	case TriggerEvent::Update:
		return "update";
	case TriggerEvent::Delete:
		return "delete";
	}
	return "insert";
}

std::optional<TriggerTiming> ParseTriggerTiming(std::string_view S) {
	std::string U(S);
	for(char &C : U)
		C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
	if(U == "BEFORE")
		return TriggerTiming::Before;
	if(U == "AFTER")
		return TriggerTiming::After;
	if(U == "INSTEAD OF" || U == "INSTEAD_OF")
		return TriggerTiming::InsteadOf;
	return std::nullopt;
}

std::optional<TriggerEvent> ParseTriggerEvent(std::string_view S) {
	std::string U(S);
	for(char &C : U)
		C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
	if(U == "INSERT")
		return TriggerEvent::Insert;
	if(U == "UPDATE")
		return TriggerEvent::Update;
	if(U == "DELETE")
		return TriggerEvent::Delete;
	return std::nullopt;
}

std::string HashTriggerSource(std::string_view BodySql) {
	return std::to_string(std::hash<std::string_view>{}(BodySql));
}

std::filesystem::path DefaultTriggerCatalogPath(const std::filesystem::path &SessionDbPath) {
	std::error_code Ec;
	const auto Db = std::filesystem::absolute(SessionDbPath, Ec);
	const auto Base = Ec || Db.empty() ? SessionDbPath : Db;
	const auto Parent = Base.parent_path();
	return (Parent.empty() ? std::filesystem::path(".") : Parent) / "astraldb_triggers.json";
}

std::filesystem::path DefaultTriggerCacheDir(const std::filesystem::path &SessionDbPath) {
	std::error_code Ec;
	const auto Db = std::filesystem::absolute(SessionDbPath, Ec);
	const auto Base = Ec || Db.empty() ? SessionDbPath : Db;
	const auto Parent = Base.parent_path();
	return (Parent.empty() ? std::filesystem::path(".") : Parent) / "astraldb_triggers_cache";
}

TriggerCatalog LoadTriggerCatalog(const std::filesystem::path &CatalogPath) {
	TriggerCatalog Catalog;
	Catalog.CatalogPath = CatalogPath;
	std::ifstream In(CatalogPath);
	if(!In)
		return Catalog;
	std::string Text((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
	if(Text.empty())
		return Catalog;
	const DS::JSON Root = DS::DecodeJSONStrict(Text);
	if(!Root.IsObject())
		throw std::runtime_error(Err::Prefixed("TRIG", "Trigger catalog root must be a JSON object."));
	const auto &Obj = Root.AsObject();
	if(const auto Ver = Obj.find("version"); Ver != Obj.end() && Ver->second.IsNumber()) {
		if(static_cast<int>(Ver->second.AsNumber()) != kCatalogVersion)
			throw std::runtime_error(Err::Prefixed("TRIG", "Unsupported trigger catalog version."));
	}
	const auto TrigIt = Obj.find("triggers");
	if(TrigIt != Obj.end() && TrigIt->second.IsObject()) {
		for(const auto &[Name, Entry] : TrigIt->second.AsObject()) {
			if(!Entry.IsObject())
				continue;
			StoredTriggerEntry E;
			E.Name = Name;
			const auto &EO = Entry.AsObject();
			if(const auto T = EO.find("table"); T != EO.end() && T->second.IsString())
				E.TableName = T->second.AsString();
			if(const auto Tim = EO.find("timing"); Tim != EO.end() && Tim->second.IsNumber())
				E.Timing = IntToTiming(static_cast<int64_t>(Tim->second.AsNumber()));
			if(const auto Ev = EO.find("event"); Ev != EO.end() && Ev->second.IsNumber())
				E.Event = IntToEvent(static_cast<int64_t>(Ev->second.AsNumber()));
			if(const auto Fer = EO.find("for_each_row"); Fer != EO.end() && Fer->second.IsBool())
				E.ForEachRow = Fer->second.AsBool();
			if(const auto En = EO.find("enabled"); En != EO.end() && En->second.IsBool())
				E.Enabled = En->second.AsBool();
			if(const auto Ak = EO.find("action_kind"); Ak != EO.end() && Ak->second.IsString())
				E.ActionKind = Ak->second.AsString();
			if(const auto Pn = EO.find("procedure"); Pn != EO.end() && Pn->second.IsString())
				E.ProcedureName = Pn->second.AsString();
			if(const auto Src = EO.find("source_sql"); Src != EO.end() && Src->second.IsString())
				E.BodySql = Src->second.AsString();
			if(const auto PathIt = EO.find("abc_path"); PathIt != EO.end() && PathIt->second.IsString())
				E.AbcPath = ResolveBesideCatalog(CatalogPath, PathIt->second.AsString());
			if(const auto SqlIt = EO.find("sql_path"); SqlIt != EO.end() && SqlIt->second.IsString())
				E.SqlPath = ResolveBesideCatalog(CatalogPath, SqlIt->second.AsString());
			if(const auto HashIt = EO.find("source_hash"); HashIt != EO.end() && HashIt->second.IsString())
				E.SourceHash = HashIt->second.AsString();
			if(!E.AbcPath.empty())
				Catalog.Triggers.push_back(std::move(E));
		}
	}
	const auto FireIt = Obj.find("recent_fires");
	if(FireIt != Obj.end() && FireIt->second.IsArray()) {
		for(const auto &F : FireIt->second.AsArray()) {
			if(!F.IsObject())
				continue;
			TriggerFireRecord R;
			const auto &FO = F.AsObject();
			if(const auto N = FO.find("trigger"); N != FO.end() && N->second.IsString())
				R.TriggerName = N->second.AsString();
			if(const auto T = FO.find("table"); T != FO.end() && T->second.IsString())
				R.TableName = T->second.AsString();
			if(const auto Tim = FO.find("timing"); Tim != FO.end() && Tim->second.IsString())
				R.Timing = Tim->second.AsString();
			if(const auto Ev = FO.find("event"); Ev != FO.end() && Ev->second.IsString())
				R.Event = Ev->second.AsString();
			if(const auto Ms = FO.find("ms"); Ms != FO.end() && Ms->second.IsNumber())
				R.FiredAtUnixMs = static_cast<std::uint64_t>(Ms->second.AsNumber());
			if(const auto Ok = FO.find("ok"); Ok != FO.end() && Ok->second.IsBool())
				R.Ok = Ok->second.AsBool();
			if(const auto D = FO.find("detail"); D != FO.end() && D->second.IsString())
				R.Detail = D->second.AsString();
			Catalog.RecentFires.push_back(std::move(R));
		}
	}
	RebuildTriggerRelations(Catalog);
	return Catalog;
}

void SaveTriggerCatalog(const TriggerCatalog &Catalog) {
	DS::JSONObject Root;
	Root.emplace("version", DS::JSON(kCatalogVersion));
	DS::JSONObject Trigs;
	const auto RelPath = [&](const std::filesystem::path &P) {
		std::error_code Ec;
		const auto R = std::filesystem::relative(P, Catalog.CatalogPath.parent_path(), Ec);
		return Ec ? P.string() : R.string();
	};
	for(const StoredTriggerEntry &E : Catalog.Triggers) {
		DS::JSONObject Entry;
		Entry.emplace("table", DS::JSON(E.TableName));
		Entry.emplace("timing", DS::JSON(static_cast<double>(TimingToInt(E.Timing))));
		Entry.emplace("event", DS::JSON(static_cast<double>(EventToInt(E.Event))));
		Entry.emplace("for_each_row", DS::JSON(E.ForEachRow));
		Entry.emplace("enabled", DS::JSON(E.Enabled));
		Entry.emplace("action_kind", DS::JSON(E.ActionKind));
		if(!E.ProcedureName.empty())
			Entry.emplace("procedure", DS::JSON(E.ProcedureName));
		if(!E.BodySql.empty())
			Entry.emplace("source_sql", DS::JSON(E.BodySql));
		if(!E.SourceHash.empty())
			Entry.emplace("source_hash", DS::JSON(E.SourceHash));
		if(!E.AbcPath.empty())
			Entry.emplace("abc_path", DS::JSON(RelPath(E.AbcPath)));
		if(!E.SqlPath.empty())
			Entry.emplace("sql_path", DS::JSON(RelPath(E.SqlPath)));
		if(E.BytecodeMeta.InstructionCount > 0)
			Entry.emplace("bytecode_instructions", DS::JSON(static_cast<double>(E.BytecodeMeta.InstructionCount)));
		SaveJsonStringArray(Entry, "calls_procedures", E.BytecodeMeta.CalledProcedures);
		Trigs.emplace(E.Name, DS::JSON(std::move(Entry)));
	}
	Root.emplace("triggers", DS::JSON(std::move(Trigs)));
	DS::JSONArray Fires;
	for(const auto &F : Catalog.RecentFires) {
		DS::JSONObject FO;
		FO.emplace("trigger", DS::JSON(F.TriggerName));
		FO.emplace("table", DS::JSON(F.TableName));
		FO.emplace("timing", DS::JSON(F.Timing));
		FO.emplace("event", DS::JSON(F.Event));
		FO.emplace("ms", DS::JSON(static_cast<double>(F.FiredAtUnixMs)));
		FO.emplace("ok", DS::JSON(F.Ok));
		if(!F.Detail.empty())
			FO.emplace("detail", DS::JSON(F.Detail));
		Fires.push_back(DS::JSON(std::move(FO)));
	}
	Root.emplace("recent_fires", DS::JSON(std::move(Fires)));
	{
		std::error_code Ec;
		std::filesystem::create_directories(Catalog.CatalogPath.parent_path(), Ec);
	}
	std::ofstream Out(Catalog.CatalogPath);
	if(!Out)
		throw std::runtime_error(
		    Err::Prefixed("TRIG", "Cannot write trigger catalog: " + Catalog.CatalogPath.string()));
	Out << DS::SerializeJSON(DS::JSON(Root));
}

void RebuildTriggerRelations(TriggerCatalog &Catalog) {
	Catalog.TableIndex.clear();
	for(const StoredTriggerEntry &E : Catalog.Triggers)
		Catalog.TableIndex[E.TableName].push_back(E.Name);
}

std::optional<StoredTriggerEntry> FindTrigger(const TriggerCatalog &Catalog, std::string_view Name) {
	for(const StoredTriggerEntry &E : Catalog.Triggers)
		if(E.Name == Name)
			return E;
	return std::nullopt;
}
void AppendTriggerFireRecord(TriggerCatalog &Catalog, TriggerFireRecord Rec) {
	Catalog.RecentFires.push_back(std::move(Rec));
	if(Catalog.RecentFires.size() > kMaxRecentFires)
		Catalog.RecentFires.erase(Catalog.RecentFires.begin(),
		                          Catalog.RecentFires.begin() + (Catalog.RecentFires.size() - kMaxRecentFires));
}

std::string FormatTriggerEntrySummary(const StoredTriggerEntry &Entry) {
	std::ostringstream Out;
	Out << "name=" << Entry.Name << "\n";
	Out << "table=" << Entry.TableName << "\n";
	Out << "timing=" << TriggerTimingTag(Entry.Timing) << "\n";
	Out << "event=" << TriggerEventTag(Entry.Event) << "\n";
	Out << "for_each_row=" << (Entry.ForEachRow ? "yes" : "no") << "\n";
	Out << "enabled=" << (Entry.Enabled ? "yes" : "no") << "\n";
	Out << "action_kind=" << Entry.ActionKind << "\n";
	if(!Entry.ProcedureName.empty())
		Out << "procedure=" << Entry.ProcedureName << "\n";
	Out << "abc=" << Entry.AbcPath.string() << "\n";
	if(!Entry.SqlPath.empty())
		Out << "sql=" << Entry.SqlPath.string() << "\n";
	if(!Entry.SourceHash.empty())
		Out << "source_hash=" << Entry.SourceHash << "\n";
	Out << "bytecode_instructions=" << Entry.BytecodeMeta.InstructionCount << "\n";
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
	Emit("calls_procedures", Entry.BytecodeMeta.CalledProcedures);
	return Out.str();
}

std::string FormatTriggerRelations(const TriggerCatalog &Catalog) {
	std::ostringstream Out;
	Out << "triggers=" << Catalog.Triggers.size() << "\n";
	for(const auto &[Table, Names] : Catalog.TableIndex) {
		Out << "table\t" << Table << "\t";
		for(std::size_t I = 0; I < Names.size(); ++I) {
			if(I)
				Out << ",";
			Out << Names[I];
		}
		Out << "\n";
	}
	return Out.str();
}

std::string FormatTriggerFireLog(const TriggerCatalog &Catalog) {
	std::ostringstream Out;
	Out << "recent_fires=" << Catalog.RecentFires.size() << "\n";
	for(const auto &F : Catalog.RecentFires) {
		Out << "fire\t" << F.TriggerName << "\t" << F.TableName << "\t" << F.Timing << "\t" << F.Event << "\t"
		    << (F.Ok ? "ok" : "err") << "\t" << F.FiredAtUnixMs;
		if(!F.Detail.empty())
			Out << "\t" << F.Detail;
		Out << "\n";
	}
	return Out.str();
}

std::string SerializeTriggerRegistry(const std::vector<StoredTriggerEntry> &Triggers) {
	DS::JSONObject Root;
	DS::JSONArray Arr;
	for(const StoredTriggerEntry &E : Triggers) {
		DS::JSONObject O;
		O.emplace("name", DS::JSON(E.Name));
		O.emplace("table", DS::JSON(E.TableName));
		O.emplace("timing", DS::JSON(static_cast<double>(TimingToInt(E.Timing))));
		O.emplace("event", DS::JSON(static_cast<double>(EventToInt(E.Event))));
		O.emplace("for_each_row", DS::JSON(E.ForEachRow));
		O.emplace("enabled", DS::JSON(E.Enabled));
		O.emplace("action_kind", DS::JSON(E.ActionKind));
		if(!E.ProcedureName.empty())
			O.emplace("procedure", DS::JSON(E.ProcedureName));
		if(!E.BodySql.empty())
			O.emplace("body_sql", DS::JSON(E.BodySql));
		Arr.push_back(DS::JSON(std::move(O)));
	}
	Root.emplace("triggers", DS::JSON(std::move(Arr)));
	return DS::SerializeJSON(DS::JSON(Root));
}

bool DeserializeTriggerRegistry(std::string_view Blob, std::vector<StoredTriggerEntry> &Out) {
	Out.clear();
	if(Blob.empty())
		return true;
	const DS::JSON Root = DS::DecodeJSONStrict(std::string(Blob));
	if(!Root.IsObject())
		return false;
	const auto It = Root.AsObject().find("triggers");
	if(It == Root.AsObject().end() || !It->second.IsArray())
		return true;
	for(const auto &Item : It->second.AsArray()) {
		if(!Item.IsObject())
			continue;
		const auto &O = Item.AsObject();
		StoredTriggerEntry E;
		if(const auto N = O.find("name"); N != O.end() && N->second.IsString())
			E.Name = N->second.AsString();
		if(const auto T = O.find("table"); T != O.end() && T->second.IsString())
			E.TableName = T->second.AsString();
		if(const auto Tim = O.find("timing"); Tim != O.end() && Tim->second.IsNumber())
			E.Timing = IntToTiming(static_cast<int64_t>(Tim->second.AsNumber()));
		if(const auto Ev = O.find("event"); Ev != O.end() && Ev->second.IsNumber())
			E.Event = IntToEvent(static_cast<int64_t>(Ev->second.AsNumber()));
		if(const auto Fer = O.find("for_each_row"); Fer != O.end() && Fer->second.IsBool())
			E.ForEachRow = Fer->second.AsBool();
		if(const auto En = O.find("enabled"); En != O.end() && En->second.IsBool())
			E.Enabled = En->second.AsBool();
		if(const auto Ak = O.find("action_kind"); Ak != O.end() && Ak->second.IsString())
			E.ActionKind = Ak->second.AsString();
		if(const auto Pn = O.find("procedure"); Pn != O.end() && Pn->second.IsString())
			E.ProcedureName = Pn->second.AsString();
		if(const auto B = O.find("body_sql"); B != O.end() && B->second.IsString())
			E.BodySql = B->second.AsString();
		if(!E.Name.empty())
			Out.push_back(std::move(E));
	}
	return true;
}
} // namespace AstralDB
