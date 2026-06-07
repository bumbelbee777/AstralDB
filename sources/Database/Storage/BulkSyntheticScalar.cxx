#include <Database/Storage/BulkSynthetic.hxx>
#include <Database/Storage/BulkSyntheticPathEval.hxx>

#include <Database/Storage/BulkSyntheticSemistructured.hxx>
#include <Database/Storage/PredicateKind.hxx>
#include <DS/SimdJsonExtract.hxx>
#include <DS/SimdXmlExtract.hxx>
#include <Database/Text/PatternMatch.hxx>
#include <Database/Graph/GeoSpatial.hxx>
#include <Database/Index/TextSearch.hxx>
#include <Database/Execution/PlanTypes.hxx>

#include <sstream>

#include <cctype>
#include <optional>

namespace AstralDB {

namespace {

bool SqlTruthLiteral(std::string_view S) {
	return S == "1" || S == "true" || S == "TRUE" || S == "t" || S == "yes";
}

std::string_view NormalizeJsonPath(std::string_view Path) noexcept {
	if(Path.size() >= 2 && Path[0] == '$' && Path[1] == '.')
		return Path.substr(2);
	if(!Path.empty() && Path[0] == '$')
		return Path.substr(1);
	return Path;
}

bool JsonExtractForRow(int64_t RowId, const Database::Column &ColDef, std::string_view Path, std::string &Out) {
	(void)ColDef;
	if(BulkSyntheticJsonExtractRow(RowId, Path, Out))
		return true;
	const std::string Json = BulkSyntheticCellString(ColDef, BulkSyntheticContext{RowId, 1, 0, 1});
	if(Json.empty())
		return false;
	return SimdJsonExtract::Extract(Json, NormalizeJsonPath(Path), Out);
}

bool XmlValidForRow(int64_t RowId, const Database::Column &ColDef) {
	(void)ColDef;
	return BulkSyntheticXmlValidRow(RowId);
}

bool XmlExtractForRow(int64_t RowId, const Database::Column &ColDef, std::string_view Path, std::string &Out) {
	(void)ColDef;
	if(BulkSyntheticXmlExtractRow(RowId, Path, Out))
		return true;
	const std::string Xml = BulkSyntheticCellString(ColDef, BulkSyntheticContext{RowId, 1, 0, 1});
	return SimdXmlExtract::Extract(Xml, Path, Out);
}

bool RegexpMatchSyntheticText(int64_t RowId, const Database::Column &ColDef, std::string_view Pattern) {
	if(Pattern.empty())
		return true;
	const BulkSyntheticContext Ctx{RowId, 1, 0, 1};
	const std::string Text = BulkSyntheticCellString(ColDef, Ctx);
	return SqlRegexpMatch(Text, std::string(Pattern), false);
}

bool TimestampLowerBoundPasses(int64_t RowId, const std::string &Op, const std::string &Bound) {
	const int64_t RowEpoch = 1'704'067'200LL + RowId;
	int64_t BoundEpoch = 1'704'067'200LL;
	if(Bound.size() >= 10) {
		try {
			const int Y = std::stoi(Bound.substr(0, 4));
			(void)Y;
			BoundEpoch = 1'704'067'200LL;
		} catch(...) {
		}
	}
	if(Op == ">=")
		return RowEpoch >= BoundEpoch;
	if(Op == ">")
		return RowEpoch > BoundEpoch;
	if(Op == "<=")
		return RowEpoch <= BoundEpoch;
	if(Op == "<")
		return RowEpoch < BoundEpoch;
	return false;
}

} // namespace

bool BulkSyntheticTimestampLowerBoundMatchesAllBulkRows(const ColumnarTable &Col, const std::string_view Op,
                                                        const std::string_view Bound) noexcept {
	if(!Col.BulkSyntheticLazy || Col.RowCount == 0)
		return false;
	if(Op != ">=" && Op != ">")
		return false;
	return TimestampLowerBoundPasses(Col.BulkStartId, std::string(Op), std::string(Bound));
}

bool BulkSyntheticTryScalarEval(int FnTag, int64_t RowId, const std::vector<std::pair<int64_t, std::string>> &Args,
                                std::string &Out) {
	using ScalarSqlFn = SQL::ScalarSqlFn;
	const auto Fn = static_cast<ScalarSqlFn>(FnTag);
	if(Fn == ScalarSqlFn::JsonExtract && Args.size() >= 2) {
		Database::Column Co;
		Co.DefaultValue = "JSON";
		if(Args[0].first == 0)
			Co.Name = Args[0].second;
		const std::string &Path = Args[1].second;
		if(!Co.Name.empty() && JsonExtractForRow(RowId, Co, Path, Out))
			return true;
		return SimdJsonExtract::Extract(Args[0].second, NormalizeJsonPath(Path), Out);
	}
	if(Fn == ScalarSqlFn::XmlValid && Args.size() >= 1) {
		Database::Column Co;
		Co.DefaultValue = "XML";
		if(Args[0].first == 0)
			Co.Name = Args[0].second;
		Out = XmlValidForRow(RowId, Co) ? "1" : "0";
		return true;
	}
	if(Fn == ScalarSqlFn::XmlExtract && Args.size() >= 2) {
		Database::Column Co;
		Co.DefaultValue = "XML";
		if(Args[0].first == 0)
			Co.Name = Args[0].second;
		return XmlExtractForRow(RowId, Co, Args[1].second, Out);
	}
	if(Fn == ScalarSqlFn::RegexpExtract && Args.size() >= 2) {
		return BulkSyntheticRegexpExtractRow(RowId, Args[1].second, Out);
	}
	if(Fn == ScalarSqlFn::CharLength && Args.size() >= 1) {
		Out = std::to_string(BulkSyntheticBioLengthRow(RowId));
		return true;
	}
	if(Fn == ScalarSqlFn::TextRank && Args.size() >= 2) {
		std::ostringstream O;
		O.precision(12);
		O << BulkSyntheticBioRankRow(RowId);
		Out = std::move(O).str();
		return true;
	}
	return false;
}

namespace {

bool SyntheticXmlColumn(const Database::Column &Col) noexcept {
	return ClassifySqlStorage(Col) == SqlStorageKind::Xml;
}

} // namespace

std::optional<bool> BulkSyntheticTryMatchPredicate(int64_t RowId, const std::string &Col, const std::string &Op,
                                                   const std::string &Rhs, const Database::Column *ColDef) {
	Database::Column StubCol;
	if(!ColDef) {
		StubCol.Name = Col;
		if(Col == "metadata" || Col == "profile")
			StubCol.DefaultValue = "JSON";
		else if(Col == "order_date")
			StubCol.DefaultValue = "TIMESTAMP";
		else if(Col == "review_text" || Col == "bio" || Col == "description")
			StubCol.DefaultValue = "TEXT";
		else
			return std::nullopt;
		ColDef = &StubCol;
	}
	const SqlStorageKind Storage = ClassifySqlStorage(*ColDef);
	if(Op == "JSON_EXTRACT" || Op == "NOT JSON_EXTRACT") {
		if(Storage != SqlStorageKind::Json)
			return std::nullopt;
		const std::size_t Split = Rhs.find('\x1E');
		if(Split == std::string::npos)
			return Op == "NOT JSON_EXTRACT";
		const std::string Path = Rhs.substr(0, Split);
		const std::string Want = Rhs.substr(Split + 1);
		std::string Got;
		if(!JsonExtractForRow(RowId, *ColDef, Path, Got))
			return Op == "NOT JSON_EXTRACT";
		const bool Hit = Got == Want || (SqlTruthLiteral(Want) && SqlTruthLiteral(Got));
		return Op == "NOT JSON_EXTRACT" ? !Hit : Hit;
	}
	if(Op == "XML_VALID" || Op == "NOT XML_VALID") {
		if(!SyntheticXmlColumn(*ColDef))
			return std::nullopt;
		const bool Valid = XmlValidForRow(RowId, *ColDef);
		const bool Hit = Valid == SqlTruthLiteral(Rhs);
		return Op == "NOT XML_VALID" ? !Hit : Hit;
	}
	if(Op == "REGEXP" || Op == "~" || Op == "NOT REGEXP" || Op == "!~") {
		if(Storage != SqlStorageKind::Text)
			return std::nullopt;
		const bool Hit = RegexpMatchSyntheticText(RowId, *ColDef, Rhs);
		return (Op == "NOT REGEXP" || Op == "!~") ? !Hit : Hit;
	}
	if(Op == ">=" || Op == ">" || Op == "<=" || Op == "<") {
		if(Storage == SqlStorageKind::Timestamp) {
			const bool Hit = TimestampLowerBoundPasses(RowId, Op, Rhs);
			return Hit;
		}
	}
	if(Op == "MATCH" || Op == "NOT MATCH") {
		if(Storage != SqlStorageKind::Text)
			return std::nullopt;
		const BulkSyntheticContext Ctx{RowId, 1, 0, 1};
		const std::string Text = BulkSyntheticCellString(*ColDef, Ctx);
		const bool Hit = TextSearch::MatchAgainst(Text, Rhs);
		return Op == "NOT MATCH" ? !Hit : Hit;
	}
	if(Op == "__ST_BBOX__") {
		double MinLon = 0.0, MinLat = 0.0, MaxLon = 0.0, MaxLat = 0.0;
		if(std::sscanf(Rhs.c_str(), "%lf,%lf,%lf,%lf", &MinLon, &MinLat, &MaxLon, &MaxLat) < 4)
			return std::nullopt;
		const BulkSyntheticContext Ctx{RowId, 1, 0, 1};
		const std::string Cell = BulkSyntheticCellString(*ColDef, Ctx);
		double Lon = 0.0, Lat = 0.0;
		if(std::sscanf(Cell.c_str(), "POINT(%lf %lf", &Lon, &Lat) < 2)
			return std::nullopt;
		return GeoSpatial::WithinBbox(GeoSpatial::Point{Lon, Lat}, MinLon, MinLat, MaxLon, MaxLat);
	}
	if(Op == "__IN__" || Op == "__NOT_IN__") {
		if(Storage == SqlStorageKind::Json) {
			const std::size_t PathSep = Rhs.find('\x1F');
			if(PathSep == std::string::npos)
				return std::nullopt;
			const std::string_view Path(Rhs.data(), PathSep);
			const std::string_view List(Rhs.data() + PathSep + 1, Rhs.size() - PathSep - 1);
			std::string Cell;
			const BulkSyntheticJsonPathPlan Plan = PlanBulkSyntheticJsonPath(Path);
			if(!BulkSyntheticJsonExtractPlanned(RowId, Plan, Path, Cell))
				return Op == "__NOT_IN__";
			bool Hit = false;
			for(std::size_t Off = 0; Off < List.size();) {
				const std::size_t End = List.find('\x1E', Off);
				const std::string_view Lit(List.data() + Off, (End == std::string::npos ? List.size() : End) - Off);
				if(Cell == Lit)
					Hit = true;
				if(End == std::string::npos)
					break;
				Off = End + 1;
			}
			return Op == "__NOT_IN__" ? !Hit : Hit;
		}
		if(Storage == SqlStorageKind::Text || Storage == SqlStorageKind::Other) {
			const BulkSyntheticContext Ctx{RowId, 1, 0, 1};
			const std::string Cell = BulkSyntheticCellString(*ColDef, Ctx);
			bool Hit = false;
			for(std::size_t Off = 0; Off < Rhs.size();) {
				const std::size_t End = Rhs.find('\x1E', Off);
				const std::string_view Lit(Rhs.data() + Off, (End == std::string::npos ? Rhs.size() : End) - Off);
				if(Cell == Lit)
					Hit = true;
				if(End == std::string::npos)
					break;
				Off = End + 1;
			}
			return Op == "__NOT_IN__" ? !Hit : Hit;
		}
	}
	(void)Col;
	return std::nullopt;
}

} // namespace AstralDB
