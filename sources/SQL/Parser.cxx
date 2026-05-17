#include <Database/AdvancedTypes.hxx>
#include <Database/MathSci.hxx>
#include <SQL/SQL.hxx>
#include <Database/HybridStorageScheduler.hxx>
#include <Database/User.hxx>
#include <IO/Error.hxx>
#include <IO/Limits.hxx>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace AstralDB {
namespace SQL {

namespace {
	bool GParserDiagnostics = true;

	template<typename T>
	std::unique_ptr<ExpressionAST> AsExpr(std::unique_ptr<T> &&Ptr) {
		return std::unique_ptr<ExpressionAST>(static_cast<ExpressionAST *>(Ptr.release()));
	}

	[[noreturn]] inline void ThrowMsg(std::string Msg) {
		throw std::runtime_error(std::move(Msg));
	}

	std::unique_ptr<ExpressionAST> CloneCaseScalarExpr(const ExpressionAST *E) {
		if(const auto *L = dynamic_cast<const LiteralAST *>(E))
			return std::make_unique<LiteralAST>(L->Value);
		if(const auto *C = dynamic_cast<const ColumnRefAST *>(E))
			return std::make_unique<ColumnRefAST>(C->Name);
		if(dynamic_cast<const NullLiteralAST *>(E))
			return std::make_unique<NullLiteralAST>();
		return nullptr;
	}
} // namespace

bool ParserDiagnosticsEnabled() {
	return GParserDiagnostics;
}

void SetParserDiagnostics(bool Enabled) {
	GParserDiagnostics = Enabled;
}

void Parser::LexFail(std::size_t ByteOffset, std::string Message) const {
	ThrowMsg(Err::FormatSqlLex(std::move(Message), Query_, ByteOffset));
}

void Parser::ParseFail(std::string Message) const {
	std::size_t Off = Query_.size();
	std::optional<std::string_view> Near;
	if(CurrentIndex_ < Tokens_.size()) {
		Off = Tokens_[CurrentIndex_].Begin;
		Near = std::string_view(Tokens_[CurrentIndex_].Value);
	} else if(!Tokens_.empty()) {
		const Token &T = Tokens_.back();
		Off = std::min(T.Begin + T.Value.size(), Query_.size());
	}
	ThrowMsg(Err::FormatSqlParse(std::move(Message), Query_, Off, Near));
}

Parser::Parser(std::string_view Query) : Query_(Query) {
	if(Query_.size() > Limits::MaxSqlSourceBytes)
		LexFail(0, "SQL input exceeds AstralDB size guard (rejecting it to avoid exhausting host memory).");
	if(ParserDiagnosticsEnabled()) {
		std::cout << "[Parser] Initializing with query:\n\"" << Query << "\"\n";
		std::cout << "[Tokenizer] Starting tokenization...\n";
	}
	Tokens_ = Tokenize();
	if(ParserDiagnosticsEnabled()) {
		std::cout << "[Tokenizer] Produced " << Tokens_.size() << " tokens:\n";
		for(const auto &token : Tokens_)
			std::cout << "[" << static_cast<int>(token.Type) << ": " << token.Value << "] ";
		std::cout << "\n";
	}
	CurrentIndex_ = 0;
	try {
		if(ParserDiagnosticsEnabled())
			std::cout << "[Parser] Starting AST construction...\n";
		auto NewAST = BuildAST();
		AST = std::move(NewAST);
		if(ParserDiagnosticsEnabled())
			std::cout << "[Parser] Successfully parsed all statements\n";
	} catch(const std::exception &) {
		throw;
	}
	if(ParserDiagnosticsEnabled()) {
		std::cout << "[AST] Final AST structure:\n";
		DumpAST();
		std::cout << "[AST] Construction completed successfully\n";
	}
}

Parser::Parser(std::string_view Query, ParserTokenizeOnlyTag /*Tag*/) : Query_(Query), CurrentIndex_(0) {
	if(Query_.size() > Limits::MaxSqlSourceBytes)
		LexFail(0, "SQL input exceeds AstralDB size guard (rejecting it to avoid exhausting host memory).");
	Tokens_ = Tokenize();
}

ASTNode Parser::ParseStandaloneSelectForViewExpansion() {
	return ParseSelectStatement();
}

int Parser::GetTokenPrecedence(const Token &Token) {
    static std::unordered_map<std::string, int> PrecedenceMap = {
        {"OR", 1},
        {"AND", 2},
        {"=", 3}, {"!=", 3}, {"==", 3}, {"IN", 3}, {"LIKE", 3},
        {"<", 4}, {"<=", 4}, {">", 4}, {">=", 4},
        {"+", 5}, {"-", 5},
        {"*", 6}, {"/", 6}, {"%", 6}
    };
    auto It = PrecedenceMap.find(Token.Value);
    return (It != PrecedenceMap.end()) ? It->second : -1;
}

bool Parser::IsConstraint(const std::string &TokenValue) {
    static const std::unordered_set<std::string> Constraints = {
        "PRIMARY", "KEY", "NOT", "NULL", "UNIQUE", "AUTO_INCREMENT",
        "DEFAULT", "REFERENCES", "CHECK", "CONSTRAINT", "GENERATED", "IDENTITY"};
    return Constraints.find(TokenValue) != Constraints.end();
}

bool Parser::IsKeyword(const std::string &TokenValue) {
    static const std::unordered_set<std::string> Keywords = {
        "SELECT", "FROM", "WHERE", "GROUP", "BY", "ORDER", "HAVING", "AS",
        "INSERT", "INTO", "VALUES", "UPDATE", "SET", "DELETE",
        "CREATE", "TABLE", "SEQUENCE", "DROP", "ALTER", "ADD", "COLUMN", "MODIFY", "RENAME",
        "PRIMARY", "KEY", "FOREIGN", "REFERENCES", "UNIQUE", "CASCADE", "RESTRICT",
        "NOT", "NULL", "DEFAULT", "AUTO_INCREMENT", "CONSTRAINT", "CHECK",
        "BOOLEAN", "BOOL", "INT", "INTEGER", "BIGINT", "SMALLINT", "TEXT", "REAL", "DOUBLE", "FLOAT",
        "DECIMAL", "NUMERIC", "CHAR", "VARCHAR", "CHARACTER",
        "DATE", "TIME", "TIMESTAMP", "DATETIME",
        "TRUE", "FALSE",         "SAVEPOINT", "RELEASE", "SAVE",
        "VIEW", "PROCEDURE", "CALL", "EXECUTE",
        "DISTINCT",
        "AND", "OR", "LIKE", "IN", "BETWEEN", "EXISTS",
        "ASC", "DESC", "LIMIT", "OFFSET", "FETCH", "FIRST", "ROWS", "ONLY",
        "BULK", "START", "STEP",
        "BEGIN", "COMMIT", "ROLLBACK", "TO",
        "GRANT", "REVOKE", "FROM", "ROLE", "TO",
        "IF", "EXISTS",
        "IS", "CURRENT_TIMESTAMP", "CURRENT_DATE", "CURRENT_TIME",
        "EXPORT", "IMPORT", "CONVERT", "DATABASE", "FORMAT", "FILE",
        "WITH", "RECURSIVE", "UNION", "ALL", "INTERSECT", "EXCEPT",
        "MERGE", "USING", "MATCHED", "CONFLICT", "DO", "NOTHING", "EXCLUDED",
        "ROLLUP", "CUBE", "GROUPING", "SETS", "GROUPING_ID",
        "PARTITION", "ROW_NUMBER", "RANK", "DENSE_RANK", "OVER", "LAG", "LEAD", "COUNT",
        "CURRENT", "ROW", "UNBOUNDED", "PRECEDING", "FOLLOWING",
        "INNER", "LEFT", "RIGHT", "FULL", "OUTER", "CROSS", "JOIN", "ON",
        "SUM", "MIN", "MAX", "AVG",
        "CASE", "WHEN", "THEN", "ELSE", "END", "CAST", "COALESCE",
        "SUBSTRING", "POSITION", "CHAR_LENGTH", "CHARACTER_LENGTH", "TRIM", "CONCAT", "EXTRACT",
        "DATE_ADD", "DATE_SUB", "DATE_DIFF", "DATE_TRUNC", "TIME_BUCKET", "TIMESTAMP_DIFF",
        "FOR", "BOTH", "LEADING", "TRAILING",
        "YEAR", "MONTH", "DAY", "HOUR", "MINUTE", "SECOND", "EPOCH",
        "GENERATED", "IDENTITY", "ALWAYS", "NEXTVAL", "INCREMENT",
        "STORAGE", "COLUMNAR", "HYBRID", "AUTO",
        "STRUCT", "MAP", "VECTOR", "MATRIX", "COMPLEX", "LIST",
        "ABS", "SQRT", "CBRT", "POW", "EXP", "LN", "LOG10", "LOG2", "SIN", "COS", "TAN", "ASIN", "ACOS", "ATAN",
        "ATAN2", "SINH", "COSH", "TANH", "FLOOR", "CEIL", "ROUND", "TRUNC", "SIGN", "MOD", "HYPOT", "DEGREES",
        "RADIANS", "LERP", "CLAMP", "MEAN", "VAR_POP", "VAR_SAMP", "STDDEV_POP", "STDDEV_SAMP", "MEDIAN", "ENTROPY",
        "NORM_L1", "NORM_L2", "LIST_SUM", "CORR", "COVAR_POP", "COVAR_SAMP", "SIGMOID", "RELU", "SOFTMAX",
        "MINMAX_SCALE", "ZSCORE", "LIST_LEN", "LIST_GET", "LIST_APPEND", "LIST_CONCAT", "LIST_CONTAINS", "LIST_SLICE",
        "LOGISTIC", "LOGIT", "SOFTPLUS", "LEAKY_RELU", "MSE_LOSS", "MAE_LOSS", "RMSE_LOSS", "BCE_LOSS", "HINGE_LOSS",
        "HUBER_LOSS", "CE_LOSS", "RANDOM", "RANDOM_NORMAL", "RANDOM_INT", "SETSEED", "COSINE_SIM", "EUCLIDEAN_DIST",
        "MANHATTAN_DIST", "MATVEC", "LIST_SORT", "LIST_SORT_DESC", "LIST_REVERSE",
        "JSON_EXTRACT", "JSON_CONTAINS", "JSON_MERGE", "JSON_ARRAY_LENGTH", "JSON_KEYS",
        "XML_EXTRACT", "XML_SERIALIZE", "XML_VALID", "TEXT_RANK", "VECTOR_TOPK", "NULLIF", "GREATEST",
        "LEAST", "FFT", "IFFT", "DCT", "IDCT", "CONV_FULL", "CONV1D", "CONV_SAME", "CONV1D_SAME", "LAPLACIAN",
        "LAPLACIAN1D", "AD_GRAD_ADD",
        "AD_GRAD_MUL_LHS", "AD_GRAD_MUL_RHS", "AD_GRAD_RELU", "AD_GRAD_SIGMOID", "AD_GRAD_CONV1D_IN",
        "AD_GRAD_CONV1D_K", "AD_CHAIN", "AD_HESSIAN", "AD_HESSIAN_RELU", "AD_HESSIAN_SIGMOID", "AD_HESSIAN_SQUARE",
        "AD_WIRTINGER_MUL_LHS", "AD_WIRTINGER_MUL_RHS", "AD_WIRTINGER_ABS2", "AD_WIRTINGER_CHAIN", "AD_WIRTINGER_DZ",
        "AD_WIRTINGER_DZBAR", "ODE_EULER", "ODE_RK4", "SDE_EULER", "SDE_GBM", "SDE_OU", "PDE_HEAT_STEP",
        "PDE_POISSON_STEP",
        "MATCH_RECOGNIZE", "MATCH", "AGAINST", "TEXT_CONTAINS", "MATCH_AGAINST", "PATTERN", "DEFINE",
        "SYSTEM", "TIME", "INDEX", "FTS", "VECTOR", "METRIC"};
    return Keywords.find(TokenValue) != Keywords.end();
}

TokenStream Parser::Tokenize() {
    TokenStream Tokens;
    const std::size_t Est =
        (std::min)(Limits::MaxSqlTokens, (std::max)(std::size_t{64}, Query_.size() / 2));
    Tokens.reserve(Est);
    size_t Position = 0;
    while(Position < Query_.size()) {
        while(Position < Query_.size() && std::isspace(Query_[Position]))
            Position++;
        if(Position >= Query_.size()) break;
        char CurrentChar = Query_[Position];

        // Handle single-line comments (--)
        if(CurrentChar == '-' && Position + 1 < Query_.size() && Query_[Position + 1] == '-') {
            Position += 2; // Skip the --
            while(Position < Query_.size() && Query_[Position] != '\n')
                Position++;
            // Skip the comment - we don't add it to tokens
            continue;
        }

        // Handle multi-line comments (/* */)
        if(CurrentChar == '/' && Position + 1 < Query_.size() && Query_[Position + 1] == '*') {
            Position += 2; // Skip the /*
            while(Position < Query_.size()) {
                if(Query_[Position] == '*' && Position + 1 < Query_.size() && Query_[Position + 1] == '/') {
                    Position += 2; // Skip the */
                    break;
                }
                Position++;
            }
            // Skip the comment - we don't add it to tokens
            continue;
        }

        // Unary +/- numeric literals (+ must not merge with binary ops; comments above consume --)
        if((CurrentChar == '+' || CurrentChar == '-') && Position + 1 < Query_.size()) {
            const char Sign = CurrentChar;
            const char Next = Query_[Position + 1];
            const bool StartsDigit = std::isdigit(static_cast<unsigned char>(Next));
            const bool StartsDotDigit =
                Next == '.' && Position + 2 < Query_.size()
                && std::isdigit(static_cast<unsigned char>(Query_[Position + 2]));
            if(StartsDigit || StartsDotDigit) {
                const size_t TokBegin = Position;
                Position++;
                const size_t Start = Position;
                if(StartsDotDigit) {
                    Position++;
                    while(Position < Query_.size() && std::isdigit(Query_[Position]))
                        Position++;
                } else {
                    while(Position < Query_.size() && std::isdigit(Query_[Position]))
                        Position++;
                    if(Position < Query_.size() && Query_[Position] == '.') {
                        Position++;
                        while(Position < Query_.size() && std::isdigit(Query_[Position]))
                            Position++;
                    }
                }
                std::string Num = std::string(Query_.substr(Start, Position - Start));
                if(Sign == '-')
                    Num = "-" + Num;
                Tokens.push_back(Token{TokenType::LITERAL, std::move(Num), TokBegin});
                if(Tokens.size() > Limits::MaxSqlTokens)
                    LexFail(TokBegin,
                            "Too many SQL tokens (limit is a safety guard against malformed or hostile input).");
                continue;
            }
        }

        if(std::isdigit(CurrentChar)) {
            size_t Start = Position;
            while(Position < Query_.size() && std::isdigit(Query_[Position]))
                Position++;
            if(Position < Query_.size() && Query_[Position] == '.') {
                Position++;
                while(Position < Query_.size() && std::isdigit(Query_[Position]))
                    Position++;
            }
            Tokens.push_back(
                Token{TokenType::LITERAL, std::string(Query_.substr(Start, Position - Start)), Start});
        }
        else if(CurrentChar == '\'') {
            const size_t QuoteBegin = Position;
            Position++;
            size_t Start = Position;
            while(Position < Query_.size() && Query_[Position] != '\'') {
                if(Query_[Position] == '\\' && Position + 1 < Query_.size())
                    Position += 2;
                else
                    Position++;
            }
            if(Position >= Query_.size())
                LexFail(QuoteBegin,
                        "Unterminated single-quoted string literal (missing closing `'` before end of input).");
            std::string Literal = std::string(Query_.substr(Start, Position - Start));
            Tokens.push_back(Token{TokenType::LITERAL, std::move(Literal), QuoteBegin});
            Position++;
        }
        else if(CurrentChar == '\"') {
            const size_t QuoteBegin = Position;
            Position++;
            size_t Start = Position;
            while(Position < Query_.size() && Query_[Position] != '\"') {
                if(Query_[Position] == '\\' && Position + 1 < Query_.size())
                    Position += 2;
                else
                    Position++;
            }
            if(Position >= Query_.size())
                LexFail(QuoteBegin,
                        "Unterminated double-quoted identifier (missing closing `\"` before end of input).");
            std::string Id = std::string(Query_.substr(Start, Position - Start));
            Tokens.push_back(Token{TokenType::IDENTIFIER, std::move(Id), QuoteBegin});
            Position++;
        }
        else if (std::isalnum(CurrentChar) || CurrentChar == '_') {
            size_t Start = Position;
            while(Position < Query_.size() && (std::isalnum(Query_[Position]) || Query_[Position] == '_'))
                Position++;
            std::string value = std::string(Query_.substr(Start, Position - Start));
            std::string Fold = value;
            for(char &C : Fold)
                C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
            if(IsKeyword(Fold)) {
                if(Fold == "SELECT") Tokens.push_back(Token{TokenType::SELECT, Fold, Start});
                else if(Fold == "CREATE") Tokens.push_back(Token{TokenType::CREATE, Fold, Start});
                else if(Fold == "INSERT") Tokens.push_back(Token{TokenType::INSERT, Fold, Start});
                else if(Fold == "BEGIN") Tokens.push_back(Token{TokenType::BEGIN, Fold, Start});
                else if(Fold == "COMMIT") Tokens.push_back(Token{TokenType::COMMIT, Fold, Start});
                else if(Fold == "ROLLBACK") Tokens.push_back(Token{TokenType::ROLLBACK, Fold, Start});
                else Tokens.push_back(Token{TokenType::KEYWORD, Fold, Start});
            } else {
                Tokens.push_back(Token{TokenType::IDENTIFIER, std::move(value), Start});
            }
        }
        else {
            std::string OperatorStr;
            if(Position + 1 < Query_.size()) {
                std::string TwoChars = std::string(Query_.substr(Position, 2));
                if(TwoChars == "<=" || TwoChars == ">=" || TwoChars == "!=" || TwoChars == "==" ||
                   TwoChars == "<>") {
                    const size_t OpBegin = Position;
                    OperatorStr = (TwoChars == "<>") ? std::string("!=") : TwoChars;
                    Position += 2;
                    Tokens.push_back(Token{TokenType::PUNCTUATION, OperatorStr, OpBegin});
                    if(Tokens.size() > Limits::MaxSqlTokens)
                        LexFail(OpBegin,
                                "Too many SQL tokens (limit is a safety guard against malformed or hostile input).");
                    continue;
                }
            }
            const size_t OpBegin = Position;
            OperatorStr = std::string(1, CurrentChar);
            Position++;
            if(std::ispunct(CurrentChar))
                Tokens.push_back(Token{TokenType::PUNCTUATION, OperatorStr, OpBegin});
            else
                Tokens.push_back(Token{TokenType::SYMBOL, OperatorStr, OpBegin});
        }
        if(Tokens.size() > Limits::MaxSqlTokens) {
            const Token &T = Tokens.back();
            LexFail(T.Begin, "Too many SQL tokens (limit is a safety guard against malformed or hostile input).");
        }
    }
    return Tokens;
}

Tree<std::unique_ptr<StatementAST>> Parser::BuildAST() const {
	if(ParserDiagnosticsEnabled())
		std::cout << "[BuildAST] Starting AST build. Token count: " << Tokens_.size() << std::endl;
	Tree<std::unique_ptr<StatementAST>> Result;
	auto *Self = const_cast<Parser *>(this);
	// Hard cap: error recovery advances at most one token per iteration; this bounds runaway loops.
	const size_t MaxIterations = std::max(Self->Tokens_.size() * size_t{8}, size_t{256});
	size_t IterationCount = 0;
	while(Self->CurrentIndex_ < Self->Tokens_.size()) {
		if(++IterationCount > MaxIterations) {
			ParseFail(
			    "Parser exceeded AST build iteration limit; input may be severely malformed or the parser is stuck.");
		}
		if(ParserDiagnosticsEnabled())
			std::cout << "[BuildAST] Parsing statement at token index: " << Self->CurrentIndex_ << std::endl;
		try {
			auto Statement = Self->ParseStatement();
			if(Statement) {
				if(ParserDiagnosticsEnabled())
					std::cout << "[BuildAST] Parsed statement at index " << Self->CurrentIndex_ << std::endl;
				Result.Add(std::move(Statement));
			} else {
				if(ParserDiagnosticsEnabled())
					std::cerr << "[BuildAST] Failed to parse statement at index " << Self->CurrentIndex_ << std::endl;
				ParseFail("Failed to parse statement.");
			}
		} catch(const std::exception &e) {
			if(ParserDiagnosticsEnabled())
				std::cerr << "[BuildAST] Error parsing statement at token " << Self->CurrentIndex_ << ": " << e.what()
				          << '\n';
			Self->AdvanceToken();
			continue;
		}
		Self->AdvanceToken();
	}
	if(Result.Empty())
		ParseFail("No valid statements were parsed.");
	if(ParserDiagnosticsEnabled())
		std::cout << "[BuildAST] AST build complete. Node count: " << Result.Size() << std::endl;
	return Result;
}

void Parser::DumpTokens() const {
    for(const auto &TokenItem : Tokens_)
        std::cout << "[" << static_cast<int>(TokenItem.Type) << ": " << TokenItem.Value << "] ";
    std::cout << std::endl;
}

ASTNode Parser::ParsePrimary() {
    if(!CurrentToken())
        ParseFail("Unexpected end of input in primary expression");
    if(auto Agg = TryParseAggregateFuncPrimary())
        return Agg;
    if(CurrentToken()->Value == "(") {
        AdvanceToken();
        auto Expr = ParseExpression();
        if(!MatchToken(TokenType::PUNCTUATION, ")"))
            ParseFail("Expected ')' in primary expression");
        return Expr;
    }
    if(CurrentToken()->Type == TokenType::KEYWORD && CurrentToken()->Value == "NULL") {
        AdvanceToken();
        return std::make_unique<NullLiteralAST>();
    }
    if(MatchKeyword("TRUE"))
        return std::make_unique<BooleanLiteralAST>(true);
    if(MatchKeyword("FALSE"))
        return std::make_unique<BooleanLiteralAST>(false);
    if(CurrentToken()->Type == TokenType::IDENTIFIER || CurrentToken()->Type == TokenType::KEYWORD) {
        std::unique_ptr<ExpressionAST> Col = std::make_unique<ColumnRefAST>(CurrentToken()->Value);
        AdvanceToken();
        return Col;
    }
    auto Literal = std::make_unique<LiteralAST>(CurrentToken()->Value);
    AdvanceToken();
    return Literal;
}

ASTNode Parser::TryParseAggregateFuncPrimary() {
	if(!AllowAggCallsInPredicate_)
		return nullptr;
	auto T = CurrentToken();
	if(!T || (T->Type != TokenType::IDENTIFIER && T->Type != TokenType::KEYWORD))
		return nullptr;
	auto FoldUpperAscii = [](std::string S) {
		for(char &C : S)
			C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
		return S;
	};
	const std::string Name = FoldUpperAscii(T->Value);
	if(Name != "COUNT" && Name != "SUM" && Name != "MIN" && Name != "MAX" && Name != "AVG")
		return nullptr;
	AdvanceToken();
	if(!CurrentToken() || CurrentToken()->Value != "(")
		ParseFail("Expected '(' after aggregate in HAVING");
	AdvanceToken();
	if(Name == "COUNT") {
		if(MatchKeyword("DISTINCT")) {
			auto Dt = CurrentToken();
			if(!Dt || Dt->Type != TokenType::IDENTIFIER)
				ParseFail("Expected column name after COUNT(DISTINCT in HAVING");
			std::string Col = Dt->Value;
			AdvanceToken();
			if(!CurrentToken() || CurrentToken()->Value != ")")
				ParseFail("Expected ')' after COUNT(DISTINCT …) in HAVING");
			AdvanceToken();
			return std::make_unique<FuncCallExprAST>(FuncCallExprAST::Kind::CountDistinct, std::move(Col));
		}
		if(!CurrentToken())
			ParseFail("Unexpected end of input in COUNT( in HAVING");
		if(CurrentToken()->Value == "*") {
			AdvanceToken();
			if(!CurrentToken() || CurrentToken()->Value != ")")
				ParseFail("Expected ')' after COUNT(*) in HAVING");
			AdvanceToken();
			return std::make_unique<FuncCallExprAST>(FuncCallExprAST::Kind::CountStar, std::string());
		}
		ParseFail("COUNT in HAVING expects * or DISTINCT column");
	}
	auto ArgTk = CurrentToken();
	if(!ArgTk || ArgTk->Type != TokenType::IDENTIFIER)
		ParseFail("Expected column name inside aggregate in HAVING");
	std::string Arg = ArgTk->Value;
	AdvanceToken();
	if(!CurrentToken() || CurrentToken()->Value != ")")
		ParseFail("Expected ')' closing aggregate in HAVING");
	AdvanceToken();
	FuncCallExprAST::Kind K = FuncCallExprAST::Kind::Sum;
	if(Name == "MIN")
		K = FuncCallExprAST::Kind::Min;
	else if(Name == "MAX")
		K = FuncCallExprAST::Kind::Max;
	else if(Name == "AVG")
		K = FuncCallExprAST::Kind::Avg;
	else if(Name != "SUM")
		ParseFail("Internal aggregate name in HAVING");
	return std::make_unique<FuncCallExprAST>(K, std::move(Arg));
}

ASTNode Parser::ParseUnaryOrPostfixPredicate() {
    if(MatchKeyword("EXISTS"))
        return ParseExistsPredicate(false);
    if(auto Tk = CurrentToken(); Tk && Tk->Type == TokenType::KEYWORD && Tk->Value == "NOT") {
        const size_t Save = CurrentIndex_;
        AdvanceToken();
        if(MatchKeyword("EXISTS"))
            return ParseExistsPredicate(true);
        if(MatchKeyword("TRUE"))
            return std::make_unique<BooleanLiteralAST>(false);
        if(MatchKeyword("FALSE"))
            return std::make_unique<BooleanLiteralAST>(true);
        CurrentIndex_ = Save;
    }
    auto Node = ParsePrimary();
    while(CurrentToken()) {
        if(CurrentToken()->Type == TokenType::KEYWORD && CurrentToken()->Value == "IS") {
            AdvanceToken();
            bool Negated = MatchKeyword("NOT");
            if(!MatchKeyword("NULL"))
                ParseFail("Expected NULL (or NOT NULL) after IS");
            Node = std::make_unique<IsNullPredAST>(AsExpr(std::move(Node)), Negated);
            continue;
        }
        if(MatchKeyword("NOT")) {
            if(MatchKeyword("LIKE")) {
                auto Pat = ParsePrimary();
                Node = std::make_unique<BinaryOpAST>(AsExpr(std::move(Node)), "NOT LIKE",
                                                     AsExpr(std::move(Pat)));
                continue;
            }
            ParseFail("Expected LIKE after NOT");
        }
        if(MatchKeyword("BETWEEN")) {
            auto Lo = ParsePrimary();
            if(!MatchKeyword("AND"))
                ParseFail("Expected AND in BETWEEN clause");
            auto Hi = ParsePrimary();
            Node = std::make_unique<BetweenAST>(AsExpr(std::move(Node)), AsExpr(std::move(Lo)),
                                                  AsExpr(std::move(Hi)));
            continue;
        }
        if(MatchKeyword("IN")) {
            if(!CurrentToken() || CurrentToken()->Value != "(")
                ParseFail("Expected '(' after IN");
            AdvanceToken();
            std::vector<std::string> LitVals;
            while(auto T = CurrentToken()) {
                if(T->Value == ")") {
                    AdvanceToken();
                    break;
                }
                LitVals.push_back(T->Value);
                AdvanceToken();
                if(CurrentToken() && CurrentToken()->Value == ",")
                    AdvanceToken();
            }
            if(LitVals.empty())
                ParseFail("IN clause requires at least one value");
            auto InRhs = std::make_unique<InValuesAST>(std::move(LitVals));
            Node = std::make_unique<BinaryOpAST>(AsExpr(std::move(Node)), "IN", AsExpr(std::move(InRhs)));
            continue;
        }
        if(MatchKeyword("LIKE")) {
            auto Pat = ParsePrimary();
            Node = std::make_unique<BinaryOpAST>(AsExpr(std::move(Node)), "LIKE", AsExpr(std::move(Pat)));
            continue;
        }
        if(MatchKeyword("MATCH")) {
            if(MatchKeyword("RECOGNIZE"))
                ParseFail("MATCH_RECOGNIZE belongs after FROM, not as a predicate");
            auto Query = ParsePrimary();
            Node = std::make_unique<BinaryOpAST>(AsExpr(std::move(Node)), "MATCH", AsExpr(std::move(Query)));
            continue;
        }
        break;
    }
    return Node;
}

namespace {
bool IsTableConstraintStarter(const std::optional<Token> &Tok) {
	if(!Tok)
		return false;
	if(Tok->Value == "PRIMARY" || Tok->Value == "CONSTRAINT" || Tok->Value == "UNIQUE"
	    || Tok->Value == "FOREIGN" || Tok->Value == "CHECK")
		return true;
	return false;
}
std::string NormalizeTypeToken(std::string_view V) {
	std::string Out;
	Out.reserve(V.size());
	for(unsigned char C : V)
		Out.push_back(static_cast<char>(std::toupper(C)));
	return Out;
}
} // namespace

std::string Parser::ParseDataType() {
	auto Tok = CurrentToken();
	if(!Tok)
		ParseFail("Expected column data type.");
	std::string Raw;
	if(Tok->Type == TokenType::KEYWORD || Tok->Type == TokenType::IDENTIFIER)
		Raw = Tok->Value;
	else
		ParseFail("Invalid data type token.");
	AdvanceToken();
	const std::string Base = NormalizeTypeToken(Raw);
	if(Base == "CHARACTER" && MatchKeyword("VARYING")) {
		/* consume optional VARYING token */
	}
	if(Base == "DOUBLE" && MatchKeyword("PRECISION")) {
		/* consume optional PRECISION token */
	}
	std::string Canon = Base;
	if(Canon == "INT" || Canon == "INTEGER" || Canon == "SMALLINT" || Canon == "BIGINT")
		return "INTEGER";
	if(Canon == "BOOL")
		return "BOOLEAN";
	if(Canon == "DEC" || Canon == "NUMBER")
		return "NUMERIC";
	if(Canon == "DATETIME")
		return "TIMESTAMP";
	if(Canon == "COMPLEX")
		return "COMPLEX";
	if(Canon == "LIST") {
		if(!CurrentToken() || CurrentToken()->Value != "(")
			ParseFail("Expected '(' after LIST");
		AdvanceToken();
		auto Ty = CurrentToken();
		if(!Ty || (Ty->Type != TokenType::IDENTIFIER && Ty->Type != TokenType::KEYWORD))
			ParseFail("LIST expects element type");
		std::ostringstream O;
		O << "LIST(" << Ty->Value << ')';
		AdvanceToken();
		if(!CurrentToken() || CurrentToken()->Value != ")")
			ParseFail("Expected ')' after LIST element type");
		AdvanceToken();
		return std::move(O).str();
	}
	if(Canon == "STRUCT" || Canon == "MAP" || Canon == "VECTOR" || Canon == "MATRIX") {
		if(!CurrentToken() || CurrentToken()->Value != "(")
			ParseFail("Expected '(' after advanced type name");
		AdvanceToken();
		std::ostringstream O;
		O << Canon << '(';
		bool First = true;
		int FieldCount = 0;
		while(auto T = CurrentToken()) {
			if(T->Value == ")") {
				AdvanceToken();
				break;
			}
			if(!First)
				O << ',';
			First = false;
			if(Canon == "STRUCT") {
				auto F = CurrentToken();
				if(!F || (F->Type != TokenType::IDENTIFIER && F->Type != TokenType::KEYWORD))
					ParseFail("STRUCT field expects name");
				O << F->Value;
				AdvanceToken();
				auto Ty = CurrentToken();
				if(!Ty || (Ty->Type != TokenType::IDENTIFIER && Ty->Type != TokenType::KEYWORD))
					ParseFail("STRUCT field expects type");
				O << ' ' << Ty->Value;
				AdvanceToken();
				if(CurrentToken() && CurrentToken()->Value == "(") {
					AdvanceToken();
					while(auto Pp = CurrentToken()) {
						if(Pp->Value == ")") {
							AdvanceToken();
							break;
						}
						O << Pp->Value;
						AdvanceToken();
						if(CurrentToken() && CurrentToken()->Value == ",") {
							O << ',';
							AdvanceToken();
						}
					}
					O << ')';
				}
				++FieldCount;
			} else if(Canon == "MAP") {
				for(int Part = 0; Part < 2; ++Part) {
					if(Part == 1) {
						O << ',';
						if(CurrentToken() && CurrentToken()->Value == ",")
							AdvanceToken();
					}
					auto Ty = CurrentToken();
					if(!Ty || (Ty->Type != TokenType::IDENTIFIER && Ty->Type != TokenType::KEYWORD))
						ParseFail("MAP expects key and value types");
					O << Ty->Value;
					AdvanceToken();
				}
				++FieldCount;
			} else if(Canon == "VECTOR") {
				auto N = CurrentToken();
				if(!N || N->Type != TokenType::LITERAL)
					ParseFail("VECTOR expects length literal");
				O << N->Value;
				AdvanceToken();
				++FieldCount;
			} else if(Canon == "MATRIX") {
				for(int Part = 0; Part < 2; ++Part) {
					if(Part == 1) {
						O << ',';
						if(CurrentToken() && CurrentToken()->Value == ",")
							AdvanceToken();
					}
					auto N = CurrentToken();
					if(!N || N->Type != TokenType::LITERAL)
						ParseFail("MATRIX expects row and column literals");
					O << N->Value;
					AdvanceToken();
				}
				++FieldCount;
			}
			if(CurrentToken() && CurrentToken()->Value == ",")
				AdvanceToken();
		}
		O << ')';
		if(Canon == "STRUCT" && FieldCount == 0)
			ParseFail("STRUCT requires at least one field");
		std::string Out = std::move(O).str();
		auto Elem = CurrentToken();
		if(Elem && (Elem->Type == TokenType::IDENTIFIER || Elem->Type == TokenType::KEYWORD)) {
			const std::string Et = NormalizeTypeToken(Elem->Value);
			if(Et == "FLOAT" || Et == "DOUBLE" || Et == "REAL" || Et == "INTEGER") {
				Out += ' ';
				Out += Et;
				AdvanceToken();
			}
		}
		return Out;
	}
	auto P = CurrentToken();
	if(P && P->Value == "(") {
		AdvanceToken();
		std::vector<std::string> Parts;
		while(auto T = CurrentToken()) {
			if(T->Value == ")") {
				AdvanceToken();
				break;
			}
			if(T->Value == ",") {
				AdvanceToken();
				continue;
			}
			if(T->Type == TokenType::LITERAL || T->Type == TokenType::IDENTIFIER
			   || T->Type == TokenType::KEYWORD)
				Parts.push_back(T->Value);
			else if(T->Type == TokenType::PUNCTUATION && std::ispunct(static_cast<unsigned char>(T->Value[0])))
				Parts.push_back(T->Value);
			else
				ParseFail("Unexpected token in type parameter list.");
			AdvanceToken();
		}
		if(Canon == "VARCHAR" || Canon == "CHAR")
			return Parts.empty() ? Canon : (Canon + "(" + Parts.front() + ")");
		if(Canon == "NUMERIC" || Canon == "DECIMAL") {
			std::ostringstream O;
			O << Canon << '(' << (Parts.empty() ? std::string("16") : Parts[0]);
			if(Parts.size() >= 2)
				O << ',' << Parts[1];
			O << ')';
			return std::move(O).str();
		}
		return Canon + (Parts.empty() ? std::string() : "(" + Parts.front() + ")");
	}
	return Canon;
}

SqlCastTarget Parser::ParseCastTargetFromDataType(std::string P) {
	if(P == "INTEGER")
		return SqlCastTarget::Integer;
	if(P == "TEXT" || P.rfind("VARCHAR", 0) == 0 || P.rfind("CHARACTER", 0) == 0)
		return SqlCastTarget::Text;
	if(P == "CHAR" || P.rfind("CHAR(", 0) == 0)
		return SqlCastTarget::Text;
	if(P == "REAL" || P == "FLOAT" || P == "DOUBLE")
		return SqlCastTarget::Real;
	if(P.rfind("NUMERIC", 0) == 0 || P.rfind("DECIMAL", 0) == 0)
		return SqlCastTarget::Real;
	if(P == "BOOLEAN")
		return SqlCastTarget::Boolean;
	if(P == "DATE" || P == "TIME" || P == "TIMESTAMP" || P == "DATETIME")
		return SqlCastTarget::Text;
	if(AdvancedTypes::IsAdvancedTypeSpelling(P))
		return SqlCastTarget::Advanced;
	ParseFail("CAST does not support target type: " + P);
}

std::vector<std::string> Parser::ParseColumnConstraintList() {
	std::vector<std::string> Out;
	for(;;) {
		auto T = CurrentToken();
		if(!T || T->Value == ")" || T->Value == ",")
			break;
		if(MatchKeyword("NOT")) {
			if(!MatchKeyword("NULL"))
				ParseFail("Expected NULL after NOT");
			Out.push_back("NOT");
			Out.push_back("NULL");
			continue;
		}
		if(MatchKeyword("PRIMARY")) {
			if(!MatchKeyword("KEY"))
				ParseFail("Expected KEY after PRIMARY");
			Out.push_back("PRIMARY");
			Out.push_back("KEY");
			continue;
		}
		if(MatchKeyword("UNIQUE")) {
			Out.push_back("UNIQUE");
			continue;
		}
		if(MatchKeyword("AUTO_INCREMENT")) {
			Out.push_back("AUTO_INCREMENT");
			continue;
		}
		if(MatchKeyword("GENERATED")) {
			const bool Always = MatchKeyword("ALWAYS");
			if(!Always && !MatchKeyword("BY"))
				ParseFail("Expected ALWAYS or BY after GENERATED");
			if(!Always) {
				if(!MatchKeyword("DEFAULT"))
					ParseFail("Expected DEFAULT after GENERATED BY");
			}
			if(!MatchKeyword("AS"))
				ParseFail("Expected AS after GENERATED …");
			if(!MatchKeyword("IDENTITY"))
				ParseFail("Expected IDENTITY after GENERATED … AS");
			int64_t IdStart = 1;
			int64_t IdInc = 1;
			if(CurrentToken() && CurrentToken()->Value == "(") {
				AdvanceToken();
				ParseSequenceOptions(IdStart, IdInc);
				if(!CurrentToken() || CurrentToken()->Value != ")")
					ParseFail("Expected ')' after identity sequence options");
				AdvanceToken();
			}
			Out.push_back(std::string("IDENTITY:") + (Always ? "1" : "0") + ":" + std::to_string(IdStart) + ":" +
			              std::to_string(IdInc));
			continue;
		}
		if(MatchKeyword("DEFAULT")) {
			if(MatchKeyword("NULL")) {
				Out.push_back("DEFAULT:" + std::string("__NULL__"));
				continue;
			}
			if(MatchKeyword("CURRENT_TIMESTAMP")) {
				Out.push_back("DEFAULT:__CURRENT_TIMESTAMP__");
				continue;
			}
			if(MatchKeyword("CURRENT_DATE")) {
				Out.push_back("DEFAULT:__CURRENT_DATE__");
				continue;
			}
			if(MatchKeyword("CURRENT_TIME")) {
				Out.push_back("DEFAULT:__CURRENT_TIME__");
				continue;
			}
			auto Lit = CurrentToken();
			if(!Lit)
				ParseFail("Expected literal after DEFAULT");
			if(Lit->Type == TokenType::LITERAL) {
				Out.push_back("DEFAULT:" + Lit->Value);
				AdvanceToken();
				continue;
			}
			if(Lit->Type == TokenType::KEYWORD
			   && (Lit->Value == "TRUE" || Lit->Value == "FALSE")) {
				Out.push_back("DEFAULT:" + Lit->Value);
				AdvanceToken();
				continue;
			}
			if(Lit->Type == TokenType::IDENTIFIER) {
				const std::string V = NormalizeTypeToken(Lit->Value);
				if(V == "TRUE" || V == "FALSE") {
					Out.push_back("DEFAULT:" + V);
					AdvanceToken();
					continue;
				}
				Out.push_back("DEFAULT:" + Lit->Value);
				AdvanceToken();
				continue;
			}
			ParseFail("Unsupported DEFAULT clause");
		}
		if(MatchKeyword("REFERENCES")) {
			auto RT = CurrentToken();
			if(!RT || (RT->Type != TokenType::IDENTIFIER && RT->Type != TokenType::KEYWORD))
				ParseFail("Expected referenced table after REFERENCES");
			const std::string RefT = RT->Value;
			AdvanceToken();
			if(!CurrentToken() || CurrentToken()->Value != "(")
				ParseFail("Expected '(' after REFERENCES table");
			AdvanceToken();
			auto RC = CurrentToken();
			if(!RC || (RC->Type != TokenType::IDENTIFIER && RC->Type != TokenType::KEYWORD))
				ParseFail("Expected referenced column");
			const std::string RefC = RC->Value;
			AdvanceToken();
			if(!CurrentToken() || CurrentToken()->Value != ")")
				ParseFail("Expected ')' after REFERENCES column");
			AdvanceToken();
			Out.push_back("REFERENCES:" + RefT + ":" + RefC);
			continue;
		}
		if(MatchKeyword("CHECK")) {
			if(!CurrentToken() || CurrentToken()->Value != "(")
				ParseFail("Expected '(' after CHECK");
			AdvanceToken();
			std::string Inner;
			int Depth = 1;
			while(Depth > 0) {
				auto N = CurrentToken();
				if(!N)
					ParseFail("Unterminated CHECK expression");
				if(N->Value == "(")
					Depth++;
				else if(N->Value == ")")
					Depth--;
				else {
					if(!Inner.empty())
						Inner.push_back(' ');
					Inner += N->Value;
				}
				AdvanceToken();
			}
			Out.push_back("CHECK:" + Inner);
			continue;
		}
		ParseFail(std::string("Unexpected column constraint: ") + T->Value);
	}
	return Out;
}

TableConstraintDef Parser::ParseTableConstraint() {
	TableConstraintDef Def;
	if(MatchKeyword("CONSTRAINT")) {
		auto N = CurrentToken();
		if(!N)
			ParseFail("Expected constraint name");
		Def.Name = N->Value;
		AdvanceToken();
	}
	if(MatchKeyword("PRIMARY")) {
		if(!MatchKeyword("KEY"))
			ParseFail("Expected KEY");
		if(!CurrentToken() || CurrentToken()->Value != "(")
			ParseFail("Expected '(' after PRIMARY KEY");
		AdvanceToken();
		while(auto T = CurrentToken()) {
			if(T->Value == ")") {
				AdvanceToken();
				break;
			}
			if(T->Value == ",") {
				AdvanceToken();
				continue;
			}
			if(T->Type == TokenType::IDENTIFIER || T->Type == TokenType::KEYWORD) {
				Def.Columns.push_back(T->Value);
				AdvanceToken();
			} else
				ParseFail("Expected column identifier in PRIMARY KEY list");
		}
		Def.Kind = TableConstraintKind::PrimaryKey;
		return Def;
	}
	if(MatchKeyword("UNIQUE")) {
		if(!CurrentToken() || CurrentToken()->Value != "(")
			ParseFail("Expected '(' after UNIQUE");
		AdvanceToken();
		while(auto T = CurrentToken()) {
			if(T->Value == ")") {
				AdvanceToken();
				break;
			}
			if(T->Value == ",") {
				AdvanceToken();
				continue;
			}
			if(T->Type == TokenType::IDENTIFIER || T->Type == TokenType::KEYWORD) {
				Def.Columns.push_back(T->Value);
				AdvanceToken();
			} else
				ParseFail("Expected column identifier in UNIQUE list");
		}
		Def.Kind = TableConstraintKind::Unique;
		return Def;
	}
	if(MatchKeyword("FOREIGN")) {
		if(!MatchKeyword("KEY"))
			ParseFail("Expected KEY after FOREIGN");
		if(!CurrentToken() || CurrentToken()->Value != "(")
			ParseFail("Expected '(' after FOREIGN KEY");
		AdvanceToken();
		while(auto LC = CurrentToken()) {
			if(LC->Value == ")") {
				AdvanceToken();
				break;
			}
			if(LC->Value == ",") {
				AdvanceToken();
				continue;
			}
			if(LC->Type == TokenType::IDENTIFIER || LC->Type == TokenType::KEYWORD) {
				Def.Columns.push_back(LC->Value);
				AdvanceToken();
			} else
				ParseFail("Expected FK column name");
		}
		if(Def.Columns.empty())
			ParseFail("FOREIGN KEY requires at least one column");
		if(!MatchKeyword("REFERENCES"))
			ParseFail("Expected REFERENCES");
		auto RT = CurrentToken();
		if(!RT || (RT->Type != TokenType::IDENTIFIER && RT->Type != TokenType::KEYWORD))
			ParseFail("Expected referenced table");
		Def.RefTable = RT->Value;
		AdvanceToken();
		if(!CurrentToken() || CurrentToken()->Value != "(")
			ParseFail("Expected '(' after REFERENCES table");
		AdvanceToken();
		while(auto Rpc = CurrentToken()) {
			if(Rpc->Value == ")") {
				AdvanceToken();
				break;
			}
			if(Rpc->Value == ",") {
				AdvanceToken();
				continue;
			}
			if(Rpc->Type == TokenType::IDENTIFIER || Rpc->Type == TokenType::KEYWORD) {
				Def.RefColumns.push_back(Rpc->Value);
				AdvanceToken();
			} else
				ParseFail("Expected referenced column name");
		}
		if(Def.RefColumns.empty())
			ParseFail("REFERENCES requires at least one column");
		if(MatchKeyword("ON")) {
			if(!MatchKeyword("DELETE"))
				ParseFail("Expected DELETE after ON");
			if(MatchKeyword("CASCADE"))
				Def.OnDeleteAction = 1;
			else if(MatchKeyword("SET") && MatchKeyword("NULL"))
				Def.OnDeleteAction = 2;
			else if(MatchKeyword("RESTRICT") || MatchKeyword("NO"))
				Def.OnDeleteAction = 0;
			else
				ParseFail("Expected CASCADE, SET NULL, or RESTRICT after ON DELETE");
		}
		Def.Kind = TableConstraintKind::ForeignKey;
		return Def;
	}
	if(MatchKeyword("CHECK")) {
		if(!CurrentToken() || CurrentToken()->Value != "(")
			ParseFail("Expected '(' after CHECK");
		AdvanceToken();
		std::string Inner;
		int Depth = 1;
		while(Depth > 0) {
			auto N = CurrentToken();
			if(!N)
				ParseFail("Unterminated table CHECK");
			if(N->Value == "(")
				Depth++;
			else if(N->Value == ")")
				Depth--;
			else {
				if(!Inner.empty())
					Inner.push_back(' ');
				Inner += N->Value;
			}
			AdvanceToken();
		}
		Def.CheckSql = Inner;
		Def.Kind = TableConstraintKind::Check;
		return Def;
	}
	ParseFail("Expected table constraint (PRIMARY KEY, UNIQUE, FOREIGN KEY, CHECK).");
}

void Parser::ParseSequenceOptions(int64_t &Start, int64_t &Increment) {
	Start = 1;
	Increment = 1;
	if(MatchKeyword("START")) {
		if(!MatchKeyword("WITH"))
			ParseFail("Expected WITH after START in sequence options");
		auto St = CurrentToken();
		if(!St || St->Type != TokenType::LITERAL)
			ParseFail("Expected integer after START WITH");
		Start = std::stoll(St->Value);
		AdvanceToken();
	}
	if(MatchKeyword("INCREMENT")) {
		if(!MatchKeyword("BY"))
			ParseFail("Expected BY after INCREMENT in sequence options");
		auto It = CurrentToken();
		if(!It || It->Type != TokenType::LITERAL)
			ParseFail("Expected integer after INCREMENT BY");
		Increment = std::stoll(It->Value);
		if(Increment == 0)
			ParseFail("INCREMENT BY must be non-zero");
		AdvanceToken();
	}
}

ASTNode Parser::ParseCreateIndexStatement() {
	bool IfNotExists = false;
	if(MatchKeyword("IF")) {
		if(!MatchKeyword("NOT"))
			ParseFail("Expected NOT in IF NOT EXISTS clause");
		if(!MatchKeyword("EXISTS"))
			ParseFail("Expected EXISTS in IF NOT EXISTS clause");
		IfNotExists = true;
	}
	auto NameTok = CurrentToken();
	if(!NameTok || NameTok->Type != TokenType::IDENTIFIER)
		ParseFail("Expected index name after CREATE INDEX.");
	std::string IdxName = NameTok->Value;
	AdvanceToken();
	if(!MatchKeyword("ON"))
		ParseFail("Expected ON after index name.");
	auto TableTok = CurrentToken();
	if(!TableTok || TableTok->Type != TokenType::IDENTIFIER)
		ParseFail("Expected table name after ON.");
	std::string TableName = TableTok->Value;
	AdvanceToken();
	if(!CurrentToken() || CurrentToken()->Value != "(")
		ParseFail("Expected '(' before index column.");
	AdvanceToken();
	auto ColTok = CurrentToken();
	if(!ColTok || ColTok->Type != TokenType::IDENTIFIER)
		ParseFail("Expected column name in CREATE INDEX.");
	std::string ColumnName = ColTok->Value;
	AdvanceToken();
	if(!CurrentToken() || CurrentToken()->Value != ")")
		ParseFail("Expected ')' after index column.");
	AdvanceToken();
	if(!MatchKeyword("USING"))
		ParseFail("Expected USING FTS or USING VECTOR in CREATE INDEX.");
	SecondaryIndexKind Kind = SecondaryIndexKind::Fts;
	int64_t MetricTag = 1;
	if(MatchKeyword("FTS"))
		Kind = SecondaryIndexKind::Fts;
	else if(MatchKeyword("VECTOR")) {
		Kind = SecondaryIndexKind::Vector;
		if(MatchKeyword("METRIC")) {
			if(MatchKeyword("L2"))
				MetricTag = 0;
			else if(MatchKeyword("COSINE"))
				MetricTag = 1;
			else
				ParseFail("VECTOR METRIC expects COSINE or L2.");
		}
	} else
		ParseFail("Expected FTS or VECTOR after USING.");
	(void)IfNotExists;
	return std::make_unique<CreateIndexAST>(std::move(IdxName), std::move(TableName), std::move(ColumnName), Kind,
	                                        MetricTag);
}

ASTNode Parser::ParseCreateStatement() {
	AdvanceToken();
	if(MatchKeyword("INDEX"))
		return ParseCreateIndexStatement();
	if(MatchKeyword("VIEW"))
		return ParseCreateViewStatement();
	if(MatchKeyword("PROCEDURE"))
		return ParseCreateProcedureStatement();
	if(MatchKeyword("SEQUENCE")) {
		bool IfNotExists = false;
		if(MatchKeyword("IF")) {
			if(!MatchKeyword("NOT"))
				ParseFail("Expected NOT in IF NOT EXISTS clause");
			if(!MatchKeyword("EXISTS"))
				ParseFail("Expected EXISTS in IF NOT EXISTS clause");
			IfNotExists = true;
		}
		auto Nt = CurrentToken();
		if(!Nt || Nt->Type != TokenType::IDENTIFIER)
			ParseFail("Expected sequence name after CREATE SEQUENCE.");
		std::string SeqName = Nt->Value;
		AdvanceToken();
		int64_t Start = 1;
		int64_t Increment = 1;
		ParseSequenceOptions(Start, Increment);
		return std::make_unique<CreateSequenceAST>(std::move(SeqName), Start, Increment, IfNotExists);
	}
	if(MatchKeyword("ROLE")) {
		auto RoleTok = CurrentToken();
		if(!RoleTok || RoleTok->Type != TokenType::IDENTIFIER)
			ParseFail("Expected role name after CREATE ROLE.");
		std::string RoleName = RoleTok->Value;
		AdvanceToken();
		return std::make_unique<CreateRoleAST>(std::move(RoleName));
	}
	if(!MatchKeyword("TABLE"))
		ParseFail("Expected VIEW, PROCEDURE, SEQUENCE, ROLE, or TABLE after CREATE.");
	bool IfNotExists = false;
	if(MatchKeyword("IF")) {
		if(!MatchKeyword("NOT"))
			ParseFail("Expected NOT in IF NOT EXISTS clause");
		if(!MatchKeyword("EXISTS"))
			ParseFail("Expected EXISTS in IF NOT EXISTS clause");
		IfNotExists = true;
	}
	auto TableToken = CurrentToken();
	if(!TableToken)
		ParseFail("Expected table name after 'CREATE TABLE'.");
	const std::string TableName = TableToken->Value;
	AdvanceToken();
	if(!MatchToken(TokenType::PUNCTUATION, "("))
		ParseFail("Expected '(' after CREATE TABLE table name.");
	std::vector<ColumnDefinition> Columns;
	std::vector<TableConstraintDef> TabCons;
	while(true) {
		auto Ahead = CurrentToken();
		if(!Ahead)
			ParseFail("Unexpected EOF in CREATE TABLE column list.");
		if(Ahead->Value == ")") {
			AdvanceToken();
			break;
		}
		if(IsTableConstraintStarter(Ahead))
			TabCons.push_back(ParseTableConstraint());
		else if(Ahead->Type == TokenType::IDENTIFIER || Ahead->Type == TokenType::KEYWORD) {
			const std::string ColName = Ahead->Value;
			AdvanceToken();
			std::string DType = ParseDataType();
			std::vector<std::string> CList = ParseColumnConstraintList();
			Columns.emplace_back(ColName, std::move(DType), std::move(CList));
		} else
			ParseFail("Expected column definition or table constraint in CREATE TABLE.");
		if(CurrentToken() && CurrentToken()->Value == ",")
			AdvanceToken();
		else if(CurrentToken() && CurrentToken()->Value == ")") {
			AdvanceToken();
			break;
		} else if(!CurrentToken())
			ParseFail("Unterminated CREATE TABLE clause.");
		else
			ParseFail("Expected ',' or ')' in CREATE TABLE.");
	}
	StorageLayout Storage = StorageLayout::Row;
	if(MatchKeyword("USING")) {
		if(!MatchKeyword("STORAGE"))
			ParseFail("Expected STORAGE after USING in CREATE TABLE");
		auto St = CurrentToken();
		if(!St)
			ParseFail("Expected ROW, COLUMNAR, HYBRID, or AUTO after USING STORAGE");
		Storage = StorageLayoutFromKeyword(St->Value);
		AdvanceToken();
	}
	return std::make_unique<CreateAST>(TableName, Columns, std::move(TabCons), IfNotExists, Storage);
}

ASTNode Parser::ParseCreateViewStatement() {
	auto Nt = CurrentToken();
	if(!Nt || Nt->Type != TokenType::IDENTIFIER)
		ParseFail("Expected view name after CREATE VIEW.");
	std::string Vn = Nt->Value;
	AdvanceToken();
	if(!MatchKeyword("AS"))
		ParseFail("Expected AS after CREATE VIEW view name.");
	auto St = CurrentToken();
	if(!St || St->Type != TokenType::SELECT)
		ParseFail("Expected SELECT after AS in CREATE VIEW.");
	const std::size_t Byte0 = Tokens_[CurrentIndex_].Begin;
	auto Def = ParseSelectStatement();
	if(!Def)
		ParseFail("Failed to parse CREATE VIEW body.");
	if(!dynamic_cast<const SelectAST *>(Def.get()))
		ParseFail("CREATE VIEW expects a plain SELECT definition (compound queries are not supported in views yet).");
	const std::size_t Past = CurrentIndex_ == 0 ? 0 : CurrentIndex_ - 1;
	const std::size_t BodyEndExcl = Tokens_[Past].Begin + Tokens_[Past].Value.size();
	std::string BodySql(Query_.substr(Byte0, BodyEndExcl - Byte0));
	return std::make_unique<CreateViewAST>(std::move(Vn), std::move(BodySql), std::move(Def));
}

ASTNode Parser::ParseCreateProcedureStatement() {
	bool IfNotExists = false;
	if(MatchKeyword("IF")) {
		if(!MatchKeyword("NOT"))
			ParseFail("Expected NOT in IF NOT EXISTS clause");
		if(!MatchKeyword("EXISTS"))
			ParseFail("Expected EXISTS in IF NOT EXISTS clause");
		IfNotExists = true;
	}
	auto Nt = CurrentToken();
	if(!Nt || Nt->Type != TokenType::IDENTIFIER)
		ParseFail("Expected procedure name after CREATE PROCEDURE.");
	std::string Pn = Nt->Value;
	AdvanceToken();
	if(!MatchKeyword("AS"))
		ParseFail("Expected AS after CREATE PROCEDURE name.");
	if(!MatchToken(TokenType::PUNCTUATION, "("))
		ParseFail("Expected '(' after AS in CREATE PROCEDURE (body is a parenthesized statement list).");
	const std::size_t Byte0 = Tokens_[CurrentIndex_].Begin;
	while(true) {
		auto Ahead = CurrentToken();
		if(!Ahead)
			ParseFail("Unexpected EOF in CREATE PROCEDURE body.");
		if(Ahead->Type == TokenType::PUNCTUATION && Ahead->Value == ")") {
			AdvanceToken();
			break;
		}
		if(Ahead->Type == TokenType::PUNCTUATION && Ahead->Value == ";") {
			AdvanceToken();
			continue;
		}
		if(!ParseStatement())
			ParseFail("Expected SQL statement inside CREATE PROCEDURE body.");
	}
	const std::size_t BodyEnd = Byte0 > 0 ? Byte0 : 0;
	std::string BodySql;
	if(CurrentIndex_ > 0) {
		const std::size_t CloseParen = Tokens_[CurrentIndex_ - 1].Begin;
		if(CloseParen > Byte0)
			BodySql = Query_.substr(Byte0, CloseParen - Byte0);
	}
	(void)BodyEnd;
	return std::make_unique<CreateProcedureAST>(std::move(Pn), std::move(BodySql), IfNotExists);
}

ASTNode Parser::ParseDropProcedureStatement() {
	bool IfExists = false;
	if(MatchKeyword("IF")) {
		if(!MatchKeyword("EXISTS"))
			ParseFail("Expected EXISTS in IF EXISTS clause");
		IfExists = true;
	}
	auto Nt = CurrentToken();
	if(!Nt || Nt->Type != TokenType::IDENTIFIER)
		ParseFail("Expected procedure name after DROP PROCEDURE.");
	std::string Pn = Nt->Value;
	AdvanceToken();
	return std::make_unique<DropProcedureAST>(std::move(Pn), IfExists);
}

ASTNode Parser::ParseCallProcedureStatement() {
	bool ViaExecute = false;
	if(MatchKeyword("PROCEDURE"))
		ViaExecute = true;
	(void)ViaExecute;
	auto Nt = CurrentToken();
	if(!Nt || Nt->Type != TokenType::IDENTIFIER)
		ParseFail("Expected procedure name after CALL / EXECUTE PROCEDURE.");
	std::string Pn = Nt->Value;
	AdvanceToken();
	return std::make_unique<CallProcedureAST>(std::move(Pn));
}

ASTNode Parser::ParseDropStatement() {
    AdvanceToken();
	if(MatchKeyword("ROLE")) {
		auto RoleTok = CurrentToken();
		if(!RoleTok || RoleTok->Type != TokenType::IDENTIFIER)
			ParseFail("Expected role name after DROP ROLE.");
		std::string RoleName = RoleTok->Value;
		AdvanceToken();
		return std::make_unique<DropRoleAST>(std::move(RoleName));
	}
	if(MatchKeyword("INDEX")) {
		bool IfExistsIdx = false;
		if(MatchKeyword("IF")) {
			if(!MatchKeyword("EXISTS"))
				ParseFail("Expected EXISTS in IF EXISTS clause");
			IfExistsIdx = true;
		}
		auto Nt = CurrentToken();
		if(!Nt || Nt->Type != TokenType::IDENTIFIER)
			ParseFail("Expected index name after DROP INDEX.");
		std::string IdxName = Nt->Value;
		AdvanceToken();
		return std::make_unique<DropIndexAST>(std::move(IdxName), IfExistsIdx);
	}
	if(MatchKeyword("SEQUENCE")) {
		bool IfExistsSeq = false;
		if(MatchKeyword("IF")) {
			if(!MatchKeyword("EXISTS"))
				ParseFail("Expected EXISTS in IF EXISTS clause");
			IfExistsSeq = true;
		}
		auto Nt = CurrentToken();
		if(!Nt || Nt->Type != TokenType::IDENTIFIER)
			ParseFail("Expected sequence name after DROP SEQUENCE.");
		std::string SeqName = Nt->Value;
		AdvanceToken();
		return std::make_unique<DropSequenceAST>(std::move(SeqName), IfExistsSeq);
	}
	if(MatchKeyword("VIEW")) {
		bool IfExistsDv = false;
		if(MatchKeyword("IF")) {
			if(!MatchKeyword("EXISTS"))
				ParseFail("Expected EXISTS in IF EXISTS clause");
			IfExistsDv = true;
		}
		auto Nt = CurrentToken();
		if(!Nt || Nt->Type != TokenType::IDENTIFIER)
			ParseFail("Expected view name after DROP VIEW.");
		std::string Vn = Nt->Value;
		AdvanceToken();
		return std::make_unique<DropViewAST>(std::move(Vn), IfExistsDv);
	}
	if(MatchKeyword("PROCEDURE"))
		return ParseDropProcedureStatement();
    if(!MatchKeyword("TABLE"))
        ParseFail("Expected VIEW, PROCEDURE, SEQUENCE, or TABLE after DROP.");
    bool IfExists = false;
    if(MatchKeyword("IF")) {
        if(!MatchKeyword("EXISTS"))
            ParseFail("Expected EXISTS in IF EXISTS clause");
        IfExists = true;
    }
    auto NameTok = CurrentToken();
    if(!NameTok || NameTok->Type != TokenType::IDENTIFIER)
        ParseFail("Expected table name after DROP TABLE.");
    std::string TableName = NameTok->Value;
    AdvanceToken();
	bool Cascade = MatchKeyword("CASCADE");
    return std::make_unique<DropAST>(TableName, IfExists, Cascade);
}

namespace {

std::optional<StorageLayout> ParseStorageHintFromQuery(std::string_view Query) {
	const std::string Needle = "/*+";
	size_t Pos = 0;
	while((Pos = Query.find(Needle, Pos)) != std::string_view::npos) {
		const size_t End = Query.find("*/", Pos + Needle.size());
		if(End == std::string_view::npos)
			break;
		std::string_view Body = Query.substr(Pos + Needle.size(), End - Pos - Needle.size());
		const size_t StoragePos = Body.find("STORAGE");
		if(StoragePos != std::string_view::npos) {
			const size_t Lp = Body.find('(', StoragePos);
			const size_t Rp = Body.find(')', Lp == std::string_view::npos ? StoragePos : Lp);
			if(Lp != std::string_view::npos && Rp != std::string_view::npos && Rp > Lp + 1) {
				std::string_view Arg = Body.substr(Lp + 1, Rp - Lp - 1);
				while(!Arg.empty() && std::isspace(static_cast<unsigned char>(Arg.front())))
					Arg.remove_prefix(1);
				while(!Arg.empty() && std::isspace(static_cast<unsigned char>(Arg.back())))
					Arg.remove_suffix(1);
				if(!Arg.empty())
					return StorageLayoutFromKeyword(Arg);
			}
		}
		Pos = End + 2;
	}
	return std::nullopt;
}

} // namespace

std::unique_ptr<SelectAST> Parser::ParseSelectArmThroughHaving() {
    bool Distinct = MatchKeyword("DISTINCT");
    std::vector<std::string> Columns;
    std::vector<std::unique_ptr<ExpressionAST>> ProjectionExprs;
    std::vector<GroupCombAgg> CombinedAggs;
    GroupAggMode AggMode = GroupAggMode::None;
    std::optional<std::string> CountDistinctCol;
    std::string CountOutputColumnForAst;
    std::vector<WindowSpec> WindowSpecs;
    auto FoldUpperAscii = [](std::string S) {
        for(char &C : S)
            C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
        return S;
    };
    auto ParseSelColumnToken = [&](const Token &Tok) {
        Columns.push_back(Tok.Value);
        ProjectionExprs.push_back(nullptr);
        AdvanceToken();
    };
    while(auto TokenOpt = CurrentToken()) {
        if(TokenOpt->Value == "FROM") {
            AdvanceToken();
            break;
        }
        if(TokenOpt->Value == "*") {
            Columns.push_back("*");
            AdvanceToken();
            if(auto NextToken = CurrentToken(); NextToken && NextToken->Value == "FROM") {
                AdvanceToken();
                break;
            }
            continue;
        }
        // COUNT(*) aggregate (requires GROUP BY; validated at codegen).
        const bool LooksLikeCount =
            (TokenOpt->Type == TokenType::KEYWORD && TokenOpt->Value == "COUNT") ||
            (TokenOpt->Type == TokenType::IDENTIFIER && FoldUpperAscii(TokenOpt->Value) == "COUNT");
        if(LooksLikeCount) {
            AdvanceToken();
            if(!CurrentToken() || CurrentToken()->Value != "(")
                ParseFail("Expected '(' after COUNT");
            AdvanceToken();
            if(MatchKeyword("DISTINCT")) {
                auto Dt = CurrentToken();
                if(!Dt || Dt->Type != TokenType::IDENTIFIER)
                    ParseFail("Expected column name after COUNT(DISTINCT");
                CountDistinctCol = Dt->Value;
                AdvanceToken();
                AggMode = GroupAggMode::CountDistinct;
            } else {
                if(!CurrentToken() || CurrentToken()->Value != "*")
                    ParseFail("Only COUNT(*) or COUNT(DISTINCT column) is supported in this dialect");
                AdvanceToken();
                AggMode = GroupAggMode::CountStar;
            }
            if(!CurrentToken() || CurrentToken()->Value != ")")
                ParseFail("Expected ')' after COUNT(...)");
            AdvanceToken();
            std::string CountOutName = "cnt";
            if(MatchKeyword("AS")) {
                auto At = CurrentToken();
                if(!At || At->Type != TokenType::IDENTIFIER)
                    ParseFail("Expected identifier alias after AS for COUNT");
                CountOutName = At->Value;
                AdvanceToken();
            }
            Columns.push_back(CountOutName);
            ProjectionExprs.push_back(nullptr);
            CountOutputColumnForAst = CountOutName;
            if(auto NextToken = CurrentToken(); NextToken && NextToken->Value == ",")
                AdvanceToken();
            continue;
        }
        const bool LooksLikeDenseRank =
            (TokenOpt->Type == TokenType::KEYWORD && TokenOpt->Value == "DENSE_RANK") ||
            (TokenOpt->Type == TokenType::IDENTIFIER && FoldUpperAscii(TokenOpt->Value) == "DENSE_RANK");
        const bool LooksLikeRank =
            (TokenOpt->Type == TokenType::KEYWORD && TokenOpt->Value == "RANK") ||
            (TokenOpt->Type == TokenType::IDENTIFIER && FoldUpperAscii(TokenOpt->Value) == "RANK");
        const bool LooksLikeRowNumber =
            (TokenOpt->Type == TokenType::KEYWORD && TokenOpt->Value == "ROW_NUMBER") ||
            (TokenOpt->Type == TokenType::IDENTIFIER && FoldUpperAscii(TokenOpt->Value) == "ROW_NUMBER");
        const bool LooksLikeLag =
            (TokenOpt->Type == TokenType::KEYWORD && TokenOpt->Value == "LAG") ||
            (TokenOpt->Type == TokenType::IDENTIFIER && FoldUpperAscii(TokenOpt->Value) == "LAG");
        const bool LooksLikeLead =
            (TokenOpt->Type == TokenType::KEYWORD && TokenOpt->Value == "LEAD") ||
            (TokenOpt->Type == TokenType::IDENTIFIER && FoldUpperAscii(TokenOpt->Value) == "LEAD");
        if(LooksLikeDenseRank || LooksLikeRank || LooksLikeRowNumber || LooksLikeLag || LooksLikeLead) {
            WindowSpec Ws;
            if(LooksLikeDenseRank)
                Ws.Kind = WindowFnKind::DenseRank;
            else if(LooksLikeRank)
                Ws.Kind = WindowFnKind::Rank;
            else if(LooksLikeRowNumber)
                Ws.Kind = WindowFnKind::RowNumber;
            else if(LooksLikeLag)
                Ws.Kind = WindowFnKind::Lag;
            else
                Ws.Kind = WindowFnKind::Lead;
            AdvanceToken();
            if(!CurrentToken() || CurrentToken()->Value != "(")
                ParseFail("Expected '(' after window function name");
            AdvanceToken();
            if(Ws.Kind == WindowFnKind::Lag || Ws.Kind == WindowFnKind::Lead) {
                auto ColTk = CurrentToken();
                if(!ColTk || ColTk->Type != TokenType::IDENTIFIER)
                    ParseFail("LAG/LEAD expect a column name inside ()");
                Ws.SourceColumn = ColTk->Value;
                AdvanceToken();
                Ws.FrameOffset = 1;
                if(CurrentToken() && CurrentToken()->Value == ",") {
                    AdvanceToken();
                    auto OffTk = CurrentToken();
                    if(!OffTk || OffTk->Type != TokenType::LITERAL)
                        ParseFail("LAG/LEAD offset must be a numeric literal");
                    try {
                        const long long Off = std::stoll(OffTk->Value);
                        if(Off < 0)
                            ParseFail("LAG/LEAD offset must be non-negative");
                        Ws.FrameOffset = Off;
                    } catch(...) {
                        ParseFail("LAG/LEAD offset must be a numeric literal");
                    }
                    AdvanceToken();
                }
            } else if(!CurrentToken() || CurrentToken()->Value != ")")
                ParseFail("ROW_NUMBER/RANK/DENSE_RANK expect an empty argument list ()");
            if(!CurrentToken() || CurrentToken()->Value != ")")
                ParseFail("Expected ')' after window function arguments");
            AdvanceToken();
            ParseWindowOverClause(Ws);
            std::string OutCol =
                Ws.Kind == WindowFnKind::RowNumber ?
                    std::string("rn") :
                Ws.Kind == WindowFnKind::Rank ?
                    std::string("rk") :
                Ws.Kind == WindowFnKind::DenseRank ?
                    std::string("dr") :
                Ws.Kind == WindowFnKind::Lag ?
                    std::string("lag_") + Ws.SourceColumn :
                    std::string("lead_") + Ws.SourceColumn;
            if(MatchKeyword("AS")) {
                auto At = CurrentToken();
                if(!At)
                    ParseFail("Expected alias after AS for window function");
                OutCol = At->Value;
                AdvanceToken();
            }
            Ws.OutputColumn = OutCol;
            if(WindowSpecs.size() >= Limits::MaxWindowFunctionsPerSelect)
                ParseFail("Too many window functions in one SELECT (see Limits::MaxWindowFunctionsPerSelect).");
            WindowSpecs.push_back(std::move(Ws));
            Columns.push_back(OutCol);
            ProjectionExprs.push_back(nullptr);
            if(auto NextToken = CurrentToken(); NextToken && NextToken->Value == ",")
                AdvanceToken();
            continue;
        }
        if(MatchKeyword("CASE")) {
            auto Ce = ParseSearchedCaseExpression();
            std::string Alias;
            if(MatchKeyword("AS")) {
                auto At = CurrentToken();
                if(!At || At->Type != TokenType::IDENTIFIER)
                    ParseFail("Expected identifier alias after AS for CASE");
                Alias = At->Value;
                AdvanceToken();
            } else
                Alias = std::string("_case") + std::to_string(NextAnonCaseAlias_++);
            Columns.push_back(std::move(Alias));
            ProjectionExprs.push_back(std::unique_ptr<ExpressionAST>(static_cast<ExpressionAST *>(Ce.release())));
            if(auto NextToken = CurrentToken(); NextToken && NextToken->Value == ",")
                AdvanceToken();
            continue;
        }
        if(MatchKeyword("CAST")) {
            if(!CurrentToken() || CurrentToken()->Value != "(")
                ParseFail("Expected '(' after CAST");
            AdvanceToken();
            auto CastOperand = ParseCaseScalarResult();
            if(!MatchKeyword("AS"))
                ParseFail("CAST requires AS <type>");
            const std::string DType = ParseDataType();
            const SqlCastTarget CT = ParseCastTargetFromDataType(DType);
            if(!CurrentToken() || CurrentToken()->Value != ")")
                ParseFail("Expected ')' after CAST type");
            AdvanceToken();
            std::string Alias;
            if(MatchKeyword("AS")) {
                auto At = CurrentToken();
                if(!At || At->Type != TokenType::IDENTIFIER)
                    ParseFail("Expected identifier alias after AS for CAST");
                Alias = At->Value;
                AdvanceToken();
            } else
                Alias = std::string("_cast") + std::to_string(NextAnonCastAlias_++);
            Columns.push_back(std::move(Alias));
            std::string TypeSql;
            if(CT == SqlCastTarget::Advanced)
                TypeSql = DType;
            auto Cx = std::make_unique<CastExprAST>(std::move(CastOperand), CT, std::move(TypeSql));
            ProjectionExprs.push_back(std::unique_ptr<ExpressionAST>(static_cast<ExpressionAST *>(Cx.release())));
            if(auto NextToken = CurrentToken(); NextToken && NextToken->Value == ",")
                AdvanceToken();
            continue;
        }
        if(MatchKeyword("GROUPING")) {
            if(MatchKeyword("ID")) {
                if(!CurrentToken() || CurrentToken()->Value != "(")
                    ParseFail("Expected '(' after GROUPING_ID");
                AdvanceToken();
                std::vector<std::string> GCols;
                for(;;) {
                    auto ColTk = CurrentToken();
                    if(!ColTk || ColTk->Type != TokenType::IDENTIFIER)
                        ParseFail("Expected column name in GROUPING_ID(...)");
                    GCols.push_back(ColTk->Value);
                    AdvanceToken();
                    if(CurrentToken() && CurrentToken()->Value == ",") {
                        AdvanceToken();
                        continue;
                    }
                    break;
                }
                if(!CurrentToken() || CurrentToken()->Value != ")")
                    ParseFail("Expected ')' after GROUPING_ID columns");
                AdvanceToken();
                std::string Alias;
                if(MatchKeyword("AS")) {
                    auto At = CurrentToken();
                    if(!At || At->Type != TokenType::IDENTIFIER)
                        ParseFail("Expected identifier alias after AS for GROUPING_ID");
                    Alias = At->Value;
                    AdvanceToken();
                } else
                    Alias = std::string("_grouping_id") + std::to_string(NextAnonCastAlias_++);
                Columns.push_back(std::move(Alias));
                ProjectionExprs.push_back(std::make_unique<GroupingIdExprAST>(std::move(GCols)));
                if(auto NextToken = CurrentToken(); NextToken && NextToken->Value == ",")
                    AdvanceToken();
                continue;
            }
            if(!CurrentToken() || CurrentToken()->Value != "(")
                ParseFail("Expected '(' after GROUPING");
            AdvanceToken();
            auto ColTk = CurrentToken();
            if(!ColTk || ColTk->Type != TokenType::IDENTIFIER)
                ParseFail("Expected column name in GROUPING(...)");
            const std::string GCol = ColTk->Value;
            AdvanceToken();
            if(!CurrentToken() || CurrentToken()->Value != ")")
                ParseFail("Expected ')' after GROUPING column");
            AdvanceToken();
            std::string Alias;
            if(MatchKeyword("AS")) {
                auto At = CurrentToken();
                if(!At || At->Type != TokenType::IDENTIFIER)
                    ParseFail("Expected identifier alias after AS for GROUPING");
                Alias = At->Value;
                AdvanceToken();
            } else
                Alias = std::string("_grouping_") + GCol;
            Columns.push_back(std::move(Alias));
            ProjectionExprs.push_back(std::make_unique<GroupingExprAST>(std::move(GCol)));
            if(auto NextToken = CurrentToken(); NextToken && NextToken->Value == ",")
                AdvanceToken();
            continue;
        }
        if(MatchKeyword("COALESCE")) {
            auto Ce = ParseCoalesceExpression();
            std::string Alias;
            if(MatchKeyword("AS")) {
                auto At = CurrentToken();
                if(!At || At->Type != TokenType::IDENTIFIER)
                    ParseFail("Expected identifier alias after AS for COALESCE");
                Alias = At->Value;
                AdvanceToken();
            } else
                Alias = std::string("_coalesce") + std::to_string(NextAnonCoalesceAlias_++);
            Columns.push_back(std::move(Alias));
            ProjectionExprs.push_back(std::unique_ptr<ExpressionAST>(static_cast<ExpressionAST *>(Ce.release())));
            if(auto NextToken = CurrentToken(); NextToken && NextToken->Value == ",")
                AdvanceToken();
            continue;
        }
        if(auto Sfb = TryParseScalarSqlBuiltinSelectExpr()) {
            std::string Alias;
            if(MatchKeyword("AS")) {
                auto At = CurrentToken();
                if(!At || At->Type != TokenType::IDENTIFIER)
                    ParseFail("Expected identifier alias after AS for scalar function");
                Alias = At->Value;
                AdvanceToken();
            } else
                Alias = std::string("_sqlfn") + std::to_string(NextAnonScalarSqlFnAlias_++);
            Columns.push_back(std::move(Alias));
            ProjectionExprs.push_back(std::unique_ptr<ExpressionAST>(static_cast<ExpressionAST *>(Sfb.release())));
            if(auto NextToken = CurrentToken(); NextToken && NextToken->Value == ",")
                AdvanceToken();
            continue;
        }
        const bool LooksLikeSum =
            (TokenOpt->Type == TokenType::KEYWORD && TokenOpt->Value == "SUM") ||
            (TokenOpt->Type == TokenType::IDENTIFIER && FoldUpperAscii(TokenOpt->Value) == "SUM");
        const bool LooksLikeMin =
            (TokenOpt->Type == TokenType::KEYWORD && TokenOpt->Value == "MIN") ||
            (TokenOpt->Type == TokenType::IDENTIFIER && FoldUpperAscii(TokenOpt->Value) == "MIN");
        const bool LooksLikeMax =
            (TokenOpt->Type == TokenType::KEYWORD && TokenOpt->Value == "MAX") ||
            (TokenOpt->Type == TokenType::IDENTIFIER && FoldUpperAscii(TokenOpt->Value) == "MAX");
        const bool LooksLikeAvg =
            (TokenOpt->Type == TokenType::KEYWORD && TokenOpt->Value == "AVG") ||
            (TokenOpt->Type == TokenType::IDENTIFIER && FoldUpperAscii(TokenOpt->Value) == "AVG");
        if(LooksLikeSum || LooksLikeMin || LooksLikeMax || LooksLikeAvg) {
            GroupCombAggKind Ak = GroupCombAggKind::Sum;
            if(LooksLikeMin)
                Ak = GroupCombAggKind::Min;
            else if(LooksLikeMax)
                Ak = GroupCombAggKind::Max;
            else if(LooksLikeAvg)
                Ak = GroupCombAggKind::Avg;
            AdvanceToken();
            if(!CurrentToken() || CurrentToken()->Value != "(")
                ParseFail("Expected '(' after aggregate function");
            AdvanceToken();
            auto ColTk = CurrentToken();
            if(!ColTk || ColTk->Type != TokenType::IDENTIFIER)
                ParseFail("Expected column identifier in aggregate");
            const std::string SrcCol = ColTk->Value;
            AdvanceToken();
            if(!CurrentToken() || CurrentToken()->Value != ")")
                ParseFail("Expected ')' after aggregate argument");
            AdvanceToken();
            std::string OutCol =
                Ak == GroupCombAggKind::Sum ?
                    std::string("sum_") + SrcCol :
                Ak == GroupCombAggKind::Min ?
                    std::string("min_") + SrcCol :
                Ak == GroupCombAggKind::Max ? std::string("max_") + SrcCol : std::string("avg_") + SrcCol;
            if(CurrentToken() && CurrentToken()->Type == TokenType::KEYWORD && CurrentToken()->Value == "OVER") {
                WindowSpec Ws;
                Ws.Kind = Ak == GroupCombAggKind::Sum ?
                              WindowFnKind::Sum :
                          Ak == GroupCombAggKind::Min ?
                              WindowFnKind::Min :
                          Ak == GroupCombAggKind::Max ? WindowFnKind::Max : WindowFnKind::Avg;
                Ws.SourceColumn = SrcCol;
                ParseWindowOverClause(Ws);
                if(MatchKeyword("AS")) {
                    auto AliasTok = CurrentToken();
                    if(!AliasTok)
                        ParseFail("Expected alias after AS for window aggregate");
                    OutCol = AliasTok->Value;
                    AdvanceToken();
                }
                Ws.OutputColumn = OutCol;
                if(WindowSpecs.size() >= Limits::MaxWindowFunctionsPerSelect)
                    ParseFail("Too many window functions in one SELECT (see Limits::MaxWindowFunctionsPerSelect).");
                WindowSpecs.push_back(std::move(Ws));
                Columns.push_back(OutCol);
                ProjectionExprs.push_back(nullptr);
                if(auto NextToken = CurrentToken(); NextToken && NextToken->Value == ",")
                    AdvanceToken();
                continue;
            }
            if(MatchKeyword("AS")) {
                auto AliasTok = CurrentToken();
                if(!AliasTok)
                    ParseFail("Expected alias after AS for aggregate");
                OutCol = AliasTok->Value;
                AdvanceToken();
            }
            CombinedAggs.push_back(GroupCombAgg{Ak, SrcCol, OutCol});
            Columns.push_back(OutCol);
            ProjectionExprs.push_back(nullptr);
            if(auto NextToken = CurrentToken(); NextToken && NextToken->Value == ",")
                AdvanceToken();
            continue;
        }
        ParseSelColumnToken(*TokenOpt);
        if(auto NextToken = CurrentToken(); NextToken && NextToken->Value == ",") {
            AdvanceToken();
            continue;
        }
        if(auto NextToken = CurrentToken(); NextToken && NextToken->Value == "FROM") {
            AdvanceToken();
            break;
        }
    }
    std::optional<std::string> AsOfTs;
    std::optional<MatchRecognizeSpec> MatchSpec;
    if(auto TokenOpt = CurrentToken()) {
        std::string TableName = TokenOpt->Value;
        AdvanceToken();
        ApplyCteSubstitution(TableName);
        if(MatchKeyword("FOR")) {
            if(!MatchKeyword("SYSTEM") || !MatchKeyword("TIME") || !MatchKeyword("AS") || !MatchKeyword("OF"))
                ParseFail("Expected FOR SYSTEM TIME AS OF <timestamp> after table name");
            auto TsTok = CurrentToken();
            if(!TsTok)
                ParseFail("AS OF expects a timestamp literal");
            AsOfTs = TsTok->Value;
            AdvanceToken();
        } else if(CurrentToken() && CurrentToken()->Value == "AS") {
            const size_t Save = CurrentIndex_;
            AdvanceToken();
            if(CurrentToken() && CurrentToken()->Value == "OF") {
                AdvanceToken();
                auto TsTok = CurrentToken();
                if(!TsTok)
                    ParseFail("AS OF expects a timestamp literal");
                AsOfTs = TsTok->Value;
                AdvanceToken();
            } else
                CurrentIndex_ = Save;
        }
        if(auto AliasTok = CurrentToken();
            AliasTok && AliasTok->Type == TokenType::IDENTIFIER &&
            AliasTok->Value != "WHERE" && AliasTok->Value != "ORDER" && AliasTok->Value != "LIMIT" &&
            AliasTok->Value != "GROUP" && AliasTok->Value != "HAVING" && AliasTok->Value != "OFFSET" &&
            AliasTok->Value != "INNER" && AliasTok->Value != "LEFT" && AliasTok->Value != "RIGHT" &&
            AliasTok->Value != "FULL" && AliasTok->Value != "OUTER" && AliasTok->Value != "CROSS" &&
            AliasTok->Value != "JOIN" && AliasTok->Value != "UNION" && AliasTok->Value != "INTERSECT" &&
            AliasTok->Value != "EXCEPT" && AliasTok->Value != "MATCH" && AliasTok->Value != "FOR")
            AdvanceToken();

        std::vector<JoinClause> Joins;
        while(CurrentToken() && CurrentToken()->Value == ",") {
            AdvanceToken();
            auto CommaTbl = CurrentToken();
            if(!CommaTbl || CommaTbl->Type != TokenType::IDENTIFIER)
                ParseFail("Expected table name after ',' in FROM clause");
            std::string RT = CommaTbl->Value;
            AdvanceToken();
            ApplyCteSubstitution(RT);
            JoinClause Jc;
            Jc.Kind = SqlJoinKind::Cross;
            Jc.RightTable = RT;
            if(auto At = CurrentToken();
               At && At->Type == TokenType::IDENTIFIER &&
               At->Value != "WHERE" && At->Value != "ORDER" && At->Value != "LIMIT" &&
               At->Value != "GROUP" && At->Value != "HAVING" && At->Value != "OFFSET" &&
               At->Value != "INNER" && At->Value != "LEFT" && At->Value != "RIGHT" &&
               At->Value != "FULL" && At->Value != "OUTER" && At->Value != "CROSS" &&
               At->Value != "JOIN" && At->Value != "UNION" && At->Value != "INTERSECT" &&
               At->Value != "EXCEPT") {
                AdvanceToken();
            }
            Joins.push_back(std::move(Jc));
        }

        auto ParseQualifiedCol = [&]() -> std::string {
            auto Id1 = CurrentToken();
            if(!Id1 || Id1->Type != TokenType::IDENTIFIER)
                ParseFail("Expected column name");
            AdvanceToken();
            if(CurrentToken() && CurrentToken()->Value == ".") {
                AdvanceToken();
                auto Id2 = CurrentToken();
                if(!Id2 || Id2->Type != TokenType::IDENTIFIER)
                    ParseFail("Expected column after '.'");
                AdvanceToken();
                return Id2->Value;
            }
            return Id1->Value;
        };

        auto TryConsumeJoin = [&]() -> bool {
            auto Tk = CurrentToken();
            if(!Tk)
                return false;
            const std::string V = Tk->Value;
            if(V == "WHERE" || V == "GROUP" || V == "ORDER" || V == "LIMIT" || V == "HAVING" ||
               V == "OFFSET" || V == "UNION" || V == "INTERSECT" || V == "EXCEPT")
                return false;
            SqlJoinKind JK = SqlJoinKind::Inner;
            if(V == "INNER") {
                AdvanceToken();
                if(!MatchKeyword("JOIN"))
                    ParseFail("Expected JOIN after INNER");
                JK = SqlJoinKind::Inner;
            } else if(V == "LEFT") {
                AdvanceToken();
                if(CurrentToken() && CurrentToken()->Value == "OUTER")
                    AdvanceToken();
                if(!MatchKeyword("JOIN"))
                    ParseFail("Expected JOIN after LEFT");
                JK = SqlJoinKind::Left;
            } else if(V == "RIGHT") {
                AdvanceToken();
                if(CurrentToken() && CurrentToken()->Value == "OUTER")
                    AdvanceToken();
                if(!MatchKeyword("JOIN"))
                    ParseFail("Expected JOIN after RIGHT");
                JK = SqlJoinKind::Right;
            } else if(V == "FULL") {
                AdvanceToken();
                if(CurrentToken() && CurrentToken()->Value == "OUTER")
                    AdvanceToken();
                if(!MatchKeyword("JOIN"))
                    ParseFail("Expected JOIN after FULL");
                JK = SqlJoinKind::Full;
            } else if(V == "CROSS") {
                AdvanceToken();
                if(!MatchKeyword("JOIN"))
                    ParseFail("Expected JOIN after CROSS");
                JK = SqlJoinKind::Cross;
            } else if(V == "JOIN") {
                AdvanceToken();
                JK = SqlJoinKind::Inner;
            } else
                return false;

            auto Rt = CurrentToken();
            if(!Rt || Rt->Type != TokenType::IDENTIFIER)
                ParseFail("Expected table name after JOIN");
            JoinClause Jc;
            Jc.Kind = JK;
            Jc.RightTable = Rt->Value;
            AdvanceToken();
            ApplyCteSubstitution(Jc.RightTable);
            if(auto At = CurrentToken();
               At && At->Type == TokenType::IDENTIFIER &&
               At->Value != "WHERE" && At->Value != "ORDER" && At->Value != "LIMIT" &&
               At->Value != "GROUP" && At->Value != "INNER" && At->Value != "LEFT" &&
               At->Value != "RIGHT" && At->Value != "FULL" && At->Value != "OUTER" &&
               At->Value != "CROSS" && At->Value != "JOIN" && At->Value != "ON" &&
               At->Value != "UNION" && At->Value != "INTERSECT" && At->Value != "EXCEPT") {
                AdvanceToken();
            }
            if(JK != SqlJoinKind::Cross) {
                if(!MatchKeyword("ON"))
                    ParseFail("Expected ON clause for JOIN");
                for(;;) {
                    const std::string LC = ParseQualifiedCol();
                    if(!CurrentToken() || CurrentToken()->Value != "=")
                        ParseFail("Expected '=' in JOIN ON predicate");
                    AdvanceToken();
                    const std::string RC = ParseQualifiedCol();
                    Jc.OnPairs.emplace_back(LC, RC);
                    if(CurrentToken() && CurrentToken()->Value == "AND") {
                        AdvanceToken();
                        continue;
                    }
                    break;
                }
            }
            Joins.push_back(std::move(Jc));
            return true;
        };
        while(TryConsumeJoin()) {}

        if(MatchKeyword("MATCH_RECOGNIZE")) {
            if(!CurrentToken() || CurrentToken()->Value != "(")
                ParseFail("MATCH_RECOGNIZE expects '('");
            AdvanceToken();
            MatchRecognizeSpec Mr;
            if(MatchKeyword("ORDER")) {
                if(!MatchKeyword("BY"))
                    ParseFail("Expected BY after ORDER in MATCH_RECOGNIZE");
                auto Col = CurrentToken();
                if(!Col || Col->Type != TokenType::IDENTIFIER)
                    ParseFail("MATCH_RECOGNIZE ORDER BY expects a column");
                Mr.OrderColumn = Col->Value;
                AdvanceToken();
            }
            if(!MatchKeyword("PATTERN"))
                ParseFail("MATCH_RECOGNIZE expects PATTERN clause");
            if(!CurrentToken() || CurrentToken()->Value != "(")
                ParseFail("PATTERN expects '('");
            AdvanceToken();
            auto PatTok = CurrentToken();
            if(!PatTok)
                ParseFail("PATTERN expects a row pattern");
            Mr.Pattern = PatTok->Value;
            AdvanceToken();
            if(!CurrentToken() || CurrentToken()->Value != ")")
                ParseFail("PATTERN expects ')'");
            AdvanceToken();
            if(MatchKeyword("DEFINE")) {
                for(;;) {
                    auto Sym = CurrentToken();
                    if(!Sym || Sym->Type != TokenType::IDENTIFIER)
                        break;
                    MatchRecognizeDefine Def;
                    Def.Symbol = Sym->Value;
                    AdvanceToken();
                    if(!MatchKeyword("AS"))
                        ParseFail("DEFINE expects symbol AS predicate");
                    auto Pred = ParseBinaryOperation();
                    Def.Predicate =
                        Pred ? std::unique_ptr<ExpressionAST>(static_cast<ExpressionAST *>(Pred.release())) :
                               nullptr;
                    Mr.Defines.push_back(std::move(Def));
                    if(CurrentToken() && CurrentToken()->Value == ",") {
                        AdvanceToken();
                        continue;
                    }
                    break;
                }
            }
            if(!CurrentToken() || CurrentToken()->Value != ")")
                ParseFail("MATCH_RECOGNIZE expects closing ')'");
            AdvanceToken();
            MatchSpec = std::move(Mr);
        }

        std::unique_ptr<ExpressionAST> WhereClause;
        if(auto NextToken = CurrentToken(); NextToken && NextToken->Value == "WHERE") {
            AdvanceToken();
            auto Node = ParseBinaryOperation();
            WhereClause =
                Node ? std::unique_ptr<ExpressionAST>(static_cast<ExpressionAST*>(Node.release())) : nullptr;
        }
        std::vector<std::string> GroupByCols;
        std::vector<std::vector<std::string>> GroupingSetsList;
        GroupOlapModifier OlapMod = GroupOlapModifier::None;
        if(auto Gt = CurrentToken(); Gt && Gt->Value == "GROUP") {
            AdvanceToken();
            if(!MatchKeyword("BY"))
                ParseFail("Expected BY after GROUP");
            while(auto ColTk = CurrentToken()) {
                if(ColTk->Value == "GROUPING" || ColTk->Value == "ORDER" || ColTk->Value == "LIMIT" ||
                   ColTk->Value == "HAVING" || ColTk->Value == "UNION" || ColTk->Value == "INTERSECT" ||
                   ColTk->Value == "EXCEPT" || ColTk->Value == "WITH")
                    break;
                if(ColTk->Type != TokenType::IDENTIFIER && ColTk->Type != TokenType::KEYWORD)
                    ParseFail("GROUP BY expects a column name");
                std::string GKey = ColTk->Value;
                AdvanceToken();
                if(CurrentToken() && CurrentToken()->Value == ".") {
                    AdvanceToken();
                    auto Id2 = CurrentToken();
                    if(!Id2 || Id2->Type != TokenType::IDENTIFIER)
                        ParseFail("Expected column name after '.' in GROUP BY");
                    GKey = Id2->Value;
                    AdvanceToken();
                }
                GroupByCols.push_back(std::move(GKey));
                if(auto Ct = CurrentToken(); Ct && Ct->Value == ",")
                    AdvanceToken();
                else
                    break;
            }
            if(MatchKeyword("GROUPING")) {
                if(!MatchKeyword("SETS"))
                    ParseFail("Expected SETS after GROUPING");
                OlapMod = GroupOlapModifier::GroupingSets;
            }
            if(OlapMod == GroupOlapModifier::GroupingSets) {
                if(!CurrentToken() || CurrentToken()->Value != "(")
                    ParseFail("GROUPING SETS expects '(' after SETS");
                AdvanceToken();
                for(;;) {
                    if(!CurrentToken() || CurrentToken()->Value != "(")
                        ParseFail("GROUPING SETS expects '(' before each grouping set");
                    AdvanceToken();
                    std::vector<std::string> OneSet;
                    while(auto ColTk = CurrentToken()) {
                        if(ColTk->Value == ")") {
                            AdvanceToken();
                            break;
                        }
                        if(ColTk->Type != TokenType::IDENTIFIER && ColTk->Type != TokenType::KEYWORD)
                            ParseFail("GROUPING SETS expects column names");
                        std::string GKey = ColTk->Value;
                        AdvanceToken();
                        if(CurrentToken() && CurrentToken()->Value == ".") {
                            AdvanceToken();
                            auto Id2 = CurrentToken();
                            if(!Id2 || Id2->Type != TokenType::IDENTIFIER)
                                ParseFail("Expected column name after '.' in GROUPING SETS");
                            GKey = Id2->Value;
                            AdvanceToken();
                        }
                        OneSet.push_back(std::move(GKey));
                        if(CurrentToken() && CurrentToken()->Value == ",")
                            AdvanceToken();
                    }
                    GroupingSetsList.push_back(std::move(OneSet));
                    if(CurrentToken() && CurrentToken()->Value == ",") {
                        AdvanceToken();
                        continue;
                    }
                    break;
                }
                if(!CurrentToken() || CurrentToken()->Value != ")")
                    ParseFail("GROUPING SETS expects ')' after grouping set list");
                AdvanceToken();
            } else if(MatchKeyword("WITH")) {
                if(MatchKeyword("ROLLUP"))
                    OlapMod = GroupOlapModifier::Rollup;
                else if(MatchKeyword("CUBE"))
                    OlapMod = GroupOlapModifier::Cube;
                else
                    ParseFail("GROUP BY WITH expects ROLLUP or CUBE");
            }
        }
        if(OlapMod == GroupOlapModifier::GroupingSets && GroupByCols.empty()) {
            for(const auto &S : GroupingSetsList) {
                for(const auto &C : S) {
                    if(std::find(GroupByCols.begin(), GroupByCols.end(), C) == GroupByCols.end())
                        GroupByCols.push_back(C);
                }
            }
        }
        if(OlapMod != GroupOlapModifier::None && GroupByCols.empty())
            ParseFail("ROLLUP, CUBE, and GROUPING SETS require at least one GROUP BY column");
        if(OlapMod != GroupOlapModifier::None && AggMode == GroupAggMode::CountDistinct)
            ParseFail("COUNT(DISTINCT) cannot be combined with ROLLUP, CUBE, or GROUPING SETS in this dialect");
        std::unique_ptr<ExpressionAST> HavingClause;
        if(auto Ht = CurrentToken(); Ht && Ht->Value == "HAVING") {
            AdvanceToken();
            const bool SavedAggPred = AllowAggCallsInPredicate_;
            AllowAggCallsInPredicate_ = true;
            auto Node = ParseBinaryOperation();
            AllowAggCallsInPredicate_ = SavedAggPred;
            HavingClause =
                Node ? std::unique_ptr<ExpressionAST>(static_cast<ExpressionAST*>(Node.release()))
                     : nullptr;
        }
        if(!CombinedAggs.empty() && GroupByCols.empty())
            ParseFail("SUM/MIN/MAX/AVG require GROUP BY columns");
        if(HavingClause && GroupByCols.empty())
            ParseFail("HAVING requires GROUP BY");
        std::string CountColArg;
        if(AggMode == GroupAggMode::CountStar || AggMode == GroupAggMode::CountDistinct)
            CountColArg = CountOutputColumnForAst;
        auto Sel = std::make_unique<SelectAST>(
            Columns, TableName, std::move(WhereClause), std::move(HavingClause),
            std::vector<std::pair<std::string, bool>>{}, -1, 0, Distinct, std::move(GroupByCols), AggMode,
            CountDistinctCol, std::move(WindowSpecs), std::move(Joins), std::move(CombinedAggs),
            std::move(ProjectionExprs), CountColArg, OlapMod, std::move(GroupingSetsList));
		if(auto Hint = ParseStorageHintFromQuery(Query_))
			Sel->SetStorageHint(*Hint);
		if(AsOfTs)
			Sel->SetAsOfTimestamp(std::move(*AsOfTs));
		if(MatchSpec)
			Sel->SetMatchRecognize(std::move(*MatchSpec));
		return Sel;
    }
    ParseFail("Expected table name after FROM");
}

std::unique_ptr<ExpressionAST> Parser::ParseSetValueExpression(const std::optional<std::string> &TargetAlias,
                                                             const std::optional<std::string> &SourceAlias,
                                                             bool AllowExcluded) {
    auto ParsePrimarySet = [&]() -> std::unique_ptr<ExpressionAST> {
        if(MatchKeyword("NULL"))
            return std::make_unique<NullLiteralAST>();
        auto T = CurrentToken();
        if(!T)
            ParseFail("Unexpected end of input in SET expression");
        if(T->Type == TokenType::IDENTIFIER ||
           (AllowExcluded && T->Type == TokenType::KEYWORD && T->Value == "EXCLUDED")) {
            std::string First = T->Value;
            AdvanceToken();
            if(AllowExcluded && First == "EXCLUDED" && CurrentToken() && CurrentToken()->Value == ".") {
                AdvanceToken();
                auto C = CurrentToken();
                if(!C || C->Type != TokenType::IDENTIFIER)
                    ParseFail("Expected column after EXCLUDED.");
                std::string Col = C->Value;
                AdvanceToken();
                return std::make_unique<QualifiedRefAST>(QualifiedRefAST::Role::Excluded, std::move(Col));
            }
            if(CurrentToken() && CurrentToken()->Value == ".") {
                AdvanceToken();
                auto C = CurrentToken();
                if(!C || C->Type != TokenType::IDENTIFIER)
                    ParseFail("Expected column after '.' in SET expression.");
                std::string Col = C->Value;
                AdvanceToken();
                if(SourceAlias && First == *SourceAlias)
                    return std::make_unique<QualifiedRefAST>(QualifiedRefAST::Role::Source, std::move(Col));
                if(TargetAlias && First == *TargetAlias)
                    return std::make_unique<QualifiedRefAST>(QualifiedRefAST::Role::Target, std::move(Col));
                ParseFail("Unknown table alias in SET expression.");
            }
            if(!TargetAlias && !SourceAlias)
                return std::make_unique<ColumnRefAST>(std::move(First));
            return std::make_unique<LiteralAST>(std::move(First));
        }
        std::string Lit = T->Value;
        AdvanceToken();
        return std::make_unique<LiteralAST>(std::move(Lit));
    };

    auto LHS = ParsePrimarySet();
    while(auto Nxt = CurrentToken()) {
        if(Nxt->Value != "+" && Nxt->Value != "-" && Nxt->Value != "*" && Nxt->Value != "/")
            break;
        std::string Op = Nxt->Value;
        AdvanceToken();
        auto RHS = ParsePrimarySet();
        LHS = std::make_unique<BinaryOpAST>(std::move(LHS), std::move(Op), std::move(RHS));
    }
    return LHS;
}

std::unique_ptr<ExpressionAST> Parser::ParseCaseScalarResult() {
    if(MatchKeyword("NULL"))
        return std::make_unique<NullLiteralAST>();
    auto T = CurrentToken();
    if(!T)
        ParseFail("Unexpected end of input in CASE expression (expected scalar)");
    if(T->Type == TokenType::IDENTIFIER
       || (T->Type == TokenType::KEYWORD && T->Value != "TRUE" && T->Value != "FALSE")) {
        std::string N = T->Value;
        AdvanceToken();
        return std::make_unique<ColumnRefAST>(std::move(N));
    }
    std::string Lit = T->Value;
    AdvanceToken();
    return std::make_unique<LiteralAST>(std::move(Lit));
}

std::unique_ptr<CaseExprAST> Parser::ParseSearchedCaseExpression() {
    std::vector<CaseExprAST::Arm> Arms;
    while(MatchKeyword("WHEN")) {
        auto Pred = ParseBinaryOperation();
        if(!MatchKeyword("THEN"))
            ParseFail("Expected THEN after CASE WHEN predicate");
        auto ThenE = ParseCaseScalarResult();
        CaseExprAST::Arm A;
        A.When = AsExpr(std::move(Pred));
        A.Then = std::move(ThenE);
        Arms.push_back(std::move(A));
    }
    if(Arms.empty())
        ParseFail("CASE requires at least one WHEN arm");
    std::unique_ptr<ExpressionAST> ElseE;
    if(MatchKeyword("ELSE"))
        ElseE = ParseCaseScalarResult();
    if(!MatchKeyword("END"))
        ParseFail("Expected END to close CASE expression");
    return std::make_unique<CaseExprAST>(std::move(Arms), std::move(ElseE));
}

std::unique_ptr<CaseExprAST> Parser::ParseCoalesceExpression() {
	if(!CurrentToken() || CurrentToken()->Value != "(")
		ParseFail("Expected '(' after COALESCE");
	AdvanceToken();
	std::vector<std::unique_ptr<ExpressionAST>> Args;
	for(;;) {
		Args.push_back(ParseCaseScalarResult());
		if(CurrentToken() && CurrentToken()->Value == ",") {
			AdvanceToken();
			continue;
		}
		break;
	}
	if(!CurrentToken() || CurrentToken()->Value != ")")
		ParseFail("Expected ')' after COALESCE arguments");
	AdvanceToken();
	if(Args.size() < 2)
		ParseFail("COALESCE requires at least two arguments");
	std::vector<CaseExprAST::Arm> Arms;
	for(size_t I = 0; I + 1 < Args.size(); ++I) {
		if(dynamic_cast<const NullLiteralAST *>(Args[I].get()))
			continue;
		auto Subj = CloneCaseScalarExpr(Args[I].get());
		if(!Subj)
			ParseFail("COALESCE arguments must be literals, NULL, or plain column references in this dialect");
		CaseExprAST::Arm A;
		A.When = std::make_unique<IsNullPredAST>(std::move(Subj), true);
		auto ThenE = CloneCaseScalarExpr(Args[I].get());
		if(!ThenE)
			ParseFail("COALESCE arguments must be literals, NULL, or plain column references in this dialect");
		A.Then = std::move(ThenE);
		Arms.push_back(std::move(A));
	}
	auto ElseE = CloneCaseScalarExpr(Args.back().get());
	if(!ElseE)
		ParseFail("COALESCE arguments must be literals, NULL, or plain column references in this dialect");
	return std::make_unique<CaseExprAST>(std::move(Arms), std::move(ElseE));
}

std::unique_ptr<ScalarFuncExprAST> Parser::TryParseScalarSqlBuiltinSelectExpr() {
	auto T = CurrentToken();
	if(!T || (T->Type != TokenType::KEYWORD && T->Type != TokenType::IDENTIFIER))
		return nullptr;
	std::string Name = T->Value;
	for(char &C : Name)
		C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
	{
		const size_t Save = CurrentIndex_;
		AdvanceToken();
		const bool Ok = CurrentToken() && CurrentToken()->Value == "(";
		CurrentIndex_ = Save;
		if(!Ok)
			return nullptr;
	}
	static const std::unordered_set<std::string> Starters = {"SUBSTRING", "UPPER", "LOWER", "CHAR_LENGTH",
	    "CHARACTER_LENGTH", "POSITION", "TRIM", "CONCAT", "EXTRACT", "DATE_ADD", "DATE_SUB", "DATE_DIFF",
	    "DATE_TRUNC", "TIME_BUCKET", "TIMESTAMP_DIFF", "STRUCT_FIELD", "MAP_GET", "COMPLEX_REAL", "COMPLEX_IMAG",
	    "COMPLEX_MUL", "VECTOR_DOT", "VECTOR_ADD", "VECTOR_NORM", "MATRIX_VEC"};
	if(const auto Builtin = MathSci::LookupBuiltin(Name)) {
		AdvanceToken();
		if(!CurrentToken() || CurrentToken()->Value != "(")
			ParseFail("Expected '(' after " + Name);
		AdvanceToken();
		std::vector<std::unique_ptr<ExpressionAST>> Args;
		if(Builtin->Arity.Max > 0) {
			for(;;) {
				Args.push_back(ParseCaseScalarResult());
				if(static_cast<int>(Args.size()) >= Builtin->Arity.Max)
					break;
				if(CurrentToken() && CurrentToken()->Value == ",") {
					AdvanceToken();
					continue;
				}
				break;
			}
		}
		if(static_cast<int>(Args.size()) < Builtin->Arity.Min)
			ParseFail(Name + " expects at least " + std::to_string(Builtin->Arity.Min) + " argument(s)");
		if(!CurrentToken() || CurrentToken()->Value != ")")
			ParseFail("Expected ')' closing " + Name);
		AdvanceToken();
		return std::make_unique<ScalarFuncExprAST>(Builtin->Fn, std::move(Args));
	}
	if(!Starters.count(Name))
		return nullptr;
	AdvanceToken();
	if(!CurrentToken() || CurrentToken()->Value != "(")
		ParseFail("Expected '(' after " + Name);
	AdvanceToken();

	auto FinishClose = [&]() {
		if(!CurrentToken() || CurrentToken()->Value != ")")
			ParseFail("Expected ')' closing scalar function call");
		AdvanceToken();
	};

	std::vector<std::unique_ptr<ExpressionAST>> Args;

	if(Name == "UPPER" || Name == "LOWER" || Name == "CHAR_LENGTH" || Name == "CHARACTER_LENGTH") {
		Args.push_back(ParseCaseScalarResult());
		ScalarSqlFn K = ScalarSqlFn::Upper;
		if(Name == "LOWER")
			K = ScalarSqlFn::Lower;
		else if(Name == "CHAR_LENGTH" || Name == "CHARACTER_LENGTH")
			K = ScalarSqlFn::CharLength;
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(K, std::move(Args));
	}
	if(Name == "SUBSTRING") {
		Args.push_back(ParseCaseScalarResult());
		if(MatchKeyword("FROM")) {
			Args.push_back(ParseCaseScalarResult());
			if(MatchKeyword("FOR"))
				Args.push_back(ParseCaseScalarResult());
		} else {
			if(!CurrentToken() || CurrentToken()->Value != ",")
				ParseFail("SUBSTRING expects FROM … FOR … or comma-separated arguments");
			AdvanceToken();
			Args.push_back(ParseCaseScalarResult());
			if(!CurrentToken() || CurrentToken()->Value != ",")
				ParseFail("SUBSTRING comma form requires three arguments");
			AdvanceToken();
			Args.push_back(ParseCaseScalarResult());
		}
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::SubstringFromFor, std::move(Args));
	}
	if(Name == "POSITION") {
		Args.push_back(ParseCaseScalarResult());
		if(!MatchKeyword("IN"))
			ParseFail("POSITION requires IN between search string and source string");
		Args.push_back(ParseCaseScalarResult());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::PositionIn, std::move(Args));
	}
	if(Name == "TRIM") {
		ScalarSqlFn Tk = ScalarSqlFn::TrimBoth;
		if(MatchKeyword("LEADING")) {
			Tk = ScalarSqlFn::TrimLeading;
			if(!MatchKeyword("FROM"))
				ParseFail("TRIM LEADING requires FROM");
			Args.push_back(ParseCaseScalarResult());
		} else if(MatchKeyword("TRAILING")) {
			Tk = ScalarSqlFn::TrimTrailing;
			if(!MatchKeyword("FROM"))
				ParseFail("TRIM TRAILING requires FROM");
			Args.push_back(ParseCaseScalarResult());
		} else if(MatchKeyword("BOTH")) {
			if(!MatchKeyword("FROM"))
				ParseFail("TRIM BOTH requires FROM");
			Args.push_back(ParseCaseScalarResult());
		} else
			Args.push_back(ParseCaseScalarResult());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(Tk, std::move(Args));
	}
	if(Name == "CONCAT") {
		Args.push_back(ParseCaseScalarResult());
		while(CurrentToken() && CurrentToken()->Value == ",") {
			AdvanceToken();
			Args.push_back(ParseCaseScalarResult());
		}
		if(Args.size() < 2)
			ParseFail("CONCAT expects at least two arguments");
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::ConcatVariadic, std::move(Args));
	}
	if(Name == "EXTRACT") {
		ScalarSqlFn Field = ScalarSqlFn::ExtractYear;
		if(MatchKeyword("YEAR"))
			Field = ScalarSqlFn::ExtractYear;
		else if(MatchKeyword("MONTH"))
			Field = ScalarSqlFn::ExtractMonth;
		else if(MatchKeyword("DAY"))
			Field = ScalarSqlFn::ExtractDay;
		else if(MatchKeyword("HOUR"))
			Field = ScalarSqlFn::ExtractHour;
		else if(MatchKeyword("MINUTE"))
			Field = ScalarSqlFn::ExtractMinute;
		else if(MatchKeyword("SECOND"))
			Field = ScalarSqlFn::ExtractSecond;
		else if(MatchKeyword("EPOCH"))
			Field = ScalarSqlFn::ExtractEpoch;
		else
			ParseFail("EXTRACT supports YEAR, MONTH, DAY, HOUR, MINUTE, SECOND, or EPOCH in this dialect");
		if(!MatchKeyword("FROM"))
			ParseFail("EXTRACT requires FROM");
		Args.push_back(ParseCaseScalarResult());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(Field, std::move(Args));
	}
	if(Name == "DATE_ADD") {
		Args.push_back(ParseCaseScalarResult());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail("DATE_ADD expects two arguments");
		AdvanceToken();
		Args.push_back(ParseCaseScalarResult());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::DateAddDays, std::move(Args));
	}
	if(Name == "DATE_SUB") {
		Args.push_back(ParseCaseScalarResult());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail("DATE_SUB expects two arguments");
		AdvanceToken();
		Args.push_back(ParseCaseScalarResult());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::DateSubDays, std::move(Args));
	}
	if(Name == "DATE_DIFF") {
		Args.push_back(ParseCaseScalarResult());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail("DATE_DIFF expects two arguments");
		AdvanceToken();
		Args.push_back(ParseCaseScalarResult());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::DateDiffDays, std::move(Args));
	}
	if(Name == "DATE_TRUNC") {
		Args.push_back(ParseCaseScalarResult());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail("DATE_TRUNC expects unit and timestamp arguments");
		AdvanceToken();
		Args.push_back(ParseCaseScalarResult());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::DateTrunc, std::move(Args));
	}
	if(Name == "TIME_BUCKET") {
		Args.push_back(ParseCaseScalarResult());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail("TIME_BUCKET expects timestamp and bucket width in seconds");
		AdvanceToken();
		Args.push_back(ParseCaseScalarResult());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::TimeBucketSeconds, std::move(Args));
	}
	if(Name == "TIMESTAMP_DIFF") {
		Args.push_back(ParseCaseScalarResult());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail("TIMESTAMP_DIFF expects two timestamp arguments");
		AdvanceToken();
		Args.push_back(ParseCaseScalarResult());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::TimestampDiffSeconds, std::move(Args));
	}
	if(Name == "STRUCT_FIELD") {
		Args.push_back(ParseCaseScalarResult());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail("STRUCT_FIELD expects struct value and field name");
		AdvanceToken();
		Args.push_back(ParseCaseScalarResult());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::StructField, std::move(Args));
	}
	if(Name == "MAP_GET") {
		Args.push_back(ParseCaseScalarResult());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail("MAP_GET expects map value and key");
		AdvanceToken();
		Args.push_back(ParseCaseScalarResult());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::MapGet, std::move(Args));
	}
	if(Name == "COMPLEX_REAL" || Name == "COMPLEX_IMAG") {
		Args.push_back(ParseCaseScalarResult());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(
		    Name == "COMPLEX_REAL" ? ScalarSqlFn::ComplexReal : ScalarSqlFn::ComplexImag, std::move(Args));
	}
	if(Name == "COMPLEX_MUL") {
		Args.push_back(ParseCaseScalarResult());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail("COMPLEX_MUL expects two complex values");
		AdvanceToken();
		Args.push_back(ParseCaseScalarResult());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::ComplexMul, std::move(Args));
	}
	if(Name == "VECTOR_DOT" || Name == "VECTOR_ADD") {
		Args.push_back(ParseCaseScalarResult());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail(Name + " expects two vector arguments");
		AdvanceToken();
		Args.push_back(ParseCaseScalarResult());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(
		    Name == "VECTOR_DOT" ? ScalarSqlFn::VectorDot : ScalarSqlFn::VectorAdd, std::move(Args));
	}
	if(Name == "VECTOR_NORM") {
		Args.push_back(ParseCaseScalarResult());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::VectorNorm, std::move(Args));
	}
	if(Name == "MATRIX_VEC") {
		Args.push_back(ParseCaseScalarResult());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail("MATRIX_VEC expects matrix and vector arguments");
		AdvanceToken();
		Args.push_back(ParseCaseScalarResult());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::MatrixVec, std::move(Args));
	}
	ParseFail("Internal: scalar builtin not handled: " + Name);
}

ASTNode Parser::ParseSelectStatement() {
    AdvanceToken();
    std::vector<std::unique_ptr<SelectAST>> Arms;
    std::vector<CompoundSetOpKind> Ops;
    Arms.push_back(ParseSelectArmThroughHaving());
    for(;;) {
        if(MatchKeyword("UNION")) {
            const bool All = MatchKeyword("ALL");
            if(!All)
                (void)MatchKeyword("DISTINCT");
            auto Sel = CurrentToken();
            if(!Sel || Sel->Type != TokenType::SELECT)
                ParseFail("Expected SELECT after UNION");
            AdvanceToken();
            Arms.push_back(ParseSelectArmThroughHaving());
            Ops.push_back(All ? CompoundSetOpKind::UnionAll : CompoundSetOpKind::UnionDistinct);
            continue;
        }
        if(MatchKeyword("INTERSECT")) {
            const bool All = MatchKeyword("ALL");
            if(!All)
                (void)MatchKeyword("DISTINCT");
            auto Sel = CurrentToken();
            if(!Sel || Sel->Type != TokenType::SELECT)
                ParseFail("Expected SELECT after INTERSECT");
            AdvanceToken();
            Arms.push_back(ParseSelectArmThroughHaving());
            Ops.push_back(All ? CompoundSetOpKind::IntersectAll : CompoundSetOpKind::Intersect);
            continue;
        }
        if(MatchKeyword("EXCEPT")) {
            const bool All = MatchKeyword("ALL");
            if(!All)
                (void)MatchKeyword("DISTINCT");
            auto Sel = CurrentToken();
            if(!Sel || Sel->Type != TokenType::SELECT)
                ParseFail("Expected SELECT after EXCEPT");
            AdvanceToken();
            Arms.push_back(ParseSelectArmThroughHaving());
            Ops.push_back(All ? CompoundSetOpKind::ExceptAll : CompoundSetOpKind::Except);
            continue;
        }
        break;
    }

    std::vector<std::pair<std::string, bool>> OrderByColumns;
    if(auto NextToken = CurrentToken(); NextToken && NextToken->Value == "ORDER") {
        AdvanceToken();
        if(auto ByToken = CurrentToken(); ByToken && ByToken->Value == "BY") {
            AdvanceToken();
            while(auto ColumnToken = CurrentToken()) {
                std::string ColumnName = ColumnToken->Value;
                AdvanceToken();
                bool Ascending = true;
                if(auto OrderToken = CurrentToken(); OrderToken) {
                    if(OrderToken->Value == "ASC") {
                        Ascending = true;
                        AdvanceToken();
                    } else if(OrderToken->Value == "DESC") {
                        Ascending = false;
                        AdvanceToken();
                    }
                }
                OrderByColumns.emplace_back(ColumnName, Ascending);
                if(auto CommaToken = CurrentToken(); CommaToken && CommaToken->Value == ",") {
                    AdvanceToken();
                    continue;
                }
                break;
            }
        }
    }
    int64_t Limit = -1;
    int64_t Offset = 0;
	if(auto OffTok = CurrentToken(); OffTok && OffTok->Value == "OFFSET") {
		AdvanceToken();
		if(auto Ov = CurrentToken()) {
			Offset = std::stoll(Ov->Value);
			AdvanceToken();
		}
		if(MatchKeyword("ROWS"))
			(void)0;
	}
    if(auto NextToken = CurrentToken(); NextToken && NextToken->Value == "LIMIT") {
        AdvanceToken();
        if(auto LimitToken = CurrentToken()) {
            Limit = std::stoll(LimitToken->Value);
            AdvanceToken();
            if(auto CommaTok = CurrentToken(); CommaTok && CommaTok->Value == ",") {
                AdvanceToken();
                if(auto SecondLimitTok = CurrentToken()) {
                    int64_t Count = std::stoll(SecondLimitTok->Value);
                    AdvanceToken();
                    Offset = Limit;
                    Limit = Count;
                }
            } else if(auto OffsetToken = CurrentToken(); OffsetToken && OffsetToken->Value == "OFFSET") {
                AdvanceToken();
                if(auto OffsetValueToken = CurrentToken()) {
                    Offset = std::stoll(OffsetValueToken->Value);
                    AdvanceToken();
                }
            }
        }
    } else if(auto FetchTok = CurrentToken(); FetchTok && FetchTok->Value == "FETCH") {
		AdvanceToken();
		if(!MatchKeyword("FIRST"))
			ParseFail("FETCH expects FIRST");
		if(auto Cnt = CurrentToken()) {
			Limit = std::stoll(Cnt->Value);
			AdvanceToken();
		}
		if(MatchKeyword("ROWS"))
			MatchKeyword("ONLY");
	}

    if(Arms.size() == 1) {
        Arms[0]->ApplyQueryOrdering(std::move(OrderByColumns), Limit, Offset);
        return std::unique_ptr<StatementAST>(std::move(Arms[0]));
    }
    auto Comp = std::make_unique<CompoundSelectAST>();
    Comp->Arms = std::move(Arms);
    Comp->Ops = std::move(Ops);
    Comp->OrderByColumns = std::move(OrderByColumns);
    Comp->Limit = Limit;
    Comp->Offset = Offset;
    return Comp;
}

ASTNode Parser::ParseInsertStatement() {
    AdvanceToken();
    if(!MatchKeyword("INTO"))
        ParseFail("Expected INTO after INSERT");
    auto TableToken = CurrentToken();
    if(!TableToken)
        ParseFail("Expected table name after INSERT INTO");
    std::string TableName = TableToken->Value;
    AdvanceToken();
    std::vector<std::string> Columns;
    if(CurrentToken() && CurrentToken()->Value == "(") {
        AdvanceToken();
        while (auto TokenOpt = CurrentToken()) {
            if(TokenOpt->Value == ")") {
                AdvanceToken();
                break;
            }
            std::string Col = TokenOpt->Value;
            Columns.push_back(Col);
            AdvanceToken();
            if(CurrentToken() && CurrentToken()->Value == ",")
                AdvanceToken();
        }
    }
    if(MatchKeyword("BULK")) {
        auto CountTok = CurrentToken();
        if(!CountTok)
            ParseFail("Expected integer row count after BULK");
        int64_t BulkCount = std::stoll(CountTok->Value);
        AdvanceToken();
        int64_t BulkStart = 1;
        int64_t BulkStep = 1;
        if(MatchKeyword("START")) {
            auto STok = CurrentToken();
            if(!STok)
                ParseFail("Expected integer after START in INSERT BULK");
            BulkStart = std::stoll(STok->Value);
            AdvanceToken();
        }
        if(MatchKeyword("STEP")) {
            auto TTok = CurrentToken();
            if(!TTok)
                ParseFail("Expected integer after STEP in INSERT BULK");
            BulkStep = std::stoll(TTok->Value);
            AdvanceToken();
        }
        return std::make_unique<BulkInsertAST>(TableName, std::move(Columns), BulkCount, BulkStart, BulkStep);
    }
    if(!MatchKeyword("VALUES"))
        ParseFail("Expected VALUES or BULK in INSERT statement");

    std::vector<std::vector<std::string>> AllValues; // Changed to vector of vectors

    while(CurrentToken() && CurrentToken()->Value == "(") {
        AdvanceToken(); // Consume '('
        std::vector<std::string> CurrentValues;
        while(auto TokenOpt = CurrentToken()) {
            if(TokenOpt->Value == ")") {
                AdvanceToken(); // Consume ')'
                break;
            }
            std::string Val;
            if(MatchKeyword("NEXTVAL")) {
                if(!CurrentToken() || CurrentToken()->Value != "(")
                    ParseFail("Expected '(' after NEXTVAL");
                AdvanceToken();
                auto SeqTok = CurrentToken();
                if(!SeqTok || SeqTok->Type != TokenType::IDENTIFIER)
                    ParseFail("Expected sequence name in NEXTVAL(...)");
                Val = std::string("__astral_nextval__:") + SeqTok->Value;
                AdvanceToken();
                if(!CurrentToken() || CurrentToken()->Value != ")")
                    ParseFail("Expected ')' after NEXTVAL sequence name");
                AdvanceToken();
            } else {
                Val = TokenOpt->Value;
                AdvanceToken();
            }
            CurrentValues.push_back(Val);
            if(CurrentToken() && CurrentToken()->Value == ",") {
                AdvanceToken(); // Consume ',' between values within a set
            }
        }
        AllValues.push_back(CurrentValues);

        if(CurrentToken() && CurrentToken()->Value == ",") {
            AdvanceToken(); // Consume ',' between value sets
        } else {
            break; // No more value sets
        }
    }

    if(AllValues.empty()) {
        ParseFail("No values provided in INSERT statement");
    }

    std::optional<UpsertSpec> UpsertOpt;
    if(MatchKeyword("ON")) {
        if(!MatchKeyword("CONFLICT"))
            ParseFail("Expected CONFLICT after ON");
        std::vector<std::string> ConflictCols;
        if(CurrentToken() && CurrentToken()->Value == "(") {
            AdvanceToken();
            while(auto TokenOpt = CurrentToken()) {
                if(TokenOpt->Value == ")") {
                    AdvanceToken();
                    break;
                }
                if(TokenOpt->Type != TokenType::IDENTIFIER)
                    ParseFail("ON CONFLICT column list expects identifiers");
                ConflictCols.push_back(TokenOpt->Value);
                AdvanceToken();
                if(CurrentToken() && CurrentToken()->Value == ",")
                    AdvanceToken();
            }
        }
        if(!MatchKeyword("DO"))
            ParseFail("Expected DO in ON CONFLICT clause");
        UpsertSpec Us;
        Us.ConflictColumns = std::move(ConflictCols);
        if(MatchKeyword("NOTHING")) {
            Us.Mode = UpsertSpec::OnConflict::Nothing;
        } else if(MatchKeyword("UPDATE")) {
            Us.Mode = UpsertSpec::OnConflict::Update;
            if(!MatchKeyword("SET"))
                ParseFail("Expected SET after DO UPDATE");
            while(auto TokenOpt = CurrentToken()) {
                if(TokenOpt->Type == TokenType::PUNCTUATION && TokenOpt->Value == ";")
                    break;
                if(TokenOpt->Value == "WHERE")
                    ParseFail("ON CONFLICT UPDATE does not support WHERE");
                std::string ColumnName = TokenOpt->Value;
                AdvanceToken();
                if(!MatchToken(TokenType::PUNCTUATION, "="))
                    ParseFail("Expected '=' in ON CONFLICT UPDATE assignment");
                UpsertAssign Asg;
                Asg.Column = ColumnName;
                Asg.Value = ParseSetValueExpression(std::nullopt, std::nullopt, true);
                Us.UpdateAssignments.push_back(std::move(Asg));
                if(CurrentToken() && CurrentToken()->Value == ",")
                    AdvanceToken();
            }
        } else
            ParseFail("Expected NOTHING or UPDATE after ON CONFLICT DO");
        UpsertOpt = std::move(Us);
    }

    auto TableAst = std::make_unique<TableAST>(TableName);
    return std::make_unique<InsertAST>(std::move(TableAst), Columns, AllValues, std::move(UpsertOpt));
}

ASTNode Parser::ParseMergeStatement() {
    AdvanceToken();
    if(!MatchKeyword("INTO"))
        ParseFail("Expected INTO after MERGE");
    auto TTok = CurrentToken();
    if(!TTok || TTok->Type != TokenType::IDENTIFIER)
        ParseFail("Expected target table name in MERGE");
    std::string TargetTable = TTok->Value;
    AdvanceToken();
    std::string TargetAlias = TargetTable;
    if(MatchKeyword("AS")) {
        auto ATok = CurrentToken();
        if(!ATok || ATok->Type != TokenType::IDENTIFIER)
            ParseFail("Expected alias after AS in MERGE INTO");
        TargetAlias = ATok->Value;
        AdvanceToken();
    }
    if(!MatchKeyword("USING"))
        ParseFail("Expected USING in MERGE");
    auto STok = CurrentToken();
    if(!STok || STok->Type != TokenType::IDENTIFIER)
        ParseFail("Expected source table name in MERGE");
    std::string SourceTable = STok->Value;
    AdvanceToken();
    std::string SourceAlias = SourceTable;
    if(MatchKeyword("AS")) {
        auto BTok = CurrentToken();
        if(!BTok || BTok->Type != TokenType::IDENTIFIER)
            ParseFail("Expected alias after AS in MERGE USING");
        SourceAlias = BTok->Value;
        AdvanceToken();
    }
    if(!MatchKeyword("ON"))
        ParseFail("Expected ON in MERGE");
    std::vector<std::pair<std::string, std::string>> OnKeyPairs;
    for(;;) {
        auto La = CurrentToken();
        if(!La || La->Type != TokenType::IDENTIFIER)
            ParseFail("Expected join column qualifier in MERGE ON");
        AdvanceToken();
        if(!MatchToken(TokenType::PUNCTUATION, "."))
            ParseFail("Expected '.' in MERGE ON join column");
        auto Lc = CurrentToken();
        if(!Lc || Lc->Type != TokenType::IDENTIFIER)
            ParseFail("Expected join column name in MERGE ON");
        const std::string OnLeftAlias = La->Value;
        const std::string OnLeftCol = Lc->Value;
        AdvanceToken();
        if(!MatchToken(TokenType::PUNCTUATION, "="))
            ParseFail("Expected '=' in MERGE ON");
        auto Ra = CurrentToken();
        if(!Ra || Ra->Type != TokenType::IDENTIFIER)
            ParseFail("Expected join column qualifier after '=' in MERGE ON");
        AdvanceToken();
        if(!MatchToken(TokenType::PUNCTUATION, "."))
            ParseFail("Expected '.' in MERGE ON join column (right)");
        auto Rc = CurrentToken();
        if(!Rc || Rc->Type != TokenType::IDENTIFIER)
            ParseFail("Expected join column name in MERGE ON (right)");
        const std::string OnRightAlias = Ra->Value;
        const std::string OnRightCol = Rc->Value;
        AdvanceToken();
        std::string TgtCol;
        std::string SrcCol;
        if(OnLeftAlias == TargetAlias && OnRightAlias == SourceAlias) {
            TgtCol = OnLeftCol;
            SrcCol = OnRightCol;
        } else if(OnLeftAlias == SourceAlias && OnRightAlias == TargetAlias) {
            TgtCol = OnRightCol;
            SrcCol = OnLeftCol;
        } else
            ParseFail("MERGE ON must compare target alias column to source alias column.");
        OnKeyPairs.emplace_back(TgtCol, SrcCol);
        if(CurrentToken() && CurrentToken()->Value == "AND") {
            AdvanceToken();
            continue;
        }
        break;
    }
    if(OnKeyPairs.empty())
        ParseFail("MERGE ON requires at least one key equality.");
    std::vector<MergeMatchedSet> Matched;
    std::vector<MergeInsertField> NotMatched;
    bool HaveMatched = false;
    bool HaveNotMatched = false;
    while(MatchKeyword("WHEN")) {
        if(MatchKeyword("NOT")) {
            if(!MatchKeyword("MATCHED"))
                ParseFail("Expected MATCHED after NOT in MERGE");
            if(HaveNotMatched)
                ParseFail("Duplicate WHEN NOT MATCHED in MERGE");
            HaveNotMatched = true;
            if(!MatchKeyword("THEN"))
                ParseFail("Expected THEN after WHEN NOT MATCHED");
            if(!MatchToken(TokenType::INSERT, "INSERT"))
                ParseFail("Expected INSERT in MERGE WHEN NOT MATCHED");
            std::vector<std::string> InsertColumns;
            if(!CurrentToken() || CurrentToken()->Value != "(")
                ParseFail("Expected column list in MERGE INSERT");
            AdvanceToken();
            while(auto TokenOpt = CurrentToken()) {
                if(TokenOpt->Value == ")") {
                    AdvanceToken();
                    break;
                }
                if(TokenOpt->Type != TokenType::IDENTIFIER)
                    ParseFail("INSERT column name expected in MERGE");
                InsertColumns.push_back(TokenOpt->Value);
                AdvanceToken();
                if(CurrentToken() && CurrentToken()->Value == ",")
                    AdvanceToken();
            }
            if(InsertColumns.empty())
                ParseFail("MERGE INSERT requires at least one column");
            if(!MatchKeyword("VALUES"))
                ParseFail("Expected VALUES in MERGE INSERT");
            if(!CurrentToken() || CurrentToken()->Value != "(")
                ParseFail("Expected '(' before MERGE INSERT values");
            AdvanceToken();
            NotMatched.reserve(InsertColumns.size());
            for(size_t Ix = 0; Ix < InsertColumns.size(); ++Ix) {
                if(Ix > 0) {
                    if(!CurrentToken() || CurrentToken()->Value != ",")
                        ParseFail("Expected ',' between MERGE INSERT values");
                    AdvanceToken();
                }
                if(!CurrentToken())
                    ParseFail("Expected value in MERGE INSERT");
                MergeInsertField Cell;
                Cell.Column = InsertColumns[Ix];
                Cell.Value =
                    ParseSetValueExpression(TargetAlias, SourceAlias, false);
                NotMatched.push_back(std::move(Cell));
            }
            if(!CurrentToken() || CurrentToken()->Value != ")")
                ParseFail("Expected ')' after MERGE INSERT values");
            AdvanceToken();
        } else if(MatchKeyword("MATCHED")) {
            if(HaveMatched)
                ParseFail("Duplicate WHEN MATCHED in MERGE");
            HaveMatched = true;
            if(!MatchKeyword("THEN"))
                ParseFail("Expected THEN after WHEN MATCHED");
            if(!MatchKeyword("UPDATE"))
                ParseFail("Expected UPDATE in MERGE WHEN MATCHED");
            if(!MatchKeyword("SET"))
                ParseFail("Expected SET in MERGE WHEN MATCHED");
            while(auto TokenOpt = CurrentToken()) {
                if(TokenOpt->Type == TokenType::KEYWORD && TokenOpt->Value == "WHEN")
                    break;
                if(TokenOpt->Type == TokenType::PUNCTUATION && TokenOpt->Value == ";")
                    break;
                std::string TargetCol;
                if(TokenOpt->Type != TokenType::IDENTIFIER)
                    ParseFail("Expected target column in MERGE UPDATE SET");
                std::string First = TokenOpt->Value;
                AdvanceToken();
                if(CurrentToken() && CurrentToken()->Value == ".") {
                    AdvanceToken();
                    auto Rest = CurrentToken();
                    if(!Rest || Rest->Type != TokenType::IDENTIFIER)
                        ParseFail("Expected column after '.' in MERGE UPDATE target");
                    if(First != TargetAlias)
                        ParseFail("MERGE UPDATE target must use the target alias");
                    TargetCol = Rest->Value;
                    AdvanceToken();
                } else
                    TargetCol = std::move(First);
                if(!MatchToken(TokenType::PUNCTUATION, "="))
                    ParseFail("Expected '=' in MERGE UPDATE assignment");
                if(!CurrentToken())
                    ParseFail("Expected value in MERGE UPDATE assignment");
                MergeMatchedSet Cell;
                Cell.TargetColumn = std::move(TargetCol);
                Cell.Value = ParseSetValueExpression(TargetAlias, SourceAlias, false);
                Matched.push_back(std::move(Cell));
                if(CurrentToken() && CurrentToken()->Value == ",")
                    AdvanceToken();
            }
        } else
            ParseFail("MERGE WHEN expects MATCHED or NOT MATCHED");
    }
    if(!HaveMatched && !HaveNotMatched)
        ParseFail("MERGE requires at least one WHEN MATCHED or WHEN NOT MATCHED branch");

    auto Out = std::make_unique<MergeAST>();
    Out->TargetTable = std::move(TargetTable);
    Out->TargetAlias = std::move(TargetAlias);
    Out->SourceTable = std::move(SourceTable);
    Out->SourceAlias = std::move(SourceAlias);
    Out->OnKeyPairs = std::move(OnKeyPairs);
    Out->Matched = std::move(Matched);
    Out->NotMatched = std::move(NotMatched);
    Out->HasMatchedBranch = HaveMatched;
    Out->HasNotMatchedBranch = HaveNotMatched;
    return Out;
}

ASTNode Parser::ParseUpdateStatement() {
    AdvanceToken();
    auto TableToken = CurrentToken();
    if(!TableToken)
        ParseFail("Expected table name after UPDATE");
    std::string TableName = TableToken->Value;
    AdvanceToken();
    if(!MatchKeyword("SET"))
        ParseFail("Expected SET in UPDATE statement");
    std::vector<std::pair<std::string, std::unique_ptr<ExpressionAST>>> Assignments;
    while(auto TokenOpt = CurrentToken()) {
        if(TokenOpt->Value == "WHERE") break;
        if(TokenOpt->Type == TokenType::PUNCTUATION && TokenOpt->Value == ";") break;
        std::string ColumnName = TokenOpt->Value;
        AdvanceToken();
        if(!MatchToken(TokenType::PUNCTUATION, "="))
            ParseFail("Expected '=' in assignment of UPDATE statement");
        Assignments.emplace_back(ColumnName, ParseSetValueExpression(std::nullopt, std::nullopt, false));
        if(CurrentToken() && CurrentToken()->Value == ",")
            AdvanceToken();
    }
    std::unique_ptr<ExpressionAST> Condition = nullptr;
    if(CurrentToken() && CurrentToken()->Value == "WHERE") {
        AdvanceToken();
        auto node = ParseBinaryOperation();
        Condition = node ? std::unique_ptr<ExpressionAST>(static_cast<ExpressionAST*>(node.release())) : nullptr;
    }
    return std::make_unique<UpdateAST>(TableName, std::move(Assignments), std::move(Condition));
}

ASTNode Parser::ParseDeleteStatement() {
    AdvanceToken();
    if(!MatchKeyword("FROM"))
        ParseFail("Expected FROM in DELETE statement");
    auto TableToken = CurrentToken();
    if(!TableToken)
        ParseFail("Expected table name in DELETE statement");
    std::string TableName = TableToken->Value;
    AdvanceToken();
    std::unique_ptr<ExpressionAST> Condition = nullptr;
    if(CurrentToken() && CurrentToken()->Value == "WHERE") {
        AdvanceToken();
        auto node = ParseBinaryOperation();
        Condition = node ? std::unique_ptr<ExpressionAST>(static_cast<ExpressionAST*>(node.release())) : nullptr;
    }
    return std::make_unique<DeleteAST>(TableName, std::move(Condition));
}

ASTNode Parser::ParseGrantStatement() {
	AdvanceToken();
	if(MatchKeyword("ROLE")) {
		if(!CurrentToken())
			ParseFail("Expected role name after GRANT ROLE.");
		std::string RoleName = CurrentToken()->Value;
		AdvanceToken();
		if(!MatchKeyword("TO"))
			ParseFail("Expected TO after role name in GRANT ROLE.");
		if(!CurrentToken())
			ParseFail("Expected user after TO in GRANT ROLE.");
		std::string UserName = CurrentToken()->Value;
		AdvanceToken();
		return std::make_unique<GrantRoleMembershipAST>(std::move(RoleName), std::move(UserName));
	}
	if(!CurrentToken())
		ParseFail("Expected permission after GRANT");
	const std::string PermissionString = CurrentToken()->Value;
	Permissions Perms;
	if(PermissionString == "SELECT")
		Perms = Permissions::Select;
	else if(PermissionString == "INSERT")
		Perms = Permissions::Insert;
	else if(PermissionString == "UPDATE")
		Perms = Permissions::Update;
	else if(PermissionString == "DELETE")
		Perms = Permissions::Delete;
	else if(PermissionString == "TRUNCATE")
		Perms = Permissions::Truncate;
	else if(PermissionString == "REFERENCES")
		Perms = Permissions::References;
	else if(PermissionString == "TRIGGER")
		Perms = Permissions::Trigger;
	else if(PermissionString == "ALL")
		Perms = Permissions::All;
	else
		ParseFail("Unknown permission in GRANT: " + PermissionString);
	AdvanceToken();
	std::vector<std::string> Columns;
	if(MatchToken(TokenType::PUNCTUATION, "(")) {
		while(true) {
			if(!CurrentToken())
				ParseFail("Unexpected EOF in GRANT column list.");
			if(CurrentToken()->Type == TokenType::PUNCTUATION && CurrentToken()->Value == ")") {
				AdvanceToken();
				break;
			}
			if(CurrentToken()->Type != TokenType::IDENTIFIER)
				ParseFail("Expected column name in GRANT column list.");
			Columns.push_back(CurrentToken()->Value);
			AdvanceToken();
			if(CurrentToken() && CurrentToken()->Type == TokenType::PUNCTUATION && CurrentToken()->Value == ",") {
				AdvanceToken();
				continue;
			}
			if(!MatchToken(TokenType::PUNCTUATION, ")"))
				ParseFail("Expected ')' to close GRANT column list.");
			break;
		}
	}
	if(!MatchKeyword("ON"))
		ParseFail("Expected ON after permission in GRANT");
	std::string TableName;
	if(CurrentToken() && CurrentToken()->Type == TokenType::IDENTIFIER) {
		TableName = CurrentToken()->Value;
		AdvanceToken();
	}
	if(!MatchKeyword("TO"))
		ParseFail("Expected TO after table in GRANT");
	bool GranteeIsRole = false;
	if(MatchKeyword("ROLE"))
		GranteeIsRole = true;
	if(!CurrentToken())
		ParseFail("Expected grantee after TO in GRANT");
	std::string Grantee = CurrentToken()->Value;
	AdvanceToken();
	bool WithGrantOption = MatchKeyword("WITH") && MatchKeyword("GRANT") && MatchKeyword("OPTION");
	return std::make_unique<GrantAST>(std::move(Grantee), Perms, TableName, std::move(Columns), GranteeIsRole,
	                                  WithGrantOption);
}

ASTNode Parser::ParseRevokeStatement() {
	AdvanceToken();
	if(MatchKeyword("ROLE")) {
		if(!CurrentToken())
			ParseFail("Expected role name after REVOKE ROLE.");
		std::string RoleName = CurrentToken()->Value;
		AdvanceToken();
		if(!MatchKeyword("FROM"))
			ParseFail("Expected FROM after role name in REVOKE ROLE.");
		if(!CurrentToken())
			ParseFail("Expected user after FROM in REVOKE ROLE.");
		std::string UserName = CurrentToken()->Value;
		AdvanceToken();
		return std::make_unique<RevokeRoleMembershipAST>(std::move(RoleName), std::move(UserName));
	}
	if(!CurrentToken())
		ParseFail("Expected permission after REVOKE");
	const std::string PermissionString = CurrentToken()->Value;
	Permissions Perms;
	if(PermissionString == "SELECT")
		Perms = Permissions::Select;
	else if(PermissionString == "INSERT")
		Perms = Permissions::Insert;
	else if(PermissionString == "UPDATE")
		Perms = Permissions::Update;
	else if(PermissionString == "DELETE")
		Perms = Permissions::Delete;
	else if(PermissionString == "TRUNCATE")
		Perms = Permissions::Truncate;
	else if(PermissionString == "REFERENCES")
		Perms = Permissions::References;
	else if(PermissionString == "TRIGGER")
		Perms = Permissions::Trigger;
	else if(PermissionString == "ALL")
		Perms = Permissions::All;
	else
		ParseFail("Unknown permission in REVOKE: " + PermissionString);
	AdvanceToken();
	std::vector<std::string> Columns;
	if(MatchToken(TokenType::PUNCTUATION, "(")) {
		while(true) {
			if(!CurrentToken())
				ParseFail("Unexpected EOF in REVOKE column list.");
			if(CurrentToken()->Type == TokenType::PUNCTUATION && CurrentToken()->Value == ")") {
				AdvanceToken();
				break;
			}
			if(CurrentToken()->Type != TokenType::IDENTIFIER)
				ParseFail("Expected column name in REVOKE column list.");
			Columns.push_back(CurrentToken()->Value);
			AdvanceToken();
			if(CurrentToken() && CurrentToken()->Type == TokenType::PUNCTUATION && CurrentToken()->Value == ",") {
				AdvanceToken();
				continue;
			}
			if(!MatchToken(TokenType::PUNCTUATION, ")"))
				ParseFail("Expected ')' to close REVOKE column list.");
			break;
		}
	}
	if(!MatchKeyword("ON"))
		ParseFail("Expected ON after permission in REVOKE");
	std::string TableName;
	if(CurrentToken() && CurrentToken()->Type == TokenType::IDENTIFIER) {
		TableName = CurrentToken()->Value;
		AdvanceToken();
	}
	if(!MatchKeyword("FROM"))
		ParseFail("Expected FROM after table in REVOKE");
	bool GranteeIsRole = false;
	if(MatchKeyword("ROLE"))
		GranteeIsRole = true;
	if(!CurrentToken())
		ParseFail("Expected grantee after FROM in REVOKE");
	std::string Grantee = CurrentToken()->Value;
	AdvanceToken();
	return std::make_unique<RevokeAST>(std::move(Grantee), Perms, TableName, std::move(Columns), GranteeIsRole);
}

void Parser::ParseWindowOverClause(WindowSpec &Ws) {
    if(!MatchKeyword("OVER"))
        ParseFail("Expected OVER after window function");
    if(!CurrentToken() || CurrentToken()->Value != "(")
        ParseFail("Expected '(' after OVER");
    AdvanceToken();
    Ws.PartitionBy.clear();
    if(MatchKeyword("PARTITION")) {
        if(!MatchKeyword("BY"))
            ParseFail("Expected BY after PARTITION in window OVER clause");
        std::unordered_set<std::string> PartSeen;
        for(;;) {
            auto Pc = CurrentToken();
            if(!Pc || Pc->Type != TokenType::IDENTIFIER)
                ParseFail(
                    "Expected PARTITION BY column (use a plain column name — reserved words belong in ORDER BY)");
            const std::string Pname = Pc->Value;
            if(PartSeen.count(Pname))
                ParseFail("Duplicate column \"" + Pname + "\" in PARTITION BY");
            if(Ws.PartitionBy.size() >= Limits::MaxWindowPartitionColumns)
                ParseFail(
                    "PARTITION BY column list exceeds the configured maximum "
                    "(see Limits::MaxWindowPartitionColumns)");
            PartSeen.insert(Pname);
            Ws.PartitionBy.push_back(Pname);
            AdvanceToken();
            if(CurrentToken() && CurrentToken()->Value == ",") {
                AdvanceToken();
                continue;
            }
            break;
        }
        if(Ws.PartitionBy.empty())
            ParseFail("PARTITION BY requires at least one column");
    }
    if(!MatchKeyword("ORDER"))
        ParseFail(Ws.PartitionBy.empty() ? "WINDOW requires ORDER BY after OVER (" :
                                          "WINDOW requires ORDER BY after PARTITION BY (only PARTITION BY without "
                                          "ORDER BY is invalid)");
    if(!MatchKeyword("BY"))
        ParseFail("WINDOW requires ORDER BY");
    auto OCol = CurrentToken();
    if(!OCol)
        ParseFail("Expected column after ORDER BY in window");
    Ws.OrderColumn = OCol->Value;
    AdvanceToken();
    Ws.OrderAscending = true;
    if(auto Ot = CurrentToken(); Ot && Ot->Value == "ASC") {
        Ws.OrderAscending = true;
        AdvanceToken();
    } else if(Ot && Ot->Value == "DESC") {
        Ws.OrderAscending = false;
        AdvanceToken();
    }
    if(MatchKeyword("ROWS")) {
        if(!MatchKeyword("BETWEEN"))
            ParseFail("Expected BETWEEN after ROWS in window frame");
        auto ParseRowsBound = [&]() -> WindowFrameBound {
            WindowFrameBound B;
            if(MatchKeyword("UNBOUNDED")) {
                if(MatchKeyword("PRECEDING"))
                    B.Kind = WindowFrameBoundKind::UnboundedPreceding;
                else if(MatchKeyword("FOLLOWING"))
                    B.Kind = WindowFrameBoundKind::UnboundedFollowing;
                else
                    ParseFail("Expected PRECEDING or FOLLOWING after UNBOUNDED");
                return B;
            }
            if(MatchKeyword("CURRENT")) {
                if(!MatchKeyword("ROW"))
                    ParseFail("Expected ROW after CURRENT in window frame");
                B.Kind = WindowFrameBoundKind::CurrentRow;
                return B;
            }
            auto Nt = CurrentToken();
            if(!Nt || Nt->Type != TokenType::LITERAL)
                ParseFail("Expected offset, CURRENT ROW, or UNBOUNDED in window frame");
            try {
                B.Offset = std::stoll(Nt->Value);
            } catch(...) {
                ParseFail("Window frame offset must be a non-negative integer");
            }
            if(B.Offset < 0)
                ParseFail("Window frame offset must be non-negative");
            AdvanceToken();
            if(MatchKeyword("PRECEDING"))
                B.Kind = WindowFrameBoundKind::Preceding;
            else if(MatchKeyword("FOLLOWING"))
                B.Kind = WindowFrameBoundKind::Following;
            else
                ParseFail("Expected PRECEDING or FOLLOWING after frame offset");
            return B;
        };
        Ws.FrameStart = ParseRowsBound();
        if(!MatchKeyword("AND"))
            ParseFail("Expected AND between window frame bounds");
        Ws.FrameEnd = ParseRowsBound();
        Ws.HasExplicitRowsFrame = true;
    }
    if(!CurrentToken() || CurrentToken()->Value != ")")
        ParseFail("Expected ')' closing OVER clause");
    AdvanceToken();
}

ASTNode Parser::ParseWhereClause() {
    if(CurrentToken() && CurrentToken()->Value == "WHERE") {
        AdvanceToken();
        return ParseBinaryOperation();
    }
    return nullptr;
}

std::unique_ptr<ExpressionAST> Parser::ParseStandalonePredicateExpression() {
	auto Node = ParseBinaryOperation();
	if(!Node)
		ParseFail("CHECK expression parse failed (expected a boolean expression).");
	if(CurrentIndex_ < Tokens_.size())
		ParseFail("Unexpected trailing tokens after CHECK expression.");
	return std::unique_ptr<ExpressionAST>(static_cast<ExpressionAST *>(Node.release()));
}

ASTNode Parser::ParseBinaryOperation() {
    auto LHS = ParseUnaryOrPostfixPredicate();
    return ParseBinaryOperation(0, std::move(LHS));
}

ASTNode Parser::ParseBinaryOperation(int MinPrec, ASTNode LHS) {
    while(true) {
        if(!CurrentToken()) break;
        int CurrentPrec = GetTokenPrecedence(*CurrentToken());
        if(CurrentPrec < MinPrec) break;
        std::string Op = CurrentToken()->Value;
        AdvanceToken();
        auto RHS = ParseUnaryOrPostfixPredicate();
        if (!RHS) {
            std::cerr << "Error: Expected expression after operator \"" << Op << "\"\n";
            return nullptr;
        }
        while(CurrentToken()) {
            int NextPrec = GetTokenPrecedence(*CurrentToken());
            if(NextPrec > CurrentPrec)
                RHS = ParseBinaryOperation(CurrentPrec + 1, std::move(RHS));
            else break;
        }
        // Create a new BinaryOpAST with the correct types
        auto BinaryOp = std::make_unique<BinaryOpAST>(
            std::unique_ptr<ExpressionAST>(static_cast<ExpressionAST*>(LHS.release())),
            Op,
            std::unique_ptr<ExpressionAST>(static_cast<ExpressionAST*>(RHS.release()))
        );
        LHS = std::move(BinaryOp);
    }
    return LHS;
}

ASTNode Parser::ParseExpression() {
    return ParseBinaryOperation();
}

void Parser::ApplyCteSubstitution(std::string &TableName) const {
	auto It = CteSubstitutions_.find(TableName);
	if(It != CteSubstitutions_.end())
		TableName = It->second;
}

ASTNode Parser::ParseExistsPredicate(bool Negated) {
	if(!CurrentToken() || CurrentToken()->Value != "(")
		ParseFail("Expected '(' after EXISTS");
	AdvanceToken();
	/* SELECT is tokenized as TokenType::SELECT, not KEYWORD; MatchKeyword("SELECT") would fail. */
	if(MatchToken(TokenType::SELECT)) {
		while(CurrentToken()) {
			if(CurrentToken()->Value == "FROM")
				break;
			AdvanceToken();
		}
	}
	if(!MatchKeyword("FROM"))
		ParseFail("EXISTS subquery requires SELECT ... FROM table (subset)");
	auto TTok = CurrentToken();
	if(!TTok)
		ParseFail("EXISTS expects table name after FROM");
	if(TTok->Type != TokenType::IDENTIFIER && TTok->Type != TokenType::KEYWORD)
		ParseFail("EXISTS FROM expects a simple table identifier");
	std::string Tbl = TTok->Value;
	AdvanceToken();
	ApplyCteSubstitution(Tbl);
	std::unique_ptr<ExpressionAST> Where = nullptr;
	if(CurrentToken() && CurrentToken()->Value == "WHERE") {
		AdvanceToken();
		auto W = ParseBinaryOperation();
		Where =
		    W ? std::unique_ptr<ExpressionAST>(static_cast<ExpressionAST *>(W.release())) : nullptr;
	}
	if(!CurrentToken() || CurrentToken()->Value != ")")
		ParseFail("Expected ')' closing EXISTS subquery");
	AdvanceToken();
	return std::make_unique<ExistsPredAST>(Negated, std::move(Tbl), std::move(Where));
}

std::unique_ptr<StatementAST> Parser::ParseWithStatement() {
	AdvanceToken();
	const bool Recursive = MatchKeyword("RECURSIVE");
	const auto Saved = CteSubstitutions_;
	std::vector<CteClause> Clauses;

	while(true) {
		auto Ahead = CurrentToken();
		if(!Ahead)
			ParseFail("Incomplete WITH clause");
		if(Ahead->Type == TokenType::SELECT) {
			if(Clauses.empty())
				ParseFail("WITH requires at least one named CTE before the main SELECT");
			break;
		}

		auto AliasTk = Ahead;
		if(AliasTk->Type != TokenType::IDENTIFIER && AliasTk->Type != TokenType::KEYWORD)
			ParseFail("WITH expects CTE name");
		const std::string Alias = AliasTk->Value;
		AdvanceToken();
		if(CurrentToken() && CurrentToken()->Value == "(") {
			AdvanceToken();
			while(true) {
				auto ColTok = CurrentToken();
				if(!ColTok || (ColTok->Type != TokenType::IDENTIFIER && ColTok->Type != TokenType::KEYWORD))
					ParseFail("WITH CTE column list expects identifiers");
				AdvanceToken();
				if(CurrentToken() && CurrentToken()->Value == ",") {
					AdvanceToken();
					continue;
				}
				if(CurrentToken() && CurrentToken()->Value == ")") {
					AdvanceToken();
					break;
				}
				ParseFail("WITH CTE column list must end with ')'");
			}
		}
		if(!MatchKeyword("AS"))
			ParseFail("WITH expects AS between CTE name and definition");
		if(!CurrentToken() || CurrentToken()->Value != "(")
			ParseFail("WITH expects '(' before CTE SELECT");
		AdvanceToken();

		const std::string Physical =
		    "__astral_cte_" + std::to_string(Clauses.size()) + "_" + Alias;
		CteSubstitutions_[Alias] = Physical;

		if(!CurrentToken() || CurrentToken()->Type != TokenType::SELECT)
			ParseFail("WITH CTE must be SELECT");
		ASTNode Inner = ParseSelectStatement();

		if(!CurrentToken() || CurrentToken()->Value != ")")
			ParseFail("WITH CTE subquery must end with ')'");
		AdvanceToken();

		CteClause ClauseInst;
		ClauseInst.Alias = Alias;
		ClauseInst.PhysicalTable = Physical;

		auto *Compound = dynamic_cast<CompoundSelectAST *>(Inner.get());
		const bool UnionRecursiveForm = Compound && Compound->Arms.size() == 2 && Compound->Ops.size() == 1 &&
		                                Compound->Ops[0] == CompoundSetOpKind::UnionAll;

		if(Recursive || UnionRecursiveForm) {
			if(!UnionRecursiveForm)
				ParseFail("RECURSIVE requires anchor SELECT UNION ALL recursive SELECT");
			if(!Compound->OrderByColumns.empty() || Compound->Limit >= 0 || Compound->Offset > 0)
				ParseFail("ORDER BY / LIMIT / OFFSET are not allowed inside a recursive CTE body");
			ClauseInst.Anchor = std::move(Compound->Arms[0]);
			ClauseInst.RecursiveStep = std::move(Compound->Arms[1]);
			Inner.reset();
		} else {
			auto *SelDyn = dynamic_cast<SelectAST *>(Inner.get());
			if(!SelDyn)
				ParseFail("WITH CTE definition must be a single SELECT (not UNION / INTERSECT / EXCEPT)");
			ClauseInst.Anchor = std::unique_ptr<SelectAST>(static_cast<SelectAST *>(Inner.release()));
		}
		Clauses.push_back(std::move(ClauseInst));

		if(CurrentToken() && CurrentToken()->Value == ",")
			AdvanceToken();
		else
			break;
	}

	if(!CurrentToken() || CurrentToken()->Type != TokenType::SELECT)
		ParseFail("WITH must be followed by SELECT");
	ASTNode Tail = ParseSelectStatement();
	if(!Tail)
		ParseFail("WITH missing main SELECT");

	CteSubstitutions_ = Saved;

	auto Out = std::make_unique<WithSelectAST>();
	Out->Clauses = std::move(Clauses);
	Out->Main = std::move(Tail);
	return Out;
}

std::unique_ptr<StatementAST> Parser::ParseStatement() {
    if (auto Token = CurrentToken()) {
        if(Token->Type == TokenType::KEYWORD && Token->Value == "WITH")
            return ParseWithStatement();
        if(Token->Type == TokenType::KEYWORD && Token->Value == "EXPORT")
            return ParseDataExchangeStatement();
        if(Token->Type == TokenType::KEYWORD && Token->Value == "IMPORT")
            return ParseDataExchangeStatement();
        if(Token->Type == TokenType::KEYWORD && Token->Value == "CONVERT")
            return ParseDataExchangeStatement();
        if(Token->Type == TokenType::KEYWORD && Token->Value == "SAVEPOINT")
            return ParseSavepointSetStatement();
        if(Token->Type == TokenType::KEYWORD && Token->Value == "RELEASE")
            return ParseReleaseSavepointStatement();
        if(Token->Type == TokenType::KEYWORD && Token->Value == "DROP")
            return ParseDropStatement();
        if(Token->Type == TokenType::KEYWORD && Token->Value == "CALL") {
            AdvanceToken();
            return ParseCallProcedureStatement();
        }
        if(Token->Type == TokenType::KEYWORD && Token->Value == "EXECUTE") {
            AdvanceToken();
            return ParseCallProcedureStatement();
        }
        if(Token->Type == TokenType::KEYWORD && Token->Value == "ALTER")
            return ParseAlterStatement();
        if(Token->Type == TokenType::KEYWORD) {
            if(Token->Value == "MERGE") return ParseMergeStatement();
            if(Token->Value == "UPDATE") return ParseUpdateStatement();
            if(Token->Value == "DELETE") return ParseDeleteStatement();
            if(Token->Value == "GRANT") return ParseGrantStatement();
            if(Token->Value == "REVOKE") return ParseRevokeStatement();
        }
        switch (Token->Type) {
            case TokenType::SELECT:
                return ParseSelectStatement();
            case TokenType::CREATE:
                return ParseCreateStatement();
            case TokenType::INSERT:
                return ParseInsertStatement();
            case TokenType::BEGIN:
                AdvanceToken();
                return std::make_unique<TransactionAST>(TransactionType::BEGIN);
            case TokenType::COMMIT:
                AdvanceToken();
                return std::make_unique<TransactionAST>(TransactionType::COMMIT);
            case TokenType::ROLLBACK:
                return ParseRollbackStatement();
            default:
                if(ParserDiagnosticsEnabled())
                    std::cerr << "[ParseStatement] Unknown statement type: " << Token->Value << std::endl;
                AdvanceToken();
                return nullptr;
        }
    }
    return nullptr;
}

ASTNode Parser::ParseRollbackStatement() {
	AdvanceToken();
	if(MatchKeyword("TO")) {
		if(!MatchKeyword("SAVEPOINT"))
			ParseFail("ROLLBACK TO expects SAVEPOINT");
		auto N = CurrentToken();
		if(!N)
			ParseFail("ROLLBACK TO SAVEPOINT requires savepoint name");
		auto Sp = std::make_unique<SavepointAST>();
		Sp->Kind = SavepointStmtKind::RollbackTo;
		Sp->Name = N->Value;
		AdvanceToken();
		return Sp;
	}
	return std::make_unique<TransactionAST>(TransactionType::ROLLBACK);
}

ASTNode Parser::ParseSavepointSetStatement() {
	AdvanceToken();
	auto N = CurrentToken();
	if(!N)
		ParseFail("SAVEPOINT requires a name");
	auto Sp = std::make_unique<SavepointAST>();
	Sp->Kind = SavepointStmtKind::Set;
	Sp->Name = N->Value;
	AdvanceToken();
	return Sp;
}

ASTNode Parser::ParseReleaseSavepointStatement() {
	AdvanceToken();
	if(!MatchKeyword("SAVEPOINT"))
		ParseFail("RELEASE expects SAVEPOINT");
	auto N = CurrentToken();
	if(!N)
		ParseFail("RELEASE SAVEPOINT requires a name");
	auto Sp = std::make_unique<SavepointAST>();
	Sp->Kind = SavepointStmtKind::Release;
	Sp->Name = N->Value;
	AdvanceToken();
	return Sp;
}

std::unique_ptr<StatementAST> Parser::ParseDataExchangeStatement() {
	auto ExpectPathLiteral = [this]() -> std::string {
		auto Tk = CurrentToken();
		if(!Tk || Tk->Type != TokenType::LITERAL)
			ParseFail("Expected quoted path string literal");
		std::string P = Tk->Value;
		AdvanceToken();
		return P;
	};
	auto ExpectFormatKeyword = [this]() -> std::string {
		auto Tk = CurrentToken();
		if(!Tk || (Tk->Type != TokenType::KEYWORD && Tk->Type != TokenType::IDENTIFIER))
			ParseFail("Expected FORMAT name (CSV, JSON, TSV)");
		std::string F = Tk->Value;
		AdvanceToken();
		for(char &Ch : F)
			Ch = static_cast<char>(std::toupper(static_cast<unsigned char>(Ch)));
		if(F != "JSON" && F != "CSV" && F != "TSV")
			ParseFail("FORMAT must be CSV, JSON, or TSV");
		return F;
	};
	auto Head = CurrentToken();
	if(!Head || Head->Type != TokenType::KEYWORD)
		ParseFail("Data exchange expects EXPORT, IMPORT, or CONVERT keyword");
	const std::string Verb = Head->Value;
	AdvanceToken();
	auto Node = std::make_unique<DataExchangeAST>();
	if(Verb == "EXPORT") {
		if(!MatchKeyword("DATABASE"))
			ParseFail("EXPORT DATABASE syntax");
		if(!MatchKeyword("TO"))
			ParseFail("EXPORT DATABASE TO syntax");
		Node->Kind = DataExchangeKind::ExportDatabase;
		Node->Path = ExpectPathLiteral();
		if(!MatchKeyword("FORMAT"))
			ParseFail("EXPORT DATABASE requires FORMAT clause");
		Node->Format = ExpectFormatKeyword();
		return Node;
	}
	if(Verb == "IMPORT") {
		if(!MatchKeyword("DATABASE"))
			ParseFail("IMPORT DATABASE syntax");
		if(!MatchKeyword("FROM"))
			ParseFail("IMPORT DATABASE FROM syntax");
		Node->Kind = DataExchangeKind::ImportDatabase;
		Node->Path = ExpectPathLiteral();
		if(!MatchKeyword("FORMAT"))
			ParseFail("IMPORT DATABASE requires FORMAT clause");
		Node->Format = ExpectFormatKeyword();
		return Node;
	}
	if(Verb == "CONVERT") {
		if(!MatchKeyword("FILE"))
			ParseFail("CONVERT FILE syntax");
		Node->Kind = DataExchangeKind::ConvertFiles;
		Node->Path = ExpectPathLiteral();
		if(!MatchKeyword("TO"))
			ParseFail("CONVERT FILE expects TO dst path");
		Node->DestPath = ExpectPathLiteral();
		if(!MatchKeyword("FROM"))
			ParseFail("CONVERT expects FROM srcFormat TO destFormat");
		Node->Format = ExpectFormatKeyword();
		if(!MatchKeyword("TO"))
			ParseFail("CONVERT expects TO destFormat");
		Node->DestFormat = ExpectFormatKeyword();
		return Node;
	}
	ParseFail("Malformed data exchange statement");
}

ASTNode Parser::ParseAlterStatement() {
	AdvanceToken();
	if(!MatchKeyword("TABLE"))
		ParseFail("ALTER expects TABLE");
	auto TT = CurrentToken();
	if(!TT)
		ParseFail("ALTER TABLE expects table identifier");
	const std::string Tbl = TT->Value;
	AdvanceToken();
	if(MatchKeyword("ADD")) {
		if(!MatchKeyword("COLUMN"))
			ParseFail("ALTER TABLE ADD COLUMN syntax");
		auto CN = CurrentToken();
		if(!CN)
			ParseFail("ALTER TABLE ADD COLUMN column name");
		const std::string Cname = CN->Value;
		AdvanceToken();
		const std::string Dty = ParseDataType();
		std::vector<std::string> Cstr = ParseColumnConstraintList();
		auto A = std::make_unique<AlterTableAST>();
		A->Kind = AlterTableKind::AddColumn;
		A->TableName = Tbl;
		A->AddedColumn = ColumnDefinition(Cname, Dty, std::move(Cstr));
		return A;
	}
	if(MatchKeyword("DROP")) {
		if(!MatchKeyword("COLUMN"))
			ParseFail("ALTER TABLE DROP COLUMN syntax");
		auto CN = CurrentToken();
		if(!CN)
			ParseFail("ALTER TABLE DROP COLUMN column name");
		auto A = std::make_unique<AlterTableAST>();
		A->Kind = AlterTableKind::DropColumn;
		A->TableName = Tbl;
		A->DropColumnName = CN->Value;
		AdvanceToken();
		return A;
	}
	if(MatchKeyword("SET")) {
		if(!MatchKeyword("STORAGE"))
			ParseFail("ALTER TABLE SET STORAGE syntax");
		auto St = CurrentToken();
		if(!St)
			ParseFail("ALTER TABLE SET STORAGE expects ROW, COLUMNAR, HYBRID, or AUTO");
		auto A = std::make_unique<AlterTableAST>();
		A->Kind = AlterTableKind::SetStorage;
		A->TableName = Tbl;
		A->StoragePolicy = StorageLayoutFromKeyword(St->Value);
		AdvanceToken();
		return A;
	}
	if(MatchKeyword("RENAME")) {
		if(!MatchKeyword("COLUMN"))
			ParseFail("ALTER TABLE RENAME COLUMN syntax");
		auto Fr = CurrentToken();
		if(!Fr)
			ParseFail("ALTER rename source column");
		const std::string From = Fr->Value;
		AdvanceToken();
		if(!MatchKeyword("TO"))
			ParseFail("ALTER RENAME COLUMN expects TO");
		auto To = CurrentToken();
		if(!To)
			ParseFail("ALTER rename target column");
		auto A = std::make_unique<AlterTableAST>();
		A->Kind = AlterTableKind::RenameColumn;
		A->TableName = Tbl;
		A->RenameFrom = From;
		A->RenameTo = To->Value;
		AdvanceToken();
		return A;
	}
	ParseFail("Unsupported ALTER TABLE variation");
}

}
}
