#include <SQL/JsonSql.hxx>

#include <DS/JSON.hxx>
#include <sstream>

namespace AstralDB {
namespace SQL {
namespace JsonSql {
namespace {

std::vector<std::string> SplitPath(std::string_view Path) {
	std::vector<std::string> Parts;
	std::string Cur;
	for(size_t I = 0; I < Path.size(); ++I) {
		const char C = Path[I];
		if(C == '.') {
			if(!Cur.empty())
				Parts.push_back(Cur);
			Cur.clear();
			continue;
		}
		if(C == '[') {
			if(!Cur.empty())
				Parts.push_back(Cur);
			Cur.clear();
			size_t J = I + 1;
			while(J < Path.size() && Path[J] != ']')
				++J;
			if(J >= Path.size())
				return {};
			Parts.push_back(std::string(Path.substr(I + 1, J - I - 1)));
			I = J;
			continue;
		}
		Cur.push_back(C);
	}
	if(!Cur.empty())
		Parts.push_back(Cur);
	return Parts;
}

} // namespace

std::optional<DS::JSON> ParseCellJson(std::string_view Cell) {
	if(Cell.empty())
		return std::nullopt;
	return DS::TryDecodeJSON(Cell);
}

std::optional<DS::JSON> ExtractPath(const DS::JSON &Root, std::string_view Path) {
	const auto Parts = SplitPath(Path);
	if(Parts.empty())
		return Root;
	const DS::JSON *Cur = &Root;
	for(const auto &P : Parts) {
		if(Cur->IsObject()) {
			const auto &O = Cur->AsObject();
			auto It = O.find(P);
			if(It == O.end())
				return std::nullopt;
			Cur = &It->second;
		} else if(Cur->IsArray()) {
			size_t Idx = 0;
			try {
				Idx = static_cast<size_t>(std::stoull(P));
			} catch(...) {
				return std::nullopt;
			}
			const auto &A = Cur->AsArray();
			if(Idx >= A.size())
				return std::nullopt;
			Cur = &A[Idx];
		} else
			return std::nullopt;
	}
	return *Cur;
}

std::string JsonCellToText(const DS::JSON &J) {
	if(J.IsNull())
		return {};
	if(J.IsString())
		return J.AsString();
	if(J.IsBool())
		return J.AsBool() ? "1" : "0";
	if(J.IsNumber()) {
		std::ostringstream O;
		O.precision(12);
		O << J.AsNumber();
		return std::move(O).str();
	}
	return DS::SerializeJSON(J);
}

} // namespace JsonSql
} // namespace SQL
} // namespace AstralDB
