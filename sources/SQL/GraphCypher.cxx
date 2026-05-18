#include <SQL/SQL.hxx>
#include <IO/Limits.hxx>

namespace AstralDB {
namespace SQL {

std::unique_ptr<StatementAST> Parser::ParseCypherMatchStatement() {
	AdvanceToken();
	if(!MatchToken(TokenType::PUNCTUATION, "("))
		ParseFail("MATCH expects '(' pattern.");
	auto A = CurrentToken();
	if(!A || A->Type != TokenType::IDENTIFIER)
		ParseFail("MATCH pattern expects source vertex variable.");
	AdvanceToken();
	if(!MatchToken(TokenType::PUNCTUATION, ")"))
		ParseFail("MATCH pattern: expected ')' after source vertex.");
	bool Reverse = false;
	if(MatchToken(TokenType::PUNCTUATION, "<")) {
		if(!MatchToken(TokenType::PUNCTUATION, "-"))
			ParseFail("MATCH pattern: expected '<-['.");
		Reverse = true;
	} else if(!MatchToken(TokenType::PUNCTUATION, "-"))
		ParseFail("MATCH pattern: expected '-[' or '<-['.");
	if(!MatchToken(TokenType::PUNCTUATION, "["))
		ParseFail("MATCH pattern: expected '['.");
	auto E = CurrentToken();
	if(!E || E->Type != TokenType::IDENTIFIER)
		ParseFail("MATCH pattern expects edge variable.");
	AdvanceToken();
	int64_t MinHops = 1;
	int64_t MaxHops = 1;
	if(MatchToken(TokenType::PUNCTUATION, "*")) {
		MinHops = 1;
		MaxHops = static_cast<int64_t>(Limits::MaxGraphTraverseDepth);
		auto Ht = CurrentToken();
		if(Ht && Ht->Type == TokenType::LITERAL) {
			const std::string &V = Ht->Value;
			const size_t Dot = V.find("..");
			if(Dot != std::string::npos) {
				MinHops = std::stoll(V.substr(0, Dot));
				MaxHops = std::stoll(V.substr(Dot + 2));
			} else {
				MinHops = std::stoll(V);
				MaxHops = MinHops;
			}
			AdvanceToken();
			if(Dot == std::string::npos && MatchToken(TokenType::PUNCTUATION, ".") &&
			   MatchToken(TokenType::PUNCTUATION, ".")) {
				auto Ht2 = CurrentToken();
				if(!Ht2 || Ht2->Type != TokenType::LITERAL)
					ParseFail("MATCH *m..n expects max hop literal.");
				MaxHops = std::stoll(Ht2->Value);
				AdvanceToken();
			}
		}
	}
	if(!MatchToken(TokenType::PUNCTUATION, "]"))
		ParseFail("MATCH pattern: expected ']' after edge.");
	if(Reverse) {
		if(!MatchToken(TokenType::PUNCTUATION, "-"))
			ParseFail("MATCH pattern: expected '-'.");
	} else {
		if(!MatchToken(TokenType::PUNCTUATION, "-"))
			ParseFail("MATCH pattern: expected '->'.");
		if(!MatchToken(TokenType::PUNCTUATION, ">"))
			ParseFail("MATCH pattern: expected '>'.");
	}
	if(!MatchToken(TokenType::PUNCTUATION, "("))
		ParseFail("MATCH pattern: expected '(' for target vertex.");
	auto B = CurrentToken();
	if(!B || B->Type != TokenType::IDENTIFIER)
		ParseFail("MATCH pattern expects target vertex variable.");
	AdvanceToken();
	if(!MatchToken(TokenType::PUNCTUATION, ")"))
		ParseFail("MATCH pattern: expected ')' after target vertex.");
	if(!MatchKeyword("IN"))
		ParseFail("MATCH requires IN graph_name.");
	auto Gn = CurrentToken();
	if(!Gn || Gn->Type != TokenType::IDENTIFIER)
		ParseFail("Expected graph name after IN.");
	std::string GraphName = Gn->Value;
	AdvanceToken();
	std::string LabelFilter;
	if(MatchKeyword("WHERE")) {
		auto Col = CurrentToken();
		if(!Col || Col->Type != TokenType::IDENTIFIER)
			ParseFail("MATCH WHERE expects edge.label = 'value'.");
		AdvanceToken();
		if(!MatchToken(TokenType::PUNCTUATION, "."))
			ParseFail("MATCH WHERE expects edge.label = 'value'.");
		auto Lc = CurrentToken();
		if(!Lc || Lc->Type != TokenType::IDENTIFIER)
			ParseFail("MATCH WHERE expects label column name.");
		(void)Lc;
		AdvanceToken();
		if(!MatchToken(TokenType::PUNCTUATION, "="))
			ParseFail("MATCH WHERE expects '='.");
		auto Lit = CurrentToken();
		if(!Lit || Lit->Type != TokenType::LITERAL)
			ParseFail("MATCH WHERE expects string literal.");
		LabelFilter = Lit->Value;
		AdvanceToken();
	}
	std::string Anchor;
	if(MatchKeyword("FROM")) {
		auto St = CurrentToken();
		if(!St || (St->Type != TokenType::LITERAL && St->Type != TokenType::IDENTIFIER))
			ParseFail("MATCH FROM expects vertex id.");
		Anchor = St->Value;
		AdvanceToken();
	}
	if(MatchKeyword("RETURN")) {
		for(;;) {
			auto T = CurrentToken();
			if(!T)
				ParseFail("MATCH RETURN list incomplete.");
			if(T->Type == TokenType::KEYWORD && T->Value == "INTO")
				break;
			AdvanceToken();
		}
	}
	if(!MatchKeyword("INTO"))
		ParseFail("MATCH requires INTO result_table (Cypher: … RETURN … INTO table).");
	auto Rt = CurrentToken();
	if(!Rt || Rt->Type != TokenType::IDENTIFIER)
		ParseFail("Expected result table name after INTO.");
	std::string Result = Rt->Value;
	AdvanceToken();
	return std::make_unique<GraphMatchAST>(std::move(GraphName), std::move(LabelFilter), MinHops, MaxHops,
	                                       std::move(Anchor), Reverse, std::move(Result));
}

} // namespace SQL
} // namespace AstralDB
