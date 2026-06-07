#include <SQL/MatchRecognize.hxx>

#include <SQL/Bytecode.hxx>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace AstralDB {
namespace SQL {
namespace {

struct PatToken {
	std::string Sym;
	char Quant = 0;
};

std::vector<PatToken> TokenizePattern(std::string_view Pat) {
	std::vector<PatToken> Out;
	std::string Cur;
	auto Flush = [&]() {
		if(Cur.empty())
			return;
		char Q = 0;
		if(!Cur.empty() && (Cur.back() == '+' || Cur.back() == '*' || Cur.back() == '?')) {
			Q = Cur.back();
			Cur.pop_back();
		}
		if(!Cur.empty())
			Out.push_back({Cur, Q});
		Cur.clear();
	};
	for(char C : Pat) {
		if(std::isspace(static_cast<unsigned char>(C))) {
			Flush();
			continue;
		}
		Cur.push_back(C);
	}
	Flush();
	return Out;
}

bool RowMatchesSymbol(const Database *Db, const Database::Item &Row, const MatchRecognizeDefine &Def) {
	if(Def.PredicatePackedDnf.empty())
		return true;
	return EvaluatePackedWhereDnf(Db, Row, Def.PredicatePackedDnf);
}

std::vector<std::vector<std::string>> LabelRows(const Database *Db, const std::vector<Database::Item> &Rows,
                                                const std::vector<MatchRecognizeDefine> &Defines) {
	std::vector<std::vector<std::string>> Labels(Rows.size());
	for(size_t I = 0; I < Rows.size(); ++I) {
		for(const auto &D : Defines) {
			if(RowMatchesSymbol(Db, Rows[I], D))
				Labels[I].push_back(D.Symbol);
		}
	}
	return Labels;
}

bool MatchHere(const std::vector<PatToken> &Pat, const std::vector<std::vector<std::string>> &Labels, size_t Row,
               size_t Tok, std::unordered_set<size_t> &UsedRows) {
	if(Tok >= Pat.size())
		return Row >= Labels.size();
	if(Row >= Labels.size())
		return false;
	const PatToken &T = Pat[Tok];
	const auto HasSym = [&](size_t R) {
		for(const auto &S : Labels[R]) {
			if(S == T.Sym)
				return true;
		}
		return false;
	};
	if(T.Quant == 0 || T.Quant == '?') {
		if(HasSym(Row)) {
			UsedRows.insert(Row);
			if(MatchHere(Pat, Labels, Row + 1, Tok + 1, UsedRows))
				return true;
			UsedRows.erase(Row);
		}
		if(T.Quant == '?') {
			if(MatchHere(Pat, Labels, Row, Tok + 1, UsedRows))
				return true;
		}
		return false;
	}
	if(T.Quant == '+') {
		if(!HasSym(Row))
			return false;
		size_t R = Row;
		while(R < Labels.size() && HasSym(R)) {
			UsedRows.insert(R);
			if(MatchHere(Pat, Labels, R + 1, Tok + 1, UsedRows))
				return true;
			++R;
		}
		for(size_t U = Row; U < R; ++U)
			UsedRows.erase(U);
		return false;
	}
	if(T.Quant == '*') {
		size_t R = Row;
		while(R < Labels.size() && HasSym(R)) {
			UsedRows.insert(R);
			if(MatchHere(Pat, Labels, R + 1, Tok + 1, UsedRows))
				return true;
			++R;
		}
		for(size_t U = Row; U < R; ++U)
			UsedRows.erase(U);
		if(MatchHere(Pat, Labels, Row, Tok + 1, UsedRows))
			return true;
		return false;
	}
	return false;
}

} // namespace

void RunMatchRecognize(Database &Db, const std::string &SourceTable, const MatchRecognizeSpec &Spec) {
	auto It = Db.Tables_.find(SourceTable);
	if(It == Db.Tables_.end())
		return;
	std::vector<Database::Item> Rows = It->second.RowStore;
	std::sort(Rows.begin(), Rows.end(), [&](const Database::Item &A, const Database::Item &B) {
		auto AIt = A.find(Spec.OrderColumn);
		auto BIt = B.find(Spec.OrderColumn);
		const std::string Av = AIt == A.end() ? std::string{} : AIt->second;
		const std::string Bv = BIt == B.end() ? std::string{} : BIt->second;
		return Av < Bv;
	});
	const auto Pat = TokenizePattern(Spec.Pattern);
	const auto Labels = LabelRows(&Db, Rows, Spec.Defines);
	std::unordered_set<size_t> Used;
	for(size_t Start = 0; Start < Rows.size(); ++Start) {
		std::unordered_set<size_t> Local;
		if(MatchHere(Pat, Labels, Start, 0, Local)) {
			for(size_t U : Local)
				Used.insert(U);
		}
	}
	std::vector<Database::Item> Out;
	Out.reserve(Used.size());
	for(size_t I = 0; I < Rows.size(); ++I) {
		if(Used.count(I))
			Out.push_back(Rows[I]);
	}
	It->second.RowStore = std::move(Out);
}

} // namespace SQL
} // namespace AstralDB
