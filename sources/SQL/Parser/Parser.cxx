#include <Database/Types/AdvancedTypes.hxx>
#include <Database/MathSci/MathSci.hxx>
#include <SQL/SQL.hxx>
#include <SQL/Parser/DialectCompat.hxx>
#include <SQL/Procedures/ProcedureParser.hxx>
#include <SQL/Procedures/BytecodeProcedures.hxx>
#include <SQL/Procedures/BytecodeTriggers.hxx>
#include <Database/Storage/HybridStorageScheduler.hxx>
#include <Database/Security/User.hxx>
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

	bool IsSqlSimpleNameToken(const std::optional<Token> &Tok) {
		return Tok.has_value() &&
		       (Tok->Type == TokenType::IDENTIFIER || Tok->Type == TokenType::KEYWORD);
	}

	bool StmtEndIsWord(std::string_view Query, std::size_t Pos, std::string_view Word) {
		if(Pos + Word.size() > Query.size())
			return false;
		for(std::size_t I = 0; I < Word.size(); ++I) {
			const char A = Query[Pos + I];
			const char B = Word[I];
			if(std::toupper(static_cast<unsigned char>(A)) != std::toupper(static_cast<unsigned char>(B)))
				return false;
		}
		const auto IsIdent = [](char C) { return std::isalnum(static_cast<unsigned char>(C)) || C == '_'; };
		if(Pos > 0 && IsIdent(Query[Pos - 1]))
			return false;
		if(Pos + Word.size() < Query.size() && IsIdent(Query[Pos + Word.size()]))
			return false;
		return true;
	}

	/** Byte offset of the statement-ending \c ; at top level (or \c Query.size() if none). */
	std::size_t StatementEndBeforeSemicolon(std::string_view Query, std::size_t BeginByte) {
		std::size_t P = BeginByte;
		int ParenDepth = 0;
		int BeginDepth = 0;
		while(P < Query.size()) {
			const unsigned char C = static_cast<unsigned char>(Query[P]);
			if(C == '\'' || C == '"') {
				const char Q = static_cast<char>(C);
				++P;
				while(P < Query.size()) {
					if(Query[P] == '\\' && P + 1 < Query.size())
						P += 2;
					else if(Query[P] == Q) {
						++P;
						break;
					} else
						++P;
				}
				continue;
			}
			if(C == '$') {
				const std::size_t TagOpen = P;
				++P;
				while(P < Query.size() && Query[P] != '$')
					++P;
				if(P >= Query.size())
					break;
				const std::string_view Tag = Query.substr(TagOpen + 1, P - TagOpen - 1);
				++P;
				const std::string Close = std::string("$") + std::string(Tag) + "$";
				const std::size_t CloseAt = Query.find(Close, P);
				if(CloseAt == std::string::npos)
					break;
				P = CloseAt + Close.size();
				continue;
			}
			if(C == '(') {
				++ParenDepth;
				++P;
				continue;
			}
			if(C == ')') {
				if(ParenDepth > 0)
					--ParenDepth;
				++P;
				continue;
			}
			if(ParenDepth == 0 && std::isalpha(static_cast<unsigned char>(C))) {
				if(StmtEndIsWord(Query, P, "BEGIN")) {
					++BeginDepth;
					P += 5;
					continue;
				}
				if(StmtEndIsWord(Query, P, "END") && BeginDepth > 0) {
					std::size_t J = P + 3;
					while(J < Query.size() && std::isspace(static_cast<unsigned char>(Query[J])))
						++J;
					if(J + 1 < Query.size() && StmtEndIsWord(Query, J, "IF")) {
						P = J + 2;
						continue;
					}
					if(J + 3 < Query.size() && StmtEndIsWord(Query, J, "LOOP")) {
						P = J + 4;
						continue;
					}
					if(J + 3 < Query.size() && StmtEndIsWord(Query, J, "CASE")) {
						P = J + 4;
						continue;
					}
					if(J + 3 < Query.size() && StmtEndIsWord(Query, J, "TRY")) {
						P = J + 3;
						continue;
					}
					if(J + 4 < Query.size() && StmtEndIsWord(Query, J, "CATCH")) {
						P = J + 5;
						continue;
					}
					--BeginDepth;
					P = J;
					continue;
				}
			}
			if(C == ';' && ParenDepth == 0 && BeginDepth == 0)
				return P;
			++P;
		}
		return Query.size();
	}

	bool IsRowNumColumnRef(const ExpressionAST *E) {
		const auto *C = dynamic_cast<const ColumnRefAST *>(E);
		if(!C)
			return false;
		std::string U = C->Name;
		for(char &Ch : U)
			Ch = static_cast<char>(std::toupper(static_cast<unsigned char>(Ch)));
		return U == "ROWNUM";
	}

	std::optional<int64_t> TryRowNumCapFromComparison(const BinaryOpAST *B) {
		if(!B)
			return std::nullopt;
		const auto *L = dynamic_cast<const ColumnRefAST *>(B->LHS.get());
		const auto *R = dynamic_cast<const LiteralAST *>(B->RHS.get());
		const auto *RL = dynamic_cast<const LiteralAST *>(B->LHS.get());
		const auto *RR = dynamic_cast<const ColumnRefAST *>(B->RHS.get());
		const ColumnRefAST *RowCol = nullptr;
		const LiteralAST *Lit = nullptr;
		if(L && IsRowNumColumnRef(B->LHS.get())) {
			RowCol = L;
			Lit = R;
		} else if(RR && IsRowNumColumnRef(B->RHS.get())) {
			RowCol = RR;
			Lit = RL;
		} else
			return std::nullopt;
		(void)RowCol;
		if(!Lit)
			return std::nullopt;
		int64_t N = 0;
		try {
			N = std::stoll(Lit->Value);
		} catch(...) {
			return std::nullopt;
		}
		if(B->Op == "<=")
			return N;
		if(B->Op == "<")
			return N > 0 ? N - 1 : 0;
		if(B->Op == "=" || B->Op == "==")
			return 1;
		return std::nullopt;
	}

	struct RowNumWhereSplit {
		std::unique_ptr<ExpressionAST> Remaining;
		int64_t Cap = -1;
	};

	RowNumWhereSplit ExtractRowNumWhere(std::unique_ptr<ExpressionAST> Root) {
		if(!Root)
			return {};
		if(auto *B = dynamic_cast<BinaryOpAST *>(Root.get())) {
			if(B->Op == "AND") {
				auto Left = std::move(B->LHS);
				auto Right = std::move(B->RHS);
				Root.reset();
				RowNumWhereSplit L = ExtractRowNumWhere(std::move(Left));
				RowNumWhereSplit R = ExtractRowNumWhere(std::move(Right));
				int64_t Cap = -1;
				if(L.Cap >= 0)
					Cap = L.Cap;
				if(R.Cap >= 0)
					Cap = Cap < 0 ? R.Cap : std::min(Cap, R.Cap);
				std::unique_ptr<ExpressionAST> Rem;
				if(L.Remaining && R.Remaining)
					Rem = std::make_unique<BinaryOpAST>(std::move(L.Remaining), "AND", std::move(R.Remaining));
				else if(L.Remaining)
					Rem = std::move(L.Remaining);
				else if(R.Remaining)
					Rem = std::move(R.Remaining);
				return {std::move(Rem), Cap};
			}
			if(auto Cap = TryRowNumCapFromComparison(B))
				return {nullptr, *Cap};
		}
		return {std::move(Root), -1};
	}
bool IsSelectAliasToken(const std::optional<Token> &Tok) noexcept {
	return Tok && (Tok->Type == TokenType::IDENTIFIER || Tok->Type == TokenType::KEYWORD);
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
        {"=", 3}, {"!=", 3}, {"==", 3}, {"IN", 3}, {"LIKE", 3}, {"ILIKE", 3}, {"GLOB", 3}, {"REGEXP", 3}, {"~", 3}, {"~*", 3}, {"!~", 3}, {"!~*", 3},
        {"<", 4}, {"<=", 4}, {">", 4}, {">=", 4},
        {"+", 5}, {"-", 5}, {"||", 5},
        {"*", 6}, {"/", 6}, {"//", 6}, {"%", 6}
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
        "CREATE", "TABLE", "TABLES", "TYPE", "SEQUENCE", "DROP", "ALTER", "ADD", "COLUMN", "MODIFY", "RENAME",
        "USER", "IDENTIFIED",
        "PRIMARY", "KEY", "FOREIGN", "REFERENCES", "UNIQUE", "CASCADE", "RESTRICT",
        "NOT", "NULL", "DEFAULT", "AUTO_INCREMENT", "CONSTRAINT", "CHECK",
        "BOOLEAN", "BOOL", "INT", "INTEGER", "BIGINT", "SMALLINT", "TEXT", "REAL", "DOUBLE", "FLOAT",
        "DECIMAL", "NUMERIC", "CHAR", "VARCHAR", "CHARACTER",
        "DATE", "TIME", "TIMESTAMP", "DATETIME",
        "TRUE", "FALSE",         "SAVEPOINT", "RELEASE", "SAVE",
        "VIEW", "PROCEDURE", "CALL", "EXEC", "EXECUTE",
        "TRIGGER", "BEFORE", "AFTER", "INSTEAD", "EACH", "ENABLE", "DISABLE",
        "DISTINCT",
        "AND", "OR", "LIKE", "ILIKE", "GLOB", "REGEXP", "IN", "BETWEEN", "EXISTS", "SOME",
        "IFNULL", "NVL", "NVL2", "DECODE", "SERIAL", "BIGSERIAL", "DUAL", "AUTOINCREMENT", "REPLACE", "COLUMNS",
        "LIST_TRANSFORM",
        "ROWNUM", "CONNECT", "START", "PRIOR", "NOCYCLE", "LEVEL",
        "ASC", "DESC", "LIMIT", "OFFSET", "FETCH", "FIRST", "ROWS", "RANGE", "ONLY",
        "BULK", "START", "STEP",
        "BEGIN", "COMMIT", "ROLLBACK", "TO", "TRANSACTION", "TRANSACTIONS", "ISOLATION", "LEVEL", "SERIALIZABLE", "READ", "COMMITTED", "REPEATABLE",
        "GRANT", "REVOKE", "FROM", "ROLE", "TO", "OF",
        "IF", "EXISTS",
        "IS", "CURRENT_TIMESTAMP", "CURRENT_DATE", "CURRENT_TIME", "NOW",
        "EXPORT", "IMPORT", "CONVERT", "DATABASE", "FORMAT", "FILE", "LOAD", "DATASET", "EMBEDDING", "INTO",
        "WITH", "RECURSIVE", "UNION", "ALL", "INTERSECT", "EXCEPT", "PRAGMA", "EXPLAIN", "ANALYZE",
        "MERGE", "USING", "MATCHED", "CONFLICT", "DO", "NOTHING", "EXCLUDED",
		"RETURNING",
        "ROLLUP", "CUBE", "GROUPING", "SETS", "GROUPING_ID",
        "PARTITION", "ROW_NUMBER", "RANK", "DENSE_RANK", "OVER", "LAG", "LEAD", "COUNT",
        "FIRST_VALUE", "LAST_VALUE", "NTH_VALUE", "PERCENT_RANK", "CUME_DIST", "NTILE",
        "CURRENT", "ROW", "UNBOUNDED", "PRECEDING", "FOLLOWING",
        "INNER", "LEFT", "RIGHT", "FULL", "OUTER", "CROSS", "JOIN", "ON", "LATERAL",
        "SUM", "MIN", "MAX", "AVG",
        "CASE", "WHEN", "THEN", "ELSE", "END", "CAST", "COALESCE",
        "SUBSTRING", "POSITION", "LENGTH", "CHAR_LENGTH", "CHARACTER_LENGTH", "TRIM", "CONCAT", "EXTRACT",
        "DATE_ADD", "DATE_SUB", "DATE_DIFF", "DATE_TRUNC", "TIME_BUCKET", "TIMESTAMP_DIFF",
        "FOR", "BOTH", "LEADING", "TRAILING",
        "YEAR", "MONTH", "DAY", "HOUR", "MINUTE", "SECOND", "EPOCH",
        "GENERATED", "IDENTITY", "ALWAYS", "NEXTVAL", "INCREMENT",
        "STORAGE", "COLUMNAR", "HYBRID", "AUTO",
        "STRUCT", "MAP", "VECTOR", "MATRIX", "MDARRAY", "COMPLEX", "LIST", "ARRAY", "POINT", "GEOMETRY",
        "ABS", "SQRT", "CBRT", "POW", "EXP", "LN", "LOG10", "LOG2", "SIN", "COS", "TAN", "ASIN", "ACOS", "ATAN",
        "ATAN2", "SINH", "COSH", "TANH", "FLOOR", "CEIL", "ROUND", "TRUNC", "SIGN", "MOD", "HYPOT", "DEGREES",
        "RADIANS", "LERP", "CLAMP", "MEAN", "VAR_POP", "VAR_SAMP", "STDDEV_POP", "STDDEV_SAMP", "MEDIAN", "ENTROPY",
        "NORM_L1", "NORM_L2", "LIST_SUM", "CORR", "COVAR_POP", "COVAR_SAMP", "SIGMOID", "RELU", "SOFTMAX",
        "MINMAX_SCALE", "ZSCORE", "LIST_LEN", "LIST_GET", "LIST_APPEND", "LIST_CONCAT", "LIST_CONTAINS", "LIST_SLICE",
        "LOGISTIC", "LOGIT", "SOFTPLUS", "LEAKY_RELU", "MSE_LOSS", "MAE_LOSS", "RMSE_LOSS", "BCE_LOSS", "HINGE_LOSS",
        "HUBER_LOSS", "CE_LOSS", "RANDOM", "RANDOM_NORMAL", "RANDOM_INT", "SETSEED", "COSINE_SIM", "EUCLIDEAN_DIST",
        "MANHATTAN_DIST", "MATVEC", "LIST_SORT", "LIST_SORT_DESC", "LIST_REVERSE",
        "JSON_EXTRACT", "JSON_CONTAINS", "JSON_MERGE", "JSON_ARRAY_LENGTH", "JSON_KEYS",
        "JSON_VALUE", "JSON_QUERY", "JSON_EXISTS",
        "XML_EXTRACT", "XML_SERIALIZE", "XML_VALID", "XMLQUERY", "XMLEXISTS",
        "REGEXP_EXTRACT", "REGEXP_SUBSTR",
        "TEXT_RANK", "VECTOR_TOPK", "NULLIF", "GREATEST",
        "LEAST", "FFT", "IFFT", "DCT", "IDCT", "CONV_FULL", "CONV1D", "CONV_SAME", "CONV1D_SAME", "LAPLACIAN",
        "LAPLACIAN1D", "AD_GRAD_ADD",
        "AD_GRAD_MUL_LHS", "AD_GRAD_MUL_RHS", "AD_GRAD_RELU", "AD_GRAD_SIGMOID", "AD_GRAD_TANH",
        "AD_GRAD_MATVEC_IN", "AD_GRAD_MATVEC_W", "AD_GRAD_MSE_PRED", "AD_GRAD_CONV1D_IN",
        "AD_GRAD_CONV1D_K", "AD_CHAIN", "AD_HESSIAN", "AD_HESSIAN_RELU", "AD_HESSIAN_SIGMOID", "AD_HESSIAN_SQUARE",
        "AD_HESSIAN_TANH", "PINN_FD_CENTRAL",
        "AD_WIRTINGER_MUL_LHS", "AD_WIRTINGER_MUL_RHS", "AD_WIRTINGER_ABS2", "AD_WIRTINGER_CHAIN", "AD_WIRTINGER_DZ",
        "AD_WIRTINGER_DZBAR", "AD_GRAPH_BUILD", "AD_GRAPH_FUSE", "AD_GRAPH_FORWARD", "AD_GRAPH_CACHE",
        "AD_GRAPH_BACKWARD", "AD_GRAPH_NODE_COUNT",
        "ML_QUANTIZE_INT8", "ML_DEQUANTIZE_INT8", "ML_QUANTIZE_MODEL", "ML_DEQUANTIZE_MODEL",
        "ML_MIXED_PREC", "ML_PRUNE", "ML_PRUNE_MODEL", "ML_POSENC", "ML_POSENC_SEQUENCE", "ML_POSENC_ADD",
        "ML_MIXED_PREC_MODEL", "ML_PREDICT_QUANT", "ML_DTYPE", "ML_CAST", "ML_POSENC_C", "ODE_EULER", "ODE_RK4", "ODE_HEUN", "ODE_MIDPOINT", "ODE_IMPLICIT_EULER",
        "SOLVE_ODE", "SDE_EULER", "SDE_GBM", "SDE_OU", "SDE_MILSTEIN", "PDE_HEAT_STEP", "PDE_POISSON_STEP",
        "PDE_ADVECTION_STEP", "PDE_WAVE_STEP", "ODE_TRAPEZOID", "ODE_SEMI_IMPLICIT", "ODE_CRANK_NICOLSON",
        "ODE_RK3", "ODE_ADAMS_BASHFORTH2", "ODE_MARCH", "SDE_MARCH",
        "LINEAR_JACOBI_STEP", "LINEAR_GS_STEP", "LINEAR_SOR_STEP", "LINEAR_RICHARDSON_STEP", "LINEAR_CG_SOLVE",
        "SOLVE_LINEAR", "PDE_POISSON_GS_STEP", "PDE_POISSON_SOR_STEP", "PDE_POISSON_SOLVE", "PDE_HEAT_MARCH",
        "SOLVE_PDE", "ROOT_NEWTON_STEP", "ROOT_SECANT_STEP", "ROOT_BISECT_STEP", "ROOT_HALLEY_STEP", "SOLVE_ROOT",
        "CLASSIFY_LINEAR", "CLASSIFY_LOGISTIC", "CLASSIFY_ARGMAX", "CLASSIFY_ONE_VS_REST",
        "NLP_TOKENIZE", "NLP_NGRAMS", "NLP_JACCARD", "NLP_EDIT_DIST", "NLP_STEM",
        "NLP_EMBED_BUILD", "NLP_EMBED_LOOKUP", "NLP_EMBED_BATCH", "NLP_EMBED_SERIALIZE", "NLP_EMBED_LOAD",
        "NLP_EMBED_FINGERPRINT", "NLP_EMBED_MEAN",
        "MATHSCI_MODEL_BUILD", "MATHSCI_MODEL_SERIALIZE", "MATHSCI_MODEL_IMPORT", "MATHSCI_MODEL_LOAD",
        "MATHSCI_MODEL_FINGERPRINT", "PREDICT", "OPTIMIZER_STEP",
        "HEAT_KERNEL_GRAPH_STEP", "HEAT_KERNEL_GRAPH_MARCH", "HEAT_KERNEL_GRAPH_APPLY",
        "HEAT_KERNEL_1D", "HEAT_KERNEL_2D", "HEAT_KERNEL_GRAD_1D",
        "IMG_LOAD", "IMG_GRAY", "IMG_RESIZE", "IMG_PATCHES", "IMG_HOG_LITE", "IMG_FLATTEN",
        "DEQ_INTEGRATE", "DEQ_ADAPT", "DEQ_LINSPACE",
        "MCTS_SEARCH", "MCTS_UCT_PICK", "BAYES_BETA_POST", "BAYES_NORMAL_POST", "BAYES_GRID_POST", "BAYES_LOG_EVIDENCE",
        "NFP_MACRO_STEP", "NFP_MACRO_MARCH", "NFP_MACRO_MOMENTS",
        "ST_POINT", "ST_X", "ST_Y", "ST_AS_TEXT", "ST_DISTANCE",
        "ST_DISTANCE_SPHERICAL", "ST_WITHIN_BBOX", "ST_POINTZ", "ST_ELEVATION", "ST_DEM_SAMPLE", "ST_TERRAIN_SLOPE",
        "ST_MESH", "ST_MESH_IMPORT_GLTF", "ST_MESH_EXPORT_GLTF", "ST_MESH_SEW", "ST_MESH_UNION",
        "ST_MESH_INTERSECTION", "ST_MESH_DIFFERENCE", "ST_MESH_VOLUME", "ST_MESH_SURFACE_AREA",
        "ST_MESH_CENTROID", "ST_MESH_TRANSLATE", "ST_MESH_SCALE", "ST_MESH_ROTATE", "ST_MESH_BOUNDS",
        "ST_MESH_MERGE",
        "ST_POLYGON", "ST_POLYGON_WKT", "ST_GEOM_AS_TEXT", "ST_GEOM_AREA", "ST_GEOM_PERIMETER",
        "ST_GEOM_CENTROID", "ST_GEOM_CONTAINS", "ST_GEOM_WITHIN", "ST_GEOM_INTERSECTS", "ST_GEOM_OVERLAPS",
        "ST_GEOM_TOUCHES", "ST_GEOM_UNION", "ST_GEOM_INTERSECTION", "ST_GEOM_DIFFERENCE",
        "ST_GEOM_SYMDIFFERENCE", "ST_GEOM_BUFFER", "ST_GEOM_SIMPLIFY", "ST_GEOM_CONVEX_HULL",
        "ST_GEOJSON_IMPORT", "ST_GEOJSON_EXPORT", "ST_GEOM_VALIDATE", "ST_GEOM_REPAIR",
        "ST_MESH_VALIDATE", "ST_MESH_REPAIR",
        "TS_COMPRESS", "TS_DECOMPRESS", "TS_COMPRESS_SERIES",
        "DATASET", "EMBEDDING", "LOAD", "INTO", "VERSION", "VARIANT", "TERRAIN", "TERRAIN_POINT", "MESH",
        "POLYGON", "UUID", "INTERVAL", "MULTISET",
        "VACUUM", "REPACK", "CONCURRENTLY",
        "GRAPH", "VERTEX", "EDGE", "TRAVERSE", "DEPTH", "BFS", "DFS", "PROJECTION", "SHORTEST", "PATH",
        "PAGERANK", "DAMPING", "ITERATIONS", "WEIGHTED", "WEIGHT", "UNDIRECTED", "TO",
        "MATCH_RECOGNIZE", "MATCH", "AGAINST", "TEXT_CONTAINS", "REGEXP_MATCH", "MATCH_AGAINST", "PATTERN", "DEFINE",
        "SYSTEM", "TIME", "INDEX", "FTS", "VECTOR", "METRIC", "SHOW", "DESCRIBE", "COMMENT", "INFORMATION_SCHEMA"};
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
                if((Tokens.size() & 0x1FF) == 0 && Tokens.size() > Limits::MaxSqlTokens)
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
            if(Position + 2 < Query_.size()) {
                std::string ThreeChars = std::string(Query_.substr(Position, 3));
                if(ThreeChars == "!~*") {
                    const size_t OpBegin = Position;
                    Position += 3;
                    Tokens.push_back(Token{TokenType::PUNCTUATION, std::string("!~*"), OpBegin});
                    if(Tokens.size() > Limits::MaxSqlTokens)
                        LexFail(OpBegin,
                                "Too many SQL tokens (limit is a safety guard against malformed or hostile input).");
                    continue;
                }
            }
            if(Position + 1 < Query_.size()) {
                std::string TwoChars = std::string(Query_.substr(Position, 2));
                if(TwoChars == "||" || TwoChars == "::" || TwoChars == "//" || TwoChars == "<=" ||
                   TwoChars == ">=" || TwoChars == "!=" || TwoChars == "==" || TwoChars == "<>" || TwoChars == "~*" || TwoChars == "!~") {
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
		while(Self->CurrentIndex_ < Self->Tokens_.size()) {
			const Token &T = Self->Tokens_[Self->CurrentIndex_];
			if(T.Type == TokenType::PUNCTUATION && T.Value == ";")
				Self->AdvanceToken();
			else
				break;
		}
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
    if(auto Sfb = TryParseScalarSqlBuiltinSelectExpr())
        return Sfb;
    if(auto Agg = TryParseAggregateFuncPrimary())
        return Agg;
    if(CurrentToken()->Value == "(") {
        AdvanceToken();
        auto First = ParseExpression();
        if(CurrentToken() && CurrentToken()->Value == ",") {
            std::vector<std::unique_ptr<ExpressionAST>> Elems;
            Elems.push_back(AsExpr(std::move(First)));
            while(CurrentToken() && CurrentToken()->Value == ",") {
                AdvanceToken();
                Elems.push_back(AsExpr(ParseExpression()));
            }
            if(!MatchToken(TokenType::PUNCTUATION, ")"))
                ParseFail("Expected ')' in row constructor");
            return std::make_unique<RowConstructorExprAST>(std::move(Elems));
        }
        if(!MatchToken(TokenType::PUNCTUATION, ")"))
            ParseFail("Expected ')' in primary expression");
        return First;
    }
    if(CurrentToken()->Type == TokenType::KEYWORD && CurrentToken()->Value == "NULL") {
        AdvanceToken();
        return std::make_unique<NullLiteralAST>();
    }
    if(MatchKeyword("TRUE"))
        return std::make_unique<BooleanLiteralAST>(true);
    if(MatchKeyword("FALSE"))
        return std::make_unique<BooleanLiteralAST>(false);
    auto UpperTok = [](std::string S) {
        for(char &C : S)
            C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
        return S;
    };
    const bool LooksLikeAny =
        (CurrentToken()->Type == TokenType::KEYWORD && CurrentToken()->Value == "ANY") ||
        (CurrentToken()->Type == TokenType::IDENTIFIER && UpperTok(CurrentToken()->Value) == "ANY");
    const bool LooksLikeSome =
        (CurrentToken()->Type == TokenType::KEYWORD && CurrentToken()->Value == "SOME") ||
        (CurrentToken()->Type == TokenType::IDENTIFIER && UpperTok(CurrentToken()->Value) == "SOME");
    const bool LooksLikeAll =
        (CurrentToken()->Type == TokenType::KEYWORD && CurrentToken()->Value == "ALL") ||
        (CurrentToken()->Type == TokenType::IDENTIFIER && UpperTok(CurrentToken()->Value) == "ALL");
    if(LooksLikeAny || LooksLikeSome || LooksLikeAll) {
        const QuantifiedSubqueryKind Qk = LooksLikeAll ? QuantifiedSubqueryKind::All : QuantifiedSubqueryKind::Any;
        AdvanceToken();
        if(!CurrentToken() || CurrentToken()->Value != "(")
            ParseFail("ANY/SOME/ALL requires '(SELECT ...)'");
        AdvanceToken();
        if(!MatchToken(TokenType::SELECT))
            ParseFail("ANY/SOME/ALL expects a SELECT subquery");
        std::string InnerCol;
        if(CurrentToken() && CurrentToken()->Value == "*")
            AdvanceToken();
        else if(auto ColTok = CurrentToken();
                ColTok && (ColTok->Type == TokenType::IDENTIFIER || ColTok->Type == TokenType::KEYWORD)) {
            InnerCol = ColTok->Value;
            AdvanceToken();
        } else
            ParseFail("ANY/SOME/ALL subquery expects SELECT column or *");
        if(!MatchKeyword("FROM"))
            ParseFail("ANY/SOME/ALL subquery requires FROM clause");
        auto TTok = CurrentToken();
        if(!TTok || (TTok->Type != TokenType::IDENTIFIER && TTok->Type != TokenType::KEYWORD))
            ParseFail("ANY/SOME/ALL expects table name after FROM");
        std::string Tbl = TTok->Value;
        AdvanceToken();
        ApplyCteSubstitution(Tbl);
        std::unique_ptr<ExpressionAST> Where = nullptr;
        if(CurrentToken() && CurrentToken()->Value == "WHERE") {
            AdvanceToken();
            auto W = ParseBinaryOperation();
            Where = W ? std::unique_ptr<ExpressionAST>(static_cast<ExpressionAST *>(W.release())) : nullptr;
        }
        if(!CurrentToken() || CurrentToken()->Value != ")")
            ParseFail("Expected ')' closing ANY/SOME/ALL subquery");
        AdvanceToken();
        return std::make_unique<QuantifiedSubqueryAST>(Qk, std::move(Tbl), std::move(InnerCol), std::move(Where));
    }
    if(CurrentToken()->Type == TokenType::IDENTIFIER || CurrentToken()->Type == TokenType::KEYWORD) {
        std::string N = CurrentToken()->Value;
        AdvanceToken();
        if(CurrentToken() && CurrentToken()->Value == ".") {
            AdvanceToken();
            auto Col = CurrentToken();
            if(!Col || (Col->Type != TokenType::IDENTIFIER && Col->Type != TokenType::KEYWORD))
                ParseFail("Expected column name after '.' in predicate");
            std::string Cn = Col->Value;
            AdvanceToken();
            return std::make_unique<ColumnRefAST>(N + "." + Cn);
        }
        return std::make_unique<ColumnRefAST>(std::move(N));
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
	if(CurrentToken() && CurrentToken()->Value == ".") {
		AdvanceToken();
		auto ArgTk2 = CurrentToken();
		if(!ArgTk2 || ArgTk2->Type != TokenType::IDENTIFIER)
			ParseFail("Expected column name after '.' in aggregate in HAVING");
		Arg = ArgTk2->Value;
		AdvanceToken();
	}
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
            if(MatchKeyword("IN")) {
                if(!CurrentToken() || CurrentToken()->Value != "(")
                    ParseFail("Expected '(' after NOT IN");
                AdvanceToken();
                if(MatchToken(TokenType::SELECT)) {
                    Node = ParseInSubqueryPredicate(true, AsExpr(std::move(Node)));
                    continue;
                }
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
                    ParseFail("NOT IN clause requires at least one value");
                auto InRhs = std::make_unique<InValuesAST>(std::move(LitVals));
                Node = std::make_unique<BinaryOpAST>(AsExpr(std::move(Node)), "NOT IN",
                                                     AsExpr(std::move(InRhs)));
                continue;
            }
            if(MatchKeyword("LIKE")) {
                auto Pat = ParsePrimary();
                Node = std::make_unique<BinaryOpAST>(AsExpr(std::move(Node)), "NOT LIKE",
                                                     AsExpr(std::move(Pat)));
                continue;
            }
            if(MatchKeyword("ILIKE")) {
                auto Pat = ParsePrimary();
                Node = std::make_unique<BinaryOpAST>(AsExpr(std::move(Node)), "NOT ILIKE",
                                                     AsExpr(std::move(Pat)));
                continue;
            }
            if(MatchKeyword("GLOB")) {
                auto Pat = ParsePrimary();
                Node = std::make_unique<BinaryOpAST>(AsExpr(std::move(Node)), "NOT GLOB",
                                                     AsExpr(std::move(Pat)));
                continue;
            }
            if(MatchKeyword("REGEXP")) {
                auto Pat = ParsePrimary();
                Node = std::make_unique<BinaryOpAST>(AsExpr(std::move(Node)), "NOT REGEXP",
                                                     AsExpr(std::move(Pat)));
                continue;
            }
            ParseFail("Expected IN, LIKE, ILIKE, GLOB, or REGEXP after NOT");
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
            if(MatchToken(TokenType::SELECT)) {
                Node = ParseInSubqueryPredicate(false, AsExpr(std::move(Node)));
                continue;
            }
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
            if(MatchKeyword("ILIKE")) {
                auto Pat = ParsePrimary();
                Node = std::make_unique<BinaryOpAST>(AsExpr(std::move(Node)), "ILIKE", AsExpr(std::move(Pat)));
                continue;
            }
            if(MatchKeyword("GLOB")) {
                auto Pat = ParsePrimary();
                Node = std::make_unique<BinaryOpAST>(AsExpr(std::move(Node)), "GLOB", AsExpr(std::move(Pat)));
                continue;
            }
            if(MatchKeyword("REGEXP")) {
                auto Pat = ParsePrimary();
                Node = std::make_unique<BinaryOpAST>(AsExpr(std::move(Node)), "REGEXP", AsExpr(std::move(Pat)));
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

std::string Parser::ParseDataType(std::vector<std::string> *DialectConstraints) {
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
	if(Base == "SERIAL" || Base == "SERIAL4") {
		if(DialectConstraints)
			DialectConstraints->push_back("IDENTITY:0:1:1");
		return "INTEGER";
	}
	if(Base == "BIGSERIAL" || Base == "SERIAL8") {
		if(DialectConstraints)
			DialectConstraints->push_back("IDENTITY:0:1:1");
		return "BIGINT";
	}
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
	if(Canon == "UUID")
		return "UUID";
	if(Canon == "INTERVAL")
		return "INTERVAL";
	if(Canon == "COMPLEX")
		return "COMPLEX";
	if(Canon == "VARIANT" || Canon == "ANY")
		return "VARIANT";
	if(Canon == "TERRAIN" || Canon == "TERRAIN_POINT")
		return "TERRAIN";
	if(Canon == "MESH")
		return "MESH";
	if(Canon == "POLYGON")
		return "POLYGON";
	if(Canon == "ROW" || Canon == "MULTISET") {
		if(!CurrentToken() || CurrentToken()->Value != "(")
			ParseFail("Expected '(' after " + Canon);
		int Depth = 0;
		std::ostringstream O;
		O << Canon;
		do {
			auto Tk = CurrentToken();
			if(!Tk)
				ParseFail("Unclosed type parameter list");
			if(Tk->Value == "(")
				++Depth;
			if(Tk->Value == ")")
				--Depth;
			O << Tk->Value;
			AdvanceToken();
		} while(Depth > 0);
		return std::move(O).str();
	}
	if(Canon == "ARRAY")
		Canon = "LIST";
	if(Canon == "MDARRAY")
		Canon = "MATRIX";
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
	if(P == "UUID" || P == "INTERVAL")
		return SqlCastTarget::Text;
	if(P.rfind("ROW(", 0) == 0 || P.rfind("MULTISET(", 0) == 0)
		return SqlCastTarget::Advanced;
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
        if(MatchKeyword("AUTO_INCREMENT") || MatchKeyword("AUTOINCREMENT")) {
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
	SecondaryIndexKind Kind = SecondaryIndexKind::BTree;
	int64_t MetricTag = 1;
	if(MatchKeyword("USING")) {
		if(MatchKeyword("FTS"))
			Kind = SecondaryIndexKind::Fts;
		else if(MatchKeyword("BTREE"))
			Kind = SecondaryIndexKind::BTree;
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
			ParseFail("Expected BTREE, FTS, or VECTOR after USING.");
	}
	(void)IfNotExists;
	return std::make_unique<CreateIndexAST>(std::move(IdxName), std::move(TableName), std::move(ColumnName), Kind,
	                                        MetricTag);
}

std::string Parser::ParseIdentifiedByPassword() {
	if(!MatchKeyword("IDENTIFIED"))
		ParseFail("Expected IDENTIFIED BY password");
	if(!MatchKeyword("BY"))
		ParseFail("Expected IDENTIFIED BY password");
	auto PwTok = CurrentToken();
	if(!PwTok)
		ParseFail("Expected password after IDENTIFIED BY");
	if(PwTok->Type != TokenType::LITERAL && PwTok->Type != TokenType::IDENTIFIER)
		ParseFail("Password must be a string literal or identifier");
	std::string Password = PwTok->Value;
	AdvanceToken();
	return Password;
}

ASTNode Parser::ParseCreateUserStatement() {
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
		ParseFail("Expected user name after CREATE USER");
	std::string UserName = NameTok->Value;
	AdvanceToken();
	std::string Password = ParseIdentifiedByPassword();
	return std::make_unique<CreateUserAST>(std::move(UserName), std::move(Password), IfNotExists);
}

std::string_view Parser::SliceStatementFrom(std::size_t BeginByte) const {
	const std::size_t End = StatementEndBeforeSemicolon(Query_, BeginByte);
	return Query_.substr(BeginByte, End - BeginByte);
}

void Parser::AdvanceThroughStatementSemicolon(std::size_t StmtStart) {
	const std::size_t EndByte = StatementEndBeforeSemicolon(Query_, StmtStart);
	while(CurrentIndex_ < Tokens_.size()) {
		const Token &Tok = Tokens_[CurrentIndex_];
		if(Tok.Begin >= EndByte) {
			if(Tok.Type == TokenType::PUNCTUATION && Tok.Value == ";")
				AdvanceToken();
			return;
		}
		AdvanceToken();
	}
}

ASTNode Parser::ParseCreateStatement() {
	AdvanceToken();
	const std::size_t StmtStart = CurrentIndex_ > 0 ? Tokens_[CurrentIndex_ - 1].Begin : 0;
	bool OrReplace = false;
	const std::size_t BeforeOr = CurrentIndex_;
	if(MatchKeyword("OR")) {
		if(MatchKeyword("ALTER")) {
			const auto AfterAlter = CurrentToken();
			if(AfterAlter && AfterAlter->Type == TokenType::KEYWORD
			   && (AfterAlter->Value == "PROCEDURE" || AfterAlter->Value == "PROC" || AfterAlter->Value == "FUNCTION"))
				OrReplace = true;
			else
				CurrentIndex_ = BeforeOr;
		} else if(MatchKeyword("REPLACE")) {
			const auto AfterReplace = CurrentToken();
			if(AfterReplace && AfterReplace->Type == TokenType::KEYWORD
			   && (AfterReplace->Value == "PROCEDURE" || AfterReplace->Value == "PROC" || AfterReplace->Value == "FUNCTION"))
				OrReplace = true;
			else
				CurrentIndex_ = BeforeOr;
		} else {
			ParseFail("Expected REPLACE or ALTER in CREATE OR clause.");
		}
	}
	if(MatchKeyword("INDEX"))
		return ParseCreateIndexStatement();
	if(MatchKeyword("TYPE")) {
		auto TypeTok = CurrentToken();
		if(!TypeTok || TypeTok->Type != TokenType::IDENTIFIER)
			ParseFail("Expected type name after CREATE TYPE.");
		std::string TypeName = TypeTok->Value;
		AdvanceToken();
		if(!MatchKeyword("AS"))
			ParseFail("CREATE TYPE requires AS (<field definitions>).");
		auto Fields = ParseObjectTypeFieldList();
		return std::make_unique<CreateTypeAST>(std::move(TypeName), std::move(Fields));
	}
	if(MatchKeyword("VIEW"))
		return ParseCreateViewStatement();
	const auto NextTok = CurrentToken();
	const bool IsProcedure = NextTok && NextTok->Type == TokenType::KEYWORD
	                         && (NextTok->Value == "PROCEDURE" || NextTok->Value == "PROC");
	const bool IsFunction = NextTok && NextTok->Type == TokenType::KEYWORD && NextTok->Value == "FUNCTION";
	if(MatchKeyword("TRIGGER"))
		return ParseCreateTriggerStatement(OrReplace);
	if(IsProcedure || IsFunction) {
		const std::string_view Slice = SliceStatementFrom(StmtStart);
		if(ProcedureParser::IsDialectProcedureStatement(Slice)) {
			AdvanceToken();
			ProcedureParseResult R = ProcedureParser(Slice).ParseDialectCreate();
			R.OrReplace = R.OrReplace || OrReplace;
			AdvanceThroughStatementSemicolon(StmtStart);
			return std::make_unique<CreateProcedureAST>(
			    std::move(R.ProcedureName), std::move(R.LoweredBodySql), R.IfNotExists, R.OrReplace,
			    std::move(R.DialectTag), std::move(R.Body_.ExceptionHandlers), std::string{}, std::move(R.Body_));
		}
		if(IsFunction)
			ParseFail("CREATE FUNCTION requires PL/pgSQL or LANGUAGE SQL syntax (LANGUAGE plpgsql|sql AS $$ … $$).");
		AdvanceToken();
		return ParseCreateProcedureStatement(OrReplace);
	}
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
	if(MatchKeyword("GRAPH")) {
		if(MatchKeyword("PROJECTION"))
			return ParseCreateGraphProjectionStatement();
		return ParseCreateGraphStatement();
	}
	if(MatchKeyword("EMBEDDING")) {
		auto Nt = CurrentToken();
		if(!Nt || Nt->Type != TokenType::IDENTIFIER)
			ParseFail("Expected embedding name after CREATE EMBEDDING.");
		std::string EmbName = Nt->Value;
		AdvanceToken();
		if(!MatchKeyword("AS") || !MatchKeyword("TABLE"))
			ParseFail("CREATE EMBEDDING requires AS TABLE table (token_col, vector_col).");
		auto Tt = CurrentToken();
		if(!Tt || Tt->Type != TokenType::IDENTIFIER)
			ParseFail("Expected table name after CREATE EMBEDDING AS TABLE.");
		std::string Src = Tt->Value;
		AdvanceToken();
		if(!MatchToken(TokenType::PUNCTUATION, "("))
			ParseFail("Expected '(' after CREATE EMBEDDING table name.");
		auto TokCol = CurrentToken();
		if(!TokCol || TokCol->Type != TokenType::IDENTIFIER)
			ParseFail("Expected token column name.");
		std::string TokenColumn = TokCol->Value;
		AdvanceToken();
		if(!MatchToken(TokenType::PUNCTUATION, ","))
			ParseFail("Expected ',' between CREATE EMBEDDING columns.");
		auto VecCol = CurrentToken();
		if(!VecCol || VecCol->Type != TokenType::IDENTIFIER)
			ParseFail("Expected vector column name.");
		std::string VectorColumn = VecCol->Value;
		AdvanceToken();
		if(!MatchToken(TokenType::PUNCTUATION, ")"))
			ParseFail("Expected ')' after CREATE EMBEDDING column list.");
		return std::make_unique<CreateEmbeddingAST>(std::move(EmbName), std::move(Src), std::move(TokenColumn),
		                                            std::move(VectorColumn));
	}
	if(MatchKeyword("DATASET")) {
		auto Nt = CurrentToken();
		if(!Nt || Nt->Type != TokenType::IDENTIFIER)
			ParseFail("Expected dataset name after CREATE DATASET.");
		std::string DsName = Nt->Value;
		AdvanceToken();
		if(!MatchKeyword("AS"))
			ParseFail("CREATE DATASET requires AS TABLE or AS BULK.");
		if(MatchKeyword("TABLE")) {
			auto Tt = CurrentToken();
			if(!Tt || Tt->Type != TokenType::IDENTIFIER)
				ParseFail("Expected table name after CREATE DATASET AS TABLE.");
			std::string Src = Tt->Value;
			AdvanceToken();
			return std::make_unique<CreateDatasetAST>(std::move(DsName), DatasetKind::TableRef, std::move(Src), 0,
			                                          1, 1);
		}
		if(MatchKeyword("BULK")) {
			auto Cnt = CurrentToken();
			if(!Cnt || Cnt->Type != TokenType::LITERAL)
				ParseFail("Expected integer after CREATE DATASET AS BULK.");
			int64_t Count = std::stoll(Cnt->Value);
			AdvanceToken();
			int64_t Start = 1;
			int64_t Step = 1;
			if(MatchKeyword("START")) {
				auto St = CurrentToken();
				if(!St || St->Type != TokenType::LITERAL)
					ParseFail("Expected integer after START.");
				Start = std::stoll(St->Value);
				AdvanceToken();
			}
			if(MatchKeyword("STEP")) {
				auto Sp = CurrentToken();
				if(!Sp || Sp->Type != TokenType::LITERAL)
					ParseFail("Expected integer after STEP.");
				Step = std::stoll(Sp->Value);
				AdvanceToken();
			}
			return std::make_unique<CreateDatasetAST>(std::move(DsName), DatasetKind::BulkFixture, std::string(), Count,
			                                          Start, Step);
		}
		ParseFail("CREATE DATASET requires AS TABLE or AS BULK.");
	}
	if(MatchKeyword("USER"))
		return ParseCreateUserStatement();
	if(MatchKeyword("ROLE")) {
		auto RoleTok = CurrentToken();
		if(!RoleTok || RoleTok->Type != TokenType::IDENTIFIER)
			ParseFail("Expected role name after CREATE ROLE.");
		std::string RoleName = RoleTok->Value;
		AdvanceToken();
		return std::make_unique<CreateRoleAST>(std::move(RoleName));
	}
	if(!MatchKeyword("TABLE"))
		ParseFail("Expected USER, VIEW, PROCEDURE, SEQUENCE, ROLE, or TABLE after CREATE.");
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
	if(MatchKeyword("OF")) {
		auto TypeTok = CurrentToken();
		if(!TypeTok || TypeTok->Type != TokenType::IDENTIFIER)
			ParseFail("CREATE TABLE ... OF expects a type name.");
		std::string OfTypeName = TypeTok->Value;
		AdvanceToken();
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
		return std::make_unique<CreateAST>(TableName, std::vector<ColumnDefinition>{}, std::vector<TableConstraintDef>{},
		                                   IfNotExists, Storage, std::move(OfTypeName));
	}
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
			std::vector<std::string> DialectExtra;
			std::string DType = ParseDataType(&DialectExtra);
			std::vector<std::string> CList = ParseColumnConstraintList();
			/* SQLite: INTEGER PRIMARY KEY is an alias of AUTOINCREMENT/ROWID.
			 * Model it as a generated identity like SERIAL. */
			if(DType == "INTEGER") {
				bool HasPk = false;
				for(size_t i = 0; i + 1 < CList.size(); ++i) {
					if(CList[i] == "PRIMARY" && CList[i + 1] == "KEY") {
						HasPk = true;
						break;
					}
				}
				if(HasPk) {
					bool HasIdentity = false;
					for(const std::string &C : DialectExtra) {
						if(C.rfind("IDENTITY:", 0) == 0) {
							HasIdentity = true;
							break;
						}
					}
					if(!HasIdentity)
						DialectExtra.push_back("IDENTITY:0:1:1");
				}
			}
			CList.insert(CList.end(), DialectExtra.begin(), DialectExtra.end());
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

std::vector<ObjectTypeFieldDef> Parser::ParseObjectTypeFieldList() {
	if(!MatchToken(TokenType::PUNCTUATION, "("))
		ParseFail("CREATE TYPE expects '(' before field list.");
	std::vector<ObjectTypeFieldDef> Fields;
	while(true) {
		auto Ahead = CurrentToken();
		if(!Ahead)
			ParseFail("Unexpected EOF in CREATE TYPE field list.");
		if(Ahead->Value == ")") {
			AdvanceToken();
			break;
		}
		if(Ahead->Type != TokenType::IDENTIFIER && Ahead->Type != TokenType::KEYWORD)
			ParseFail("CREATE TYPE field expects identifier name.");
		ObjectTypeFieldDef Fd;
		Fd.Name = Ahead->Value;
		AdvanceToken();
		Fd.Type = ParseDataType();
		Fields.push_back(std::move(Fd));
		if(CurrentToken() && CurrentToken()->Value == ",")
			AdvanceToken();
		else if(CurrentToken() && CurrentToken()->Value == ")") {
			AdvanceToken();
			break;
		} else if(!CurrentToken())
			ParseFail("Unterminated CREATE TYPE field list.");
		else
			ParseFail("Expected ',' or ')' in CREATE TYPE field list.");
	}
	if(Fields.empty())
		ParseFail("CREATE TYPE requires at least one field.");
	return Fields;
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

ASTNode Parser::ParseCreateProcedureStatement(bool OrReplace) {
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
	const std::size_t Byte0 = CurrentIndex_ < Tokens_.size() ? Tokens_[CurrentIndex_].Begin : Query_.size();
	int ParenDepth = 1;
	while(ParenDepth > 0) {
		auto Ahead = CurrentToken();
		if(!Ahead)
			ParseFail("Unexpected EOF in CREATE PROCEDURE body.");
		if(Ahead->Type == TokenType::PUNCTUATION && Ahead->Value == "(")
			++ParenDepth;
		else if(Ahead->Type == TokenType::PUNCTUATION && Ahead->Value == ")") {
			--ParenDepth;
			if(ParenDepth == 0) {
				AdvanceToken();
				break;
			}
		}
		AdvanceToken();
	}
	std::string BodySql;
	if(CurrentIndex_ > 0) {
		const std::size_t CloseParen = Tokens_[CurrentIndex_ - 1].Begin;
		if(CloseParen > Byte0)
			BodySql = Query_.substr(Byte0, CloseParen - Byte0);
	}
	return std::make_unique<CreateProcedureAST>(std::move(Pn), std::move(BodySql), IfNotExists, OrReplace);
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

ASTNode Parser::ParseCallProcedureStatement(std::string DefaultInvokeKind) {
	std::string InvokeKind = DefaultInvokeKind;
	if(MatchKeyword("PROCEDURE")) {
		if(InvokeKind == "exec")
			InvokeKind = "exec";
		else
			InvokeKind = "execute";
	}
	auto Nt = CurrentToken();
	if(!Nt || Nt->Type != TokenType::IDENTIFIER)
		ParseFail("Expected procedure name after CALL / EXEC / EXECUTE [PROCEDURE].");
	std::string Pn = Nt->Value;
	AdvanceToken();
	return std::make_unique<CallProcedureAST>(std::move(Pn), std::move(InvokeKind));
}

ASTNode Parser::ParseCreateTriggerStatement(bool OrReplace) {
	bool IfNotExists = false;
	if(MatchKeyword("IF")) {
		if(!MatchKeyword("NOT"))
			ParseFail("Expected NOT in IF NOT EXISTS clause.");
		if(!MatchKeyword("EXISTS"))
			ParseFail("Expected EXISTS in IF NOT EXISTS clause.");
		IfNotExists = true;
	}
	auto Nt = CurrentToken();
	if(!Nt || Nt->Type != TokenType::IDENTIFIER)
		ParseFail("Expected trigger name after CREATE TRIGGER.");
	std::string Tn = Nt->Value;
	AdvanceToken();
	auto TimTok = CurrentToken();
	if(!TimTok || TimTok->Type != TokenType::KEYWORD)
		ParseFail("Expected BEFORE, AFTER, or INSTEAD OF after trigger name.");
	std::string TimingKw = TimTok->Value;
	if(TimingKw == "INSTEAD") {
		AdvanceToken();
		if(!MatchKeyword("OF"))
			ParseFail("Expected OF after INSTEAD.");
		TimingKw = "INSTEAD OF";
	} else
		AdvanceToken();
	const auto Timing = ParseTriggerTiming(TimingKw);
	if(!Timing)
		ParseFail("Expected BEFORE, AFTER, or INSTEAD OF.");
	auto EvTok = CurrentToken();
	std::string_view EventKw;
	if(EvTok) {
		if(EvTok->Type == TokenType::INSERT)
			EventKw = "INSERT";
		else if(EvTok->Type == TokenType::KEYWORD && EvTok->Value == "UPDATE")
			EventKw = "UPDATE";
		else if(EvTok->Type == TokenType::KEYWORD && EvTok->Value == "DELETE")
			EventKw = "DELETE";
	}
	if(EventKw.empty())
		ParseFail("Expected INSERT, UPDATE, or DELETE.");
	const auto Event = ParseTriggerEvent(EventKw);
	if(!Event)
		ParseFail("Expected INSERT, UPDATE, or DELETE.");
	AdvanceToken();
	if(!MatchKeyword("ON"))
		ParseFail("Expected ON after trigger event.");
	auto TblTok = CurrentToken();
	if(!TblTok || TblTok->Type != TokenType::IDENTIFIER)
		ParseFail("Expected table name after ON.");
	std::string TableName = TblTok->Value;
	AdvanceToken();
	bool ForEachRow = false;
	if(MatchKeyword("FOR")) {
		if(!MatchKeyword("EACH"))
			ParseFail("Expected EACH in FOR EACH ROW.");
		if(!MatchKeyword("ROW"))
			ParseFail("Expected ROW in FOR EACH ROW.");
		ForEachRow = true;
	}
	std::string ActionKind;
	std::string ProcedureName;
	std::string BodySql;
	if(MatchKeyword("EXECUTE") || MatchKeyword("EXEC")) {
		if(MatchKeyword("PROCEDURE")) {
			auto Pn = CurrentToken();
			if(!Pn || Pn->Type != TokenType::IDENTIFIER)
				ParseFail("Expected procedure name after EXECUTE PROCEDURE.");
			ProcedureName = Pn->Value;
			AdvanceToken();
		} else {
			ParseFail("Expected PROCEDURE name after EXECUTE (use EXECUTE PROCEDURE name).");
		}
		ActionKind = "procedure";
		BodySql = "CALL " + ProcedureName + ";";
	} else if(MatchKeyword("CALL")) {
		auto Pn = CurrentToken();
		if(!Pn || Pn->Type != TokenType::IDENTIFIER)
			ParseFail("Expected procedure name after CALL.");
		ProcedureName = Pn->Value;
		AdvanceToken();
		ActionKind = "procedure";
		BodySql = "CALL " + ProcedureName + ";";
	} else if(MatchKeyword("AS")) {
		if(!MatchToken(TokenType::PUNCTUATION, "("))
			ParseFail("Expected '(' after AS in CREATE TRIGGER.");
		const std::size_t Byte0 = Tokens_[CurrentIndex_].Begin;
		while(true) {
			auto Ahead = CurrentToken();
			if(!Ahead)
				ParseFail("Unexpected EOF in CREATE TRIGGER body.");
			if(Ahead->Type == TokenType::PUNCTUATION && Ahead->Value == ")") {
				AdvanceToken();
				break;
			}
			if(Ahead->Type == TokenType::PUNCTUATION && Ahead->Value == ";") {
				AdvanceToken();
				continue;
			}
			if(!ParseStatement())
				ParseFail("Expected SQL statement inside CREATE TRIGGER body.");
		}
		const std::size_t CloseParen = CurrentIndex_ > 0 ? Tokens_[CurrentIndex_ - 1].Begin : Byte0;
		if(CloseParen > Byte0)
			BodySql = std::string(Query_.substr(Byte0, CloseParen - Byte0));
		ActionKind = "inline";
	} else
		ParseFail("Expected EXECUTE PROCEDURE, CALL, or AS (…) after CREATE TRIGGER header.");
	auto Ast = std::make_unique<CreateTriggerAST>();
	Ast->TriggerName = std::move(Tn);
	Ast->TableName = std::move(TableName);
	Ast->Timing = *Timing;
	Ast->Event = *Event;
	Ast->ForEachRow = ForEachRow;
	Ast->ActionKind = std::move(ActionKind);
	Ast->ProcedureName = std::move(ProcedureName);
	Ast->BodySql_ = std::move(BodySql);
	Ast->IfNotExists = IfNotExists;
	Ast->OrReplace = OrReplace;
	return Ast;
}

ASTNode Parser::ParseDropTriggerStatement() {
	bool IfExists = false;
	if(MatchKeyword("IF")) {
		if(!MatchKeyword("EXISTS"))
			ParseFail("Expected EXISTS in IF EXISTS clause.");
		IfExists = true;
	}
	auto Nt = CurrentToken();
	if(!Nt || Nt->Type != TokenType::IDENTIFIER)
		ParseFail("Expected trigger name after DROP TRIGGER.");
	std::string Tn = Nt->Value;
	AdvanceToken();
	return std::make_unique<DropTriggerAST>(std::move(Tn), IfExists);
}

ASTNode Parser::ParseAlterTriggerStatement() {
	auto Nt = CurrentToken();
	if(!Nt || Nt->Type != TokenType::IDENTIFIER)
		ParseFail("Expected trigger name after ALTER TRIGGER.");
	std::string Tn = Nt->Value;
	AdvanceToken();
	if(MatchKeyword("ENABLE"))
		return std::make_unique<AlterTriggerAST>(std::move(Tn), true);
	if(MatchKeyword("DISABLE"))
		return std::make_unique<AlterTriggerAST>(std::move(Tn), false);
	ParseFail("Expected ENABLE or DISABLE after ALTER TRIGGER name.");
}

ASTNode Parser::ParseDropStatement() {
    AdvanceToken();
	if(MatchKeyword("USER")) {
		bool IfExists = false;
		if(MatchKeyword("IF")) {
			if(!MatchKeyword("EXISTS"))
				ParseFail("Expected EXISTS in IF EXISTS clause");
			IfExists = true;
		}
		auto UserTok = CurrentToken();
		if(!UserTok || UserTok->Type != TokenType::IDENTIFIER)
			ParseFail("Expected user name after DROP USER.");
		std::string UserName = UserTok->Value;
		AdvanceToken();
		return std::make_unique<DropUserAST>(std::move(UserName), IfExists);
	}
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
	if(MatchKeyword("EMBEDDING")) {
		auto Nt = CurrentToken();
		if(!Nt || Nt->Type != TokenType::IDENTIFIER)
			ParseFail("Expected embedding name after DROP EMBEDDING.");
		std::string EmbName = Nt->Value;
		AdvanceToken();
		return std::make_unique<DropEmbeddingAST>(std::move(EmbName));
	}
	if(MatchKeyword("DATASET")) {
		auto Nt = CurrentToken();
		if(!Nt || Nt->Type != TokenType::IDENTIFIER)
			ParseFail("Expected dataset name after DROP DATASET.");
		std::string DsName = Nt->Value;
		AdvanceToken();
		return std::make_unique<DropDatasetAST>(std::move(DsName));
	}
	if(MatchKeyword("GRAPH")) {
		auto Nt = CurrentToken();
		if(!Nt || Nt->Type != TokenType::IDENTIFIER)
			ParseFail("Expected graph name after DROP GRAPH.");
		std::string Gn = Nt->Value;
		AdvanceToken();
		return std::make_unique<DropGraphAST>(std::move(Gn));
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
	if(MatchKeyword("TYPE")) {
		bool IfExistsTy = false;
		if(MatchKeyword("IF")) {
			if(!MatchKeyword("EXISTS"))
				ParseFail("Expected EXISTS in IF EXISTS clause");
			IfExistsTy = true;
		}
		auto Nt = CurrentToken();
		if(!Nt || Nt->Type != TokenType::IDENTIFIER)
			ParseFail("Expected type name after DROP TYPE.");
		std::string TypeName = Nt->Value;
		AdvanceToken();
		return std::make_unique<DropTypeAST>(std::move(TypeName), IfExistsTy);
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
	if(MatchKeyword("TRIGGER"))
		return ParseDropTriggerStatement();
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
        const bool LooksLikeRownum =
            (TokenOpt->Type == TokenType::KEYWORD && TokenOpt->Value == "ROWNUM") ||
            (TokenOpt->Type == TokenType::IDENTIFIER && FoldUpperAscii(TokenOpt->Value) == "ROWNUM");
        const bool LooksLikeRowNumber =
            (TokenOpt->Type == TokenType::KEYWORD && TokenOpt->Value == "ROW_NUMBER") ||
            (TokenOpt->Type == TokenType::IDENTIFIER && FoldUpperAscii(TokenOpt->Value) == "ROW_NUMBER");
        if(LooksLikeRownum) {
            AdvanceToken();
            WindowSpec Ws;
            Ws.Kind = WindowFnKind::RowNumber;
            Ws.OrderColumn = std::string();
            Ws.OutputColumn = "rownum";
            if(MatchKeyword("AS")) {
                auto At = CurrentToken();
                if(!At)
                    ParseFail("Expected alias after AS for ROWNUM");
                Ws.OutputColumn = At->Value;
                AdvanceToken();
            }
            if(WindowSpecs.size() >= Limits::MaxWindowFunctionsPerSelect)
                ParseFail("Too many window functions in one SELECT (see Limits::MaxWindowFunctionsPerSelect).");
            WindowSpecs.push_back(std::move(Ws));
            Columns.push_back(WindowSpecs.back().OutputColumn);
            ProjectionExprs.push_back(nullptr);
            if(auto NextToken = CurrentToken(); NextToken && NextToken->Value == ",")
                AdvanceToken();
            continue;
        }
        const bool LooksLikeLag =
            (TokenOpt->Type == TokenType::KEYWORD && TokenOpt->Value == "LAG") ||
            (TokenOpt->Type == TokenType::IDENTIFIER && FoldUpperAscii(TokenOpt->Value) == "LAG");
        const bool LooksLikeLead =
            (TokenOpt->Type == TokenType::KEYWORD && TokenOpt->Value == "LEAD") ||
            (TokenOpt->Type == TokenType::IDENTIFIER && FoldUpperAscii(TokenOpt->Value) == "LEAD");
        const bool LooksLikeFirstValue =
            (TokenOpt->Type == TokenType::KEYWORD && TokenOpt->Value == "FIRST_VALUE") ||
            (TokenOpt->Type == TokenType::IDENTIFIER && FoldUpperAscii(TokenOpt->Value) == "FIRST_VALUE");
        const bool LooksLikeLastValue =
            (TokenOpt->Type == TokenType::KEYWORD && TokenOpt->Value == "LAST_VALUE") ||
            (TokenOpt->Type == TokenType::IDENTIFIER && FoldUpperAscii(TokenOpt->Value) == "LAST_VALUE");
        const bool LooksLikeNthValue =
            (TokenOpt->Type == TokenType::KEYWORD && TokenOpt->Value == "NTH_VALUE") ||
            (TokenOpt->Type == TokenType::IDENTIFIER && FoldUpperAscii(TokenOpt->Value) == "NTH_VALUE");
        const bool LooksLikePercentRank =
            (TokenOpt->Type == TokenType::KEYWORD && TokenOpt->Value == "PERCENT_RANK") ||
            (TokenOpt->Type == TokenType::IDENTIFIER && FoldUpperAscii(TokenOpt->Value) == "PERCENT_RANK");
        const bool LooksLikeCumeDist =
            (TokenOpt->Type == TokenType::KEYWORD && TokenOpt->Value == "CUME_DIST") ||
            (TokenOpt->Type == TokenType::IDENTIFIER && FoldUpperAscii(TokenOpt->Value) == "CUME_DIST");
        const bool LooksLikeNtile =
            (TokenOpt->Type == TokenType::KEYWORD && TokenOpt->Value == "NTILE") ||
            (TokenOpt->Type == TokenType::IDENTIFIER && FoldUpperAscii(TokenOpt->Value) == "NTILE");
        if(LooksLikeDenseRank || LooksLikeRank || LooksLikeRowNumber || LooksLikeLag || LooksLikeLead ||
           LooksLikeFirstValue || LooksLikeLastValue || LooksLikeNthValue || LooksLikePercentRank ||
           LooksLikeCumeDist || LooksLikeNtile) {
            WindowSpec Ws;
            if(LooksLikeDenseRank)
                Ws.Kind = WindowFnKind::DenseRank;
            else if(LooksLikeRank)
                Ws.Kind = WindowFnKind::Rank;
            else if(LooksLikeRowNumber)
                Ws.Kind = WindowFnKind::RowNumber;
            else if(LooksLikeLag)
                Ws.Kind = WindowFnKind::Lag;
            else if(LooksLikeFirstValue)
                Ws.Kind = WindowFnKind::FirstValue;
            else if(LooksLikeLastValue)
                Ws.Kind = WindowFnKind::LastValue;
            else if(LooksLikeNthValue)
                Ws.Kind = WindowFnKind::NthValue;
            else if(LooksLikePercentRank)
                Ws.Kind = WindowFnKind::PercentRank;
            else if(LooksLikeCumeDist)
                Ws.Kind = WindowFnKind::CumeDist;
            else if(LooksLikeNtile)
                Ws.Kind = WindowFnKind::Ntile;
            else
                Ws.Kind = WindowFnKind::Lead;
            AdvanceToken();
            if(!CurrentToken() || CurrentToken()->Value != "(")
                ParseFail("Expected '(' after window function name");
            AdvanceToken();
            if(Ws.Kind == WindowFnKind::Lag || Ws.Kind == WindowFnKind::Lead) {
                if(!TryParseWindowAggArgument(Ws.SourceColumn)) {
                    Ws.SourceColumn = ParseQualifiedSqlName();
                }
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
            } else if(Ws.Kind == WindowFnKind::FirstValue || Ws.Kind == WindowFnKind::LastValue) {
                auto ColTk = CurrentToken();
                if(!ColTk || ColTk->Type != TokenType::IDENTIFIER)
                    ParseFail("FIRST_VALUE/LAST_VALUE expect a column name inside ()");
                Ws.SourceColumn = ColTk->Value;
                AdvanceToken();
            } else if(Ws.Kind == WindowFnKind::NthValue) {
                auto ColTk = CurrentToken();
                if(!ColTk || ColTk->Type != TokenType::IDENTIFIER)
                    ParseFail("NTH_VALUE expects a source column as first argument");
                Ws.SourceColumn = ColTk->Value;
                AdvanceToken();
                if(!CurrentToken() || CurrentToken()->Value != ",")
                    ParseFail("NTH_VALUE expects N as a second argument");
                AdvanceToken();
                auto Nt = CurrentToken();
                if(!Nt || Nt->Type != TokenType::LITERAL)
                    ParseFail("NTH_VALUE N must be a positive integer literal");
                try {
                    const long long N = std::stoll(Nt->Value);
                    if(N <= 0)
                        ParseFail("NTH_VALUE N must be positive");
                    Ws.FrameOffset = N;
                } catch(...) {
                    ParseFail("NTH_VALUE N must be a positive integer literal");
                }
                AdvanceToken();
            } else if(Ws.Kind == WindowFnKind::Ntile) {
                auto Nt = CurrentToken();
                if(!Nt || Nt->Type != TokenType::LITERAL)
                    ParseFail("NTILE expects a positive integer bucket count");
                try {
                    const long long N = std::stoll(Nt->Value);
                    if(N <= 0)
                        ParseFail("NTILE bucket count must be positive");
                    Ws.FrameOffset = N;
                } catch(...) {
                    ParseFail("NTILE bucket count must be a positive integer literal");
                }
                AdvanceToken();
            } else if(Ws.Kind == WindowFnKind::PercentRank || Ws.Kind == WindowFnKind::CumeDist) {
                // no arguments
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
                Ws.Kind == WindowFnKind::Lead ?
                    std::string("lead_") + Ws.SourceColumn :
                Ws.Kind == WindowFnKind::FirstValue ?
                    std::string("first_") + Ws.SourceColumn :
                Ws.Kind == WindowFnKind::LastValue ?
                    std::string("last_") + Ws.SourceColumn :
                Ws.Kind == WindowFnKind::NthValue ?
                    std::string("nth_") + Ws.SourceColumn :
                Ws.Kind == WindowFnKind::PercentRank ?
                    std::string("percent_rank") :
                Ws.Kind == WindowFnKind::CumeDist ?
                    std::string("cume_dist") :
                    std::string("ntile");
            if(MatchKeyword("AS")) {
                auto At = CurrentToken();
                if(!IsSelectAliasToken(At))
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
            auto CastOperand = ParseScalarExpression();
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
        if(MatchKeyword("COALESCE") || MatchKeyword("IFNULL") || MatchKeyword("NVL")) {
            auto Ce = ParseCoalesceCallExpression();
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
        if(MatchKeyword("NVL2")) {
            auto Ce = ParseNvl2Expression();
            std::string Alias;
            if(MatchKeyword("AS")) {
                auto At = CurrentToken();
                if(!At || At->Type != TokenType::IDENTIFIER)
                    ParseFail("Expected identifier alias after AS for NVL2");
                Alias = At->Value;
                AdvanceToken();
            } else
                Alias = std::string("_nvl2") + std::to_string(NextAnonCoalesceAlias_++);
            Columns.push_back(std::move(Alias));
            ProjectionExprs.push_back(std::unique_ptr<ExpressionAST>(static_cast<ExpressionAST *>(Ce.release())));
            if(auto NextToken = CurrentToken(); NextToken && NextToken->Value == ",")
                AdvanceToken();
            continue;
        }
        if(MatchKeyword("COLUMNS")) {
            auto Cx = ParseColumnsExpression();
            std::string Placeholder = std::string(kColumnsExpandPrefix) + std::to_string(NextAnonScalarSqlFnAlias_++);
            Columns.push_back(std::move(Placeholder));
            ProjectionExprs.push_back(std::unique_ptr<ExpressionAST>(static_cast<ExpressionAST *>(Cx.release())));
            if(auto NextToken = CurrentToken(); NextToken && NextToken->Value == ",")
                AdvanceToken();
            continue;
        }
        if(MatchKeyword("DECODE")) {
            auto Ce = ParseDecodeExpression();
            std::string Alias;
            if(MatchKeyword("AS")) {
                auto At = CurrentToken();
                if(!At || At->Type != TokenType::IDENTIFIER)
                    ParseFail("Expected identifier alias after AS for DECODE");
                Alias = At->Value;
                AdvanceToken();
            } else
                Alias = std::string("_decode") + std::to_string(NextAnonCaseAlias_++);
            Columns.push_back(std::move(Alias));
            ProjectionExprs.push_back(std::unique_ptr<ExpressionAST>(static_cast<ExpressionAST *>(Ce.release())));
            if(auto NextToken = CurrentToken(); NextToken && NextToken->Value == ",")
                AdvanceToken();
            continue;
        }
        if(auto Cc = TryParseConcatProjection()) {
            std::string Alias;
            if(MatchKeyword("AS")) {
                auto At = CurrentToken();
                if(!At || At->Type != TokenType::IDENTIFIER)
                    ParseFail("Expected identifier alias after AS for concatenation");
                Alias = At->Value;
                AdvanceToken();
            } else
                Alias = std::string("_concat") + std::to_string(NextAnonScalarSqlFnAlias_++);
            Columns.push_back(std::move(Alias));
            ProjectionExprs.push_back(std::unique_ptr<ExpressionAST>(static_cast<ExpressionAST *>(Cc.release())));
            if(auto NextToken = CurrentToken(); NextToken && NextToken->Value == ",")
                AdvanceToken();
            continue;
        }
        if(auto Sfb = TryParseScalarSqlBuiltinSelectExpr()) {
            std::string Alias;
            if(MatchKeyword("AS")) {
                auto At = CurrentToken();
                if(!IsSelectAliasToken(At))
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
        auto FoldId = [&]() { return FoldUpperAscii(TokenOpt->Value); };
        const std::string Folded = FoldId();
        const bool LooksLikeStdDev =
            Folded == "STDDEV" || Folded == "STDDEV_POP" || Folded == "STDDEV_SAMP";
        const bool LooksLikeMedian = Folded == "MEDIAN";
        const bool LooksLikeMode = Folded == "MODE";
        const bool LooksLikeQuantile =
            Folded == "APPROX_QUANTILE" || Folded == "PERCENTILE_CONT";
        if(LooksLikeSum || LooksLikeMin || LooksLikeMax || LooksLikeAvg || LooksLikeStdDev ||
           LooksLikeMedian || LooksLikeMode || LooksLikeQuantile) {
            GroupCombAggKind Ak = GroupCombAggKind::Sum;
            double Quantile = 0.5;
            if(LooksLikeMin)
                Ak = GroupCombAggKind::Min;
            else if(LooksLikeMax)
                Ak = GroupCombAggKind::Max;
            else if(LooksLikeAvg)
                Ak = GroupCombAggKind::Avg;
            else if(LooksLikeMedian)
                Ak = GroupCombAggKind::Median;
            else if(LooksLikeMode)
                Ak = GroupCombAggKind::Mode;
            else if(LooksLikeQuantile)
                Ak = GroupCombAggKind::ApproxQuantile;
            else if(Folded == "STDDEV_SAMP")
                Ak = GroupCombAggKind::StdDevSamp;
            else if(LooksLikeStdDev)
                Ak = GroupCombAggKind::StdDevPop;
            AdvanceToken();
            if(!CurrentToken() || CurrentToken()->Value != "(")
                ParseFail("Expected '(' after aggregate function");
            AdvanceToken();
            auto ColTk = CurrentToken();
            if(!ColTk || ColTk->Type != TokenType::IDENTIFIER)
                ParseFail("Expected column identifier in aggregate");
            std::string SrcCol = ColTk->Value;
            AdvanceToken();
            if(CurrentToken() && CurrentToken()->Value == ".") {
                AdvanceToken();
                auto ColTk2 = CurrentToken();
                if(!ColTk2 || ColTk2->Type != TokenType::IDENTIFIER)
                    ParseFail("Expected column name after '.' in aggregate");
                SrcCol = ColTk2->Value;
                AdvanceToken();
            }
            if(Ak == GroupCombAggKind::ApproxQuantile) {
                if(CurrentToken() && CurrentToken()->Value == ",") {
                    AdvanceToken();
                    auto Qt = CurrentToken();
                    if(!Qt || Qt->Type != TokenType::LITERAL)
                        ParseFail("APPROX_QUANTILE expects numeric quantile");
                    Quantile = std::stod(Qt->Value);
                    AdvanceToken();
                }
            }
            if(!CurrentToken() || CurrentToken()->Value != ")")
                ParseFail("Expected ')' after aggregate argument");
            AdvanceToken();
            std::string OutCol =
                Ak == GroupCombAggKind::Sum ?
                    std::string("sum_") + SrcCol :
                Ak == GroupCombAggKind::Min ?
                    std::string("min_") + SrcCol :
                Ak == GroupCombAggKind::Max ? std::string("max_") + SrcCol :
                Ak == GroupCombAggKind::Avg ? std::string("avg_") + SrcCol :
                Ak == GroupCombAggKind::Median ? std::string("median_") + SrcCol :
                Ak == GroupCombAggKind::Mode ? std::string("mode_") + SrcCol :
                Ak == GroupCombAggKind::ApproxQuantile ? std::string("quantile_") + SrcCol :
                Ak == GroupCombAggKind::StdDevSamp ? std::string("stddev_samp_") + SrcCol :
                                                     std::string("stddev_pop_") + SrcCol;
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
            CombinedAggs.push_back(GroupCombAgg{Ak, SrcCol, OutCol, Quantile});
            Columns.push_back(OutCol);
            ProjectionExprs.push_back(nullptr);
            if(auto NextToken = CurrentToken(); NextToken && NextToken->Value == ",")
                AdvanceToken();
            continue;
        }
        if(TokenOpt->Type == TokenType::LITERAL) {
            std::string LitVal = TokenOpt->Value;
            AdvanceToken();
            std::string Alias;
            if(MatchKeyword("AS")) {
                auto At = CurrentToken();
                if(!IsSqlSimpleNameToken(At))
                    ParseFail("Expected identifier alias after AS for literal");
                Alias = At->Value;
                AdvanceToken();
            } else
                Alias = std::string("_lit") + std::to_string(NextAnonScalarSqlFnAlias_++);
            Columns.push_back(std::move(Alias));
            ProjectionExprs.push_back(
                std::make_unique<CaseExprAST>(std::vector<CaseExprAST::Arm>{},
                                                std::make_unique<LiteralAST>(std::move(LitVal))));
            if(auto NextToken = CurrentToken(); NextToken && NextToken->Value == ",")
                AdvanceToken();
            continue;
        }
        if(TokenOpt->Type == TokenType::IDENTIFIER ||
           (TokenOpt->Type == TokenType::KEYWORD && TokenOpt->Value != "FROM")) {
            const size_t Save = CurrentIndex_;
            AdvanceToken();
            if(CurrentToken() &&
               (CurrentToken()->Value == "::" || CurrentToken()->Value == "||" ||
                CurrentToken()->Value == "+" || CurrentToken()->Value == "-" ||
                CurrentToken()->Value == "*" || CurrentToken()->Value == "/" ||
                CurrentToken()->Value == "//" || CurrentToken()->Value == "->" ||
                CurrentToken()->Value == ".")) {
                CurrentIndex_ = Save;
                auto Expr = ParseScalarExpression();
                if(!Expr)
                    ParseFail("Expected scalar expression in SELECT list");
                std::string Alias;
                if(MatchKeyword("AS")) {
                    auto At = CurrentToken();
                    if(!IsSqlSimpleNameToken(At))
                        ParseFail("Expected identifier alias after AS for scalar expression");
                    Alias = At->Value;
                    AdvanceToken();
                } else
                    Alias = std::string("_expr") + std::to_string(NextAnonScalarSqlFnAlias_++);
                Columns.push_back(std::move(Alias));
                ProjectionExprs.push_back(
                    std::unique_ptr<ExpressionAST>(static_cast<ExpressionAST *>(Expr.release())));
                if(auto NextToken = CurrentToken(); NextToken && NextToken->Value == ",")
                    AdvanceToken();
                continue;
            }
            CurrentIndex_ = Save;
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
        std::string FromAlias;
        if(auto AliasTok = CurrentToken();
            AliasTok && AliasTok->Type == TokenType::IDENTIFIER &&
            AliasTok->Value != "WHERE" && AliasTok->Value != "ORDER" && AliasTok->Value != "LIMIT" &&
            AliasTok->Value != "GROUP" && AliasTok->Value != "HAVING" && AliasTok->Value != "OFFSET" &&
            AliasTok->Value != "INNER" && AliasTok->Value != "LEFT" && AliasTok->Value != "RIGHT" &&
            AliasTok->Value != "FULL" && AliasTok->Value != "OUTER" && AliasTok->Value != "CROSS" &&
            AliasTok->Value != "JOIN" && AliasTok->Value != "UNION" && AliasTok->Value != "INTERSECT" &&
            AliasTok->Value != "EXCEPT" && AliasTok->Value != "MATCH" && AliasTok->Value != "FOR") {
            FromAlias = AliasTok->Value;
            AdvanceToken();
        }

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
                Jc.RightAlias = At->Value;
                AdvanceToken();
            }
            Joins.push_back(std::move(Jc));
        }

        auto ParseQualifiedCol = [&]() -> std::string {
            auto Id1 = CurrentToken();
            if(!IsSqlSimpleNameToken(Id1))
                ParseFail("Expected column name");
            AdvanceToken();
            if(CurrentToken() && CurrentToken()->Value == ".") {
                AdvanceToken();
                auto Id2 = CurrentToken();
                if(!IsSqlSimpleNameToken(Id2))
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

            JoinClause Jc;
            Jc.Kind = JK;
            if(CurrentToken() && CurrentToken()->Value == "LATERAL") {
                Jc.IsLateral = true;
                AdvanceToken();
            }
            auto Rt = CurrentToken();
            if(!Rt || Rt->Type != TokenType::IDENTIFIER)
                ParseFail("Expected table name after JOIN");
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
                Jc.RightAlias = At->Value;
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
        int64_t RowNumCap = -1;
        if(WhereClause) {
            RowNumWhereSplit Split = ExtractRowNumWhere(std::move(WhereClause));
            WhereClause = std::move(Split.Remaining);
            RowNumCap = Split.Cap;
        }
        std::optional<ConnectBySpec> ConnectByOpt;
        std::unique_ptr<ExpressionAST> StartWithClause;
        auto TryStartWithClause = [&]() {
            if(MatchKeyword("START")) {
                if(!MatchKeyword("WITH"))
                    ParseFail("Expected WITH after START");
                auto Node = ParseBinaryOperation();
                StartWithClause =
                    Node ? std::unique_ptr<ExpressionAST>(static_cast<ExpressionAST *>(Node.release())) : nullptr;
            }
        };
        TryStartWithClause();
        if(MatchKeyword("CONNECT")) {
            if(!MatchKeyword("BY"))
                ParseFail("Expected BY after CONNECT");
            ConnectBySpec Cb;
            if(MatchKeyword("NOCYCLE"))
                Cb.NoCycle = true;
            bool PriorLeft = MatchKeyword("PRIOR");
            auto ParseConnectCol = [&]() -> std::string {
                auto Tk = CurrentToken();
                if(!Tk || (Tk->Type != TokenType::IDENTIFIER && Tk->Type != TokenType::KEYWORD))
                    ParseFail("CONNECT BY expects a column name");
                std::string Col = Tk->Value;
                AdvanceToken();
                if(CurrentToken() && CurrentToken()->Value == ".") {
                    AdvanceToken();
                    auto Tk2 = CurrentToken();
                    if(!Tk2 || Tk2->Type != TokenType::IDENTIFIER)
                        ParseFail("Expected column after '.' in CONNECT BY");
                    Col = Tk2->Value;
                    AdvanceToken();
                }
                return Col;
            };
            const std::string LeftCol = ParseConnectCol();
            if(!MatchToken(TokenType::PUNCTUATION, "="))
                ParseFail("CONNECT BY expects = between parent and child columns");
            const bool PriorRight = !PriorLeft && MatchKeyword("PRIOR");
            const std::string RightCol = ParseConnectCol();
            if(!PriorLeft && !PriorRight)
                ParseFail("CONNECT BY requires PRIOR on one side of the equality");
            if(PriorLeft) {
                Cb.ParentColumn = LeftCol;
                Cb.ChildColumn = RightCol;
                Cb.PriorOnParent = true;
            } else {
                Cb.ParentColumn = RightCol;
                Cb.ChildColumn = LeftCol;
                Cb.PriorOnParent = true;
            }
            ConnectByOpt = std::move(Cb);
        }
        TryStartWithClause();
        if(ConnectByOpt && StartWithClause)
            ConnectByOpt->StartWith = std::move(StartWithClause);
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
                if(ColTk->Value == "CUBE") {
                    AdvanceToken();
                    if(!CurrentToken() || CurrentToken()->Value != "(") {
                        GroupByCols.push_back("CUBE");
                        if(auto Ct = CurrentToken(); Ct && Ct->Value == ",")
                            AdvanceToken();
                        else
                            break;
                        continue;
                    }
                    AdvanceToken();
                    OlapMod = GroupOlapModifier::Cube;
                    GroupByCols.clear();
                    while(auto CubeCol = CurrentToken()) {
                        if(CubeCol->Value == ")") {
                            AdvanceToken();
                            break;
                        }
                        if(CubeCol->Type != TokenType::IDENTIFIER && CubeCol->Type != TokenType::KEYWORD)
                            ParseFail("GROUP BY CUBE expects column names");
                        std::string GKey = CubeCol->Value;
                        AdvanceToken();
                        if(CurrentToken() && CurrentToken()->Value == ".") {
                            AdvanceToken();
                            auto Id2 = CurrentToken();
                            if(!Id2 || Id2->Type != TokenType::IDENTIFIER)
                                ParseFail("Expected column name after '.' in GROUP BY CUBE");
                            GKey = Id2->Value;
                            AdvanceToken();
                        }
                        GroupByCols.push_back(std::move(GKey));
                        if(CurrentToken() && CurrentToken()->Value == ",")
                            AdvanceToken();
                    }
                    break;
                }
                if(ColTk->Value == "ROLLUP") {
                    AdvanceToken();
                    if(!CurrentToken() || CurrentToken()->Value != "(") {
                        GroupByCols.push_back("ROLLUP");
                        if(auto Ct = CurrentToken(); Ct && Ct->Value == ",")
                            AdvanceToken();
                        else
                            break;
                        continue;
                    }
                    AdvanceToken();
                    OlapMod = GroupOlapModifier::Rollup;
                    GroupByCols.clear();
                    while(auto RollCol = CurrentToken()) {
                        if(RollCol->Value == ")") {
                            AdvanceToken();
                            break;
                        }
                        if(RollCol->Type != TokenType::IDENTIFIER && RollCol->Type != TokenType::KEYWORD)
                            ParseFail("GROUP BY ROLLUP expects column names");
                        std::string GKey = RollCol->Value;
                        AdvanceToken();
                        if(CurrentToken() && CurrentToken()->Value == ".") {
                            AdvanceToken();
                            auto Id2 = CurrentToken();
                            if(!Id2 || Id2->Type != TokenType::IDENTIFIER)
                                ParseFail("Expected column name after '.' in GROUP BY ROLLUP");
                            GKey = Id2->Value;
                            AdvanceToken();
                        }
                        GroupByCols.push_back(std::move(GKey));
                        if(CurrentToken() && CurrentToken()->Value == ",")
                            AdvanceToken();
                    }
                    break;
                }
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
            std::vector<OrderBySpec>{}, RowNumCap, 0, Distinct, std::move(GroupByCols), AggMode,
            CountDistinctCol, std::move(WindowSpecs), std::move(Joins), std::move(CombinedAggs),
            std::move(ProjectionExprs), CountColArg, OlapMod, std::move(GroupingSetsList));
		if(auto Hint = ParseStorageHintFromQuery(Query_))
			Sel->SetStorageHint(*Hint);
		if(AsOfTs)
			Sel->SetAsOfTimestamp(std::move(*AsOfTs));
		if(MatchSpec)
			Sel->SetMatchRecognize(std::move(*MatchSpec));
		if(ConnectByOpt)
			Sel->SetConnectBy(std::move(*ConnectByOpt));
		if(!FromAlias.empty())
			Sel->SetSourceTableAlias(std::move(FromAlias));
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
        if(Nxt->Value != "+" && Nxt->Value != "-" && Nxt->Value != "*" && Nxt->Value != "/" &&
           Nxt->Value != "//")
            break;
        std::string Op = Nxt->Value;
        AdvanceToken();
        auto RHS = ParsePrimarySet();
        LHS = std::make_unique<BinaryOpAST>(std::move(LHS), std::move(Op), std::move(RHS));
    }
    return LHS;
}

std::unique_ptr<ExpressionAST> Parser::ParseCaseScalarResult() {
	return ParseScalarExpression();
}

std::unique_ptr<CoalesceExprAST> Parser::ParseCoalesceCallExpression() {
	if(!CurrentToken() || CurrentToken()->Value != "(")
		ParseFail("Expected '(' after COALESCE / IFNULL / NVL");
	AdvanceToken();
	std::vector<std::unique_ptr<ExpressionAST>> Args;
	for(;;) {
		Args.push_back(ParseScalarExpression());
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
		ParseFail("COALESCE / IFNULL / NVL require at least two arguments");
	return std::make_unique<CoalesceExprAST>(std::move(Args));
}

std::unique_ptr<LambdaExprAST> Parser::TryParseLambdaExpression() {
	const size_t Save = CurrentIndex_;
	if(CurrentToken() && CurrentToken()->Value == "(") {
		AdvanceToken();
		std::vector<std::string> Params;
		for(;;) {
			auto Pt = CurrentToken();
			if(!Pt || Pt->Type != TokenType::IDENTIFIER)
				break;
			Params.push_back(Pt->Value);
			AdvanceToken();
			if(CurrentToken() && CurrentToken()->Value == ",") {
				AdvanceToken();
				continue;
			}
			break;
		}
		if(Params.empty() || !CurrentToken() || CurrentToken()->Value != ")") {
			CurrentIndex_ = Save;
			return nullptr;
		}
		AdvanceToken();
		if(!MatchToken(TokenType::PUNCTUATION, "->")) {
			CurrentIndex_ = Save;
			return nullptr;
		}
		auto Body = ParseScalarAddSub();
		return std::make_unique<LambdaExprAST>(std::move(Params), std::move(Body));
	}
	auto T = CurrentToken();
	if(!T || T->Type != TokenType::IDENTIFIER) {
		CurrentIndex_ = Save;
		return nullptr;
	}
	const std::string Param = T->Value;
	AdvanceToken();
	if(!MatchToken(TokenType::PUNCTUATION, "->")) {
		CurrentIndex_ = Save;
		return nullptr;
	}
	auto Body = ParseScalarAddSub();
	return std::make_unique<LambdaExprAST>(std::vector<std::string>{Param}, std::move(Body));
}

std::unique_ptr<ColumnsExprAST> Parser::ParseColumnsExpression() {
	if(!CurrentToken() || CurrentToken()->Value != "(")
		ParseFail("Expected '(' after COLUMNS");
	AdvanceToken();
	auto Out = std::make_unique<ColumnsExprAST>();
	if(CurrentToken() && CurrentToken()->Value == "*") {
		AdvanceToken();
		Out->Mode = ColumnsPickMode::All;
	} else if(auto Lam = TryParseLambdaExpression()) {
		Out->Mode = ColumnsPickMode::Lambda;
		Out->Lambda = std::move(Lam);
	} else {
		auto Lt = CurrentToken();
		if(!Lt || Lt->Type != TokenType::LITERAL)
			ParseFail("COLUMNS expects *, a string pattern, or a lambda (param -> predicate)");
		Out->Mode = ColumnsPickMode::Glob;
		Out->GlobPattern = Lt->Value;
		AdvanceToken();
	}
	if(!CurrentToken() || CurrentToken()->Value != ")")
		ParseFail("Expected ')' after COLUMNS(...)");
	AdvanceToken();
	return Out;
}

std::unique_ptr<ExpressionAST> Parser::ParseScalarConcat() {
	std::vector<std::unique_ptr<ExpressionAST>> Parts;
	Parts.push_back(ParseScalarPrimary());
	while(CurrentToken() && CurrentToken()->Value == "||") {
		AdvanceToken();
		Parts.push_back(ParseScalarPrimary());
	}
	if(Parts.size() == 1)
		return std::move(Parts[0]);
	return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::ConcatVariadic, std::move(Parts));
}

std::unique_ptr<ExpressionAST> Parser::ParseScalarMulDiv() {
	auto LHS = ParseScalarConcat();
	while(auto Nxt = CurrentToken()) {
		if(Nxt->Value != "*" && Nxt->Value != "/" && Nxt->Value != "//" && Nxt->Value != "%")
			break;
		std::string Op = Nxt->Value;
		AdvanceToken();
		auto RHS = ParseScalarConcat();
		LHS = std::make_unique<BinaryOpAST>(std::move(LHS), std::move(Op), std::move(RHS));
	}
	return LHS;
}

std::unique_ptr<ExpressionAST> Parser::ParseScalarAddSub() {
	auto LHS = ParseScalarMulDiv();
	while(auto Nxt = CurrentToken()) {
		if(Nxt->Value != "+" && Nxt->Value != "-")
			break;
		std::string Op = Nxt->Value;
		AdvanceToken();
		auto RHS = ParseScalarMulDiv();
		LHS = std::make_unique<BinaryOpAST>(std::move(LHS), std::move(Op), std::move(RHS));
	}
	return LHS;
}

std::unique_ptr<ExpressionAST> Parser::ParseScalarPrimary() {
	if(MatchKeyword("NULL"))
		return std::make_unique<NullLiteralAST>();
	if(auto Lam = TryParseLambdaExpression())
		return Lam;
	if(CurrentToken() && CurrentToken()->Value == "(") {
		AdvanceToken();
		auto Inner = ParseScalarExpression();
		if(!CurrentToken() || CurrentToken()->Value != ")")
			ParseFail("Expected ')' closing scalar subexpression");
		AdvanceToken();
		return Inner;
	}
	if(MatchKeyword("COALESCE") || MatchKeyword("IFNULL") || MatchKeyword("NVL"))
		return ParseCoalesceCallExpression();
	if(MatchKeyword("NVL2"))
		return ParseNvl2Expression();
	if(MatchKeyword("DECODE"))
		return ParseDecodeExpression();
	if(MatchKeyword("NOW")) {
		if(CurrentToken() && CurrentToken()->Value == "(") {
			AdvanceToken();
			if(!CurrentToken() || CurrentToken()->Value != ")")
				ParseFail("NOW expects empty parentheses");
			AdvanceToken();
		}
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::Now, std::vector<std::unique_ptr<ExpressionAST>>{});
	}
	if(MatchKeyword("CURRENT_DATE"))
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::CurrentDate, std::vector<std::unique_ptr<ExpressionAST>>{});
	if(MatchKeyword("CURRENT_TIME"))
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::CurrentTime, std::vector<std::unique_ptr<ExpressionAST>>{});
	if(MatchKeyword("CURRENT_TIMESTAMP"))
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::CurrentTimestamp,
		                                           std::vector<std::unique_ptr<ExpressionAST>>{});
	if(MatchKeyword("CAST")) {
		if(!CurrentToken() || CurrentToken()->Value != "(")
			ParseFail("Expected '(' after CAST");
		AdvanceToken();
		auto Operand = ParseScalarExpression();
		if(!MatchKeyword("AS"))
			ParseFail("CAST requires AS <type>");
		const std::string DType = ParseDataType();
		const SqlCastTarget CT = ParseCastTargetFromDataType(DType);
		if(!CurrentToken() || CurrentToken()->Value != ")")
			ParseFail("Expected ')' after CAST type");
		AdvanceToken();
		std::string TypeSql;
		if(CT == SqlCastTarget::Advanced)
			TypeSql = DType;
		return std::make_unique<CastExprAST>(std::move(Operand), CT, std::move(TypeSql));
	}
	if(auto Sfb = TryParseScalarSqlBuiltinSelectExpr())
		return Sfb;
	auto T = CurrentToken();
	if(!T)
		ParseFail("Unexpected end of input in scalar expression");
	if(T->Type == TokenType::IDENTIFIER ||
	   (T->Type == TokenType::KEYWORD && T->Value != "TRUE" && T->Value != "FALSE")) {
		std::string N = T->Value;
		AdvanceToken();
		if(CurrentToken() && CurrentToken()->Value == ".") {
			AdvanceToken();
			auto Col = CurrentToken();
			if(!IsSqlSimpleNameToken(Col))
				ParseFail("Expected column name after '.' in scalar expression");
			std::string Cn = Col->Value;
			AdvanceToken();
			return std::make_unique<ColumnRefAST>(N + "." + Cn);
		}
		return std::make_unique<ColumnRefAST>(std::move(N));
	}
	if(T->Type == TokenType::LITERAL) {
		std::string Lit = T->Value;
		AdvanceToken();
		return std::make_unique<LiteralAST>(std::move(Lit));
	}
	ParseFail("Expected scalar expression (literal, column, or function call)");
}

std::unique_ptr<ExpressionAST> Parser::ParseScalarExpression() {
	std::unique_ptr<ExpressionAST> Node = ParseScalarAddSub();
	while(CurrentToken() && CurrentToken()->Value == "::") {
		AdvanceToken();
		const std::string DType = ParseDataType();
		const SqlCastTarget CT = ParseCastTargetFromDataType(DType);
		std::string TypeSql;
		if(CT == SqlCastTarget::Advanced)
			TypeSql = DType;
		Node = std::make_unique<CastExprAST>(std::move(Node), CT, std::move(TypeSql));
	}
	return Node;
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
		Args.push_back(ParseScalarExpression());
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
		ParseFail("COALESCE / IFNULL / NVL require at least two arguments");
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

std::unique_ptr<Nvl2ExprAST> Parser::ParseNvl2Expression() {
	if(!CurrentToken() || CurrentToken()->Value != "(")
		ParseFail("Expected '(' after NVL2");
	AdvanceToken();
	auto Subj = ParseScalarExpression();
	if(!CurrentToken() || CurrentToken()->Value != ",")
		ParseFail("NVL2 expects three comma-separated arguments");
	AdvanceToken();
	auto NotNullVal = ParseScalarExpression();
	if(!CurrentToken() || CurrentToken()->Value != ",")
		ParseFail("NVL2 expects three comma-separated arguments");
	AdvanceToken();
	auto NullVal = ParseScalarExpression();
	if(!CurrentToken() || CurrentToken()->Value != ")")
		ParseFail("Expected ')' after NVL2 arguments");
	AdvanceToken();
	return std::make_unique<Nvl2ExprAST>(std::move(Subj), std::move(NotNullVal), std::move(NullVal));
}

std::unique_ptr<CaseExprAST> Parser::ParseDecodeExpression() {
	if(!CurrentToken() || CurrentToken()->Value != "(")
		ParseFail("Expected '(' after DECODE");
	AdvanceToken();
	auto Subj = ParseScalarExpression();
	std::vector<CaseExprAST::Arm> Arms;
	std::unique_ptr<ExpressionAST> ElseE;
	for(;;) {
		if(CurrentToken() && CurrentToken()->Value == ")") {
			AdvanceToken();
			break;
		}
		if(!CurrentToken() || CurrentToken()->Value == ",")
			ParseFail("DECODE expects search/result pairs or a closing ')'");
		auto Search = ParseScalarExpression();
		if(CurrentToken() && CurrentToken()->Value == ")") {
			ElseE = std::move(Search);
			AdvanceToken();
			break;
		}
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail("DECODE expects ',' between search and result expressions");
		AdvanceToken();
		auto Result = ParseScalarExpression();
		auto SubjL = CloneCaseScalarExpr(Subj.get());
		auto SearchL = CloneCaseScalarExpr(Search.get());
		if(!SubjL || !SearchL)
			ParseFail("DECODE search key must be a literal, NULL, or plain column reference");
		CaseExprAST::Arm A;
		A.When = std::make_unique<BinaryOpAST>(std::move(SubjL), "=", std::move(SearchL));
		A.Then = std::move(Result);
		Arms.push_back(std::move(A));
		if(CurrentToken() && CurrentToken()->Value == ",")
			AdvanceToken();
	}
	if(Arms.empty() && !ElseE)
		ParseFail("DECODE requires at least one WHEN pair or a default value");
	return std::make_unique<CaseExprAST>(std::move(Arms), std::move(ElseE));
}

std::unique_ptr<ScalarFuncExprAST> Parser::TryParseConcatProjection() {
	const size_t Save = CurrentIndex_;
	auto First = ParseCaseScalarResult();
	if(!CurrentToken() || CurrentToken()->Value != "||") {
		CurrentIndex_ = Save;
		return nullptr;
	}
	std::vector<std::unique_ptr<ExpressionAST>> Parts;
	Parts.push_back(std::move(First));
	while(CurrentToken() && CurrentToken()->Value == "||") {
		AdvanceToken();
		Parts.push_back(ParseCaseScalarResult());
	}
	return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::ConcatVariadic, std::move(Parts));
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
	static const std::unordered_set<std::string> Starters = {"SUBSTRING", "UPPER", "LOWER", "LENGTH", "CHAR_LENGTH",
	    "CHARACTER_LENGTH", "POSITION", "TRIM", "CONCAT", "EXTRACT", "DATE_ADD", "DATE_SUB", "DATE_DIFF",
	    "DATE_TRUNC", "TIME_BUCKET", "TIMESTAMP_DIFF", "CURRENT_TIMESTAMP", "AT_TIME_ZONE", "CONVERT_TZ",
	    "STRUCT_FIELD", "MAP_GET", "COMPLEX_REAL", "COMPLEX_IMAG",
	    "COMPLEX_MUL", "VECTOR_DOT", "VECTOR_ADD", "VECTOR_NORM", "MATRIX_VEC"};
	if(Name == "LIST_TRANSFORM") {
		AdvanceToken();
		if(!CurrentToken() || CurrentToken()->Value != "(")
			ParseFail("Expected '(' after LIST_TRANSFORM");
		AdvanceToken();
		std::vector<std::unique_ptr<ExpressionAST>> Args;
		Args.push_back(ParseScalarExpression());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail("LIST_TRANSFORM expects (list, param -> expr)");
		AdvanceToken();
		auto Lam = TryParseLambdaExpression();
		if(!Lam || Lam->Params.size() != 1)
			ParseFail("LIST_TRANSFORM second argument must be a unary lambda (x -> ...)");
		Args.push_back(std::unique_ptr<ExpressionAST>(static_cast<ExpressionAST *>(Lam.release())));
		if(!CurrentToken() || CurrentToken()->Value != ")")
			ParseFail("Expected ')' after LIST_TRANSFORM arguments");
		AdvanceToken();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::ListTransform, std::move(Args));
	}
	if(const auto Builtin = MathSci::LookupBuiltin(Name)) {
		AdvanceToken();
		if(!CurrentToken() || CurrentToken()->Value != "(")
			ParseFail("Expected '(' after " + Name);
		AdvanceToken();
		std::vector<std::unique_ptr<ExpressionAST>> Args;
		if(Builtin->Arity.Max > 0) {
			for(;;) {
				Args.push_back(ParseScalarExpression());
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

	if(Name == "NOW") {
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::Now, std::move(Args));
	}
	if(Name == "CURRENT_TIMESTAMP") {
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::CurrentTimestamp, std::move(Args));
	}

	if(Name == "UPPER" || Name == "LOWER" || Name == "LENGTH" || Name == "CHAR_LENGTH" ||
	    Name == "CHARACTER_LENGTH") {
		Args.push_back(ParseScalarExpression());
		ScalarSqlFn K = ScalarSqlFn::Upper;
		if(Name == "LOWER")
			K = ScalarSqlFn::Lower;
		else if(Name == "LENGTH" || Name == "CHAR_LENGTH" || Name == "CHARACTER_LENGTH")
			K = ScalarSqlFn::CharLength;
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(K, std::move(Args));
	}
	if(Name == "SUBSTRING") {
		Args.push_back(ParseScalarExpression());
		if(MatchKeyword("FROM")) {
			Args.push_back(ParseScalarExpression());
			if(MatchKeyword("FOR"))
				Args.push_back(ParseScalarExpression());
		} else {
			if(!CurrentToken() || CurrentToken()->Value != ",")
				ParseFail("SUBSTRING expects FROM … FOR … or comma-separated arguments");
			AdvanceToken();
			Args.push_back(ParseScalarExpression());
			if(!CurrentToken() || CurrentToken()->Value != ",")
				ParseFail("SUBSTRING comma form requires three arguments");
			AdvanceToken();
			Args.push_back(ParseScalarExpression());
		}
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::SubstringFromFor, std::move(Args));
	}
	if(Name == "POSITION") {
		Args.push_back(ParseScalarExpression());
		if(!MatchKeyword("IN"))
			ParseFail("POSITION requires IN between search string and source string");
		Args.push_back(ParseScalarExpression());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::PositionIn, std::move(Args));
	}
	if(Name == "TRIM") {
		ScalarSqlFn Tk = ScalarSqlFn::TrimBoth;
		if(MatchKeyword("LEADING")) {
			Tk = ScalarSqlFn::TrimLeading;
			if(!MatchKeyword("FROM"))
				ParseFail("TRIM LEADING requires FROM");
			Args.push_back(ParseScalarExpression());
		} else if(MatchKeyword("TRAILING")) {
			Tk = ScalarSqlFn::TrimTrailing;
			if(!MatchKeyword("FROM"))
				ParseFail("TRIM TRAILING requires FROM");
			Args.push_back(ParseScalarExpression());
		} else if(MatchKeyword("BOTH")) {
			if(!MatchKeyword("FROM"))
				ParseFail("TRIM BOTH requires FROM");
			Args.push_back(ParseScalarExpression());
		} else
			Args.push_back(ParseScalarExpression());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(Tk, std::move(Args));
	}
	if(Name == "CONCAT") {
		Args.push_back(ParseScalarExpression());
		while(CurrentToken() && CurrentToken()->Value == ",") {
			AdvanceToken();
			Args.push_back(ParseScalarExpression());
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
		Args.push_back(ParseScalarExpression());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(Field, std::move(Args));
	}
	if(Name == "DATE_ADD") {
		Args.push_back(ParseScalarExpression());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail("DATE_ADD expects two arguments");
		AdvanceToken();
		Args.push_back(ParseScalarExpression());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::DateAddDays, std::move(Args));
	}
	if(Name == "DATE_SUB") {
		Args.push_back(ParseScalarExpression());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail("DATE_SUB expects two arguments");
		AdvanceToken();
		Args.push_back(ParseScalarExpression());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::DateSubDays, std::move(Args));
	}
	if(Name == "DATE_DIFF") {
		Args.push_back(ParseScalarExpression());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail("DATE_DIFF expects two arguments");
		AdvanceToken();
		Args.push_back(ParseScalarExpression());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::DateDiffDays, std::move(Args));
	}
	if(Name == "DATE_TRUNC") {
		Args.push_back(ParseScalarExpression());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail("DATE_TRUNC expects unit and timestamp arguments");
		AdvanceToken();
		Args.push_back(ParseScalarExpression());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::DateTrunc, std::move(Args));
	}
	if(Name == "TIME_BUCKET") {
		Args.push_back(ParseScalarExpression());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail("TIME_BUCKET expects timestamp and bucket width in seconds");
		AdvanceToken();
		Args.push_back(ParseScalarExpression());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::TimeBucketSeconds, std::move(Args));
	}
	if(Name == "TIMESTAMP_DIFF") {
		Args.push_back(ParseScalarExpression());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail("TIMESTAMP_DIFF expects two timestamp arguments");
		AdvanceToken();
		Args.push_back(ParseScalarExpression());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::TimestampDiffSeconds, std::move(Args));
	}
	if(Name == "AT_TIME_ZONE") {
		Args.push_back(ParseScalarExpression());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail("AT_TIME_ZONE expects timestamp and timezone offset");
		AdvanceToken();
		Args.push_back(ParseScalarExpression());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::AtTimeZone, std::move(Args));
	}
	if(Name == "CONVERT_TZ") {
		Args.push_back(ParseScalarExpression());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail("CONVERT_TZ expects timestamp, from_tz, to_tz");
		AdvanceToken();
		Args.push_back(ParseScalarExpression());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail("CONVERT_TZ expects timestamp, from_tz, to_tz");
		AdvanceToken();
		Args.push_back(ParseScalarExpression());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::ConvertTimezone, std::move(Args));
	}
	if(Name == "STRUCT_FIELD") {
		Args.push_back(ParseScalarExpression());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail("STRUCT_FIELD expects struct value and field name");
		AdvanceToken();
		Args.push_back(ParseScalarExpression());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::StructField, std::move(Args));
	}
	if(Name == "MAP_GET") {
		Args.push_back(ParseScalarExpression());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail("MAP_GET expects map value and key");
		AdvanceToken();
		Args.push_back(ParseScalarExpression());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::MapGet, std::move(Args));
	}
	if(Name == "COMPLEX_REAL" || Name == "COMPLEX_IMAG") {
		Args.push_back(ParseScalarExpression());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(
		    Name == "COMPLEX_REAL" ? ScalarSqlFn::ComplexReal : ScalarSqlFn::ComplexImag, std::move(Args));
	}
	if(Name == "COMPLEX_MUL") {
		Args.push_back(ParseScalarExpression());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail("COMPLEX_MUL expects two complex values");
		AdvanceToken();
		Args.push_back(ParseScalarExpression());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::ComplexMul, std::move(Args));
	}
	if(Name == "VECTOR_DOT" || Name == "VECTOR_ADD") {
		Args.push_back(ParseScalarExpression());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail(Name + " expects two vector arguments");
		AdvanceToken();
		Args.push_back(ParseScalarExpression());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(
		    Name == "VECTOR_DOT" ? ScalarSqlFn::VectorDot : ScalarSqlFn::VectorAdd, std::move(Args));
	}
	if(Name == "VECTOR_NORM") {
		Args.push_back(ParseScalarExpression());
		FinishClose();
		return std::make_unique<ScalarFuncExprAST>(ScalarSqlFn::VectorNorm, std::move(Args));
	}
	if(Name == "MATRIX_VEC") {
		Args.push_back(ParseScalarExpression());
		if(!CurrentToken() || CurrentToken()->Value != ",")
			ParseFail("MATRIX_VEC expects matrix and vector arguments");
		AdvanceToken();
		Args.push_back(ParseScalarExpression());
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

    std::vector<OrderBySpec> OrderByColumns;
    if(auto NextToken = CurrentToken(); NextToken && NextToken->Value == "ORDER") {
        AdvanceToken();
        if(auto ByToken = CurrentToken(); ByToken && ByToken->Value == "BY") {
            AdvanceToken();
            while(auto ColumnToken = CurrentToken()) {
                std::string ColumnName = ColumnToken->Value;
                AdvanceToken();
                OrderBySpec Spec;
                Spec.Column = std::move(ColumnName);
                Spec.Ascending = true;
                Spec.NullsFirst = false;
                if(auto OrderToken = CurrentToken(); OrderToken) {
                    if(OrderToken->Value == "ASC") {
                        Spec.Ascending = true;
                        AdvanceToken();
                    } else if(OrderToken->Value == "DESC") {
                        Spec.Ascending = false;
                        AdvanceToken();
                    }
                }
                if(auto NullTok = CurrentToken(); NullTok && NullTok->Value == "NULLS") {
                    AdvanceToken();
                    if(MatchKeyword("FIRST"))
                        Spec.NullsFirst = true;
                    else if(MatchKeyword("LAST"))
                        Spec.NullsFirst = false;
                    else
                        ParseFail("Expected FIRST or LAST after NULLS");
                }
                OrderByColumns.push_back(std::move(Spec));
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

ASTNode Parser::ParseInsertStatement(bool SqliteReplace) {
    if(!SqliteReplace) {
        AdvanceToken();
        if(!MatchKeyword("INTO"))
            ParseFail("Expected INTO after INSERT");
    }
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

    std::vector<std::vector<std::unique_ptr<ExpressionAST>>> AllValues;

    while(CurrentToken() && CurrentToken()->Value == "(") {
        AdvanceToken(); // Consume '('
        std::vector<std::unique_ptr<ExpressionAST>> CurrentValues;
        while(auto TokenOpt = CurrentToken()) {
            if(TokenOpt->Value == ")") {
                AdvanceToken(); // Consume ')'
                break;
            }
            if(MatchKeyword("NEXTVAL")) {
                if(!CurrentToken() || CurrentToken()->Value != "(")
                    ParseFail("Expected '(' after NEXTVAL");
                AdvanceToken();
                auto SeqTok = CurrentToken();
                if(!SeqTok || SeqTok->Type != TokenType::IDENTIFIER)
                    ParseFail("Expected sequence name in NEXTVAL(...)");
                std::string Val = std::string("__astral_nextval__:") + SeqTok->Value;
                AdvanceToken();
                if(!CurrentToken() || CurrentToken()->Value != ")")
                    ParseFail("Expected ')' after NEXTVAL sequence name");
                AdvanceToken();
                CurrentValues.push_back(std::make_unique<LiteralAST>(std::move(Val)));
            } else {
                CurrentValues.push_back(ParseScalarExpression());
            }
            if(CurrentToken() && CurrentToken()->Value == ",") {
                AdvanceToken(); // Consume ',' between values within a set
            }
        }
        AllValues.push_back(std::move(CurrentValues));

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
    if(SqliteReplace) {
        UpsertSpec Us;
        Us.Mode = UpsertSpec::OnConflict::Update;
        Us.SqliteReplace = true;
        if(Columns.empty())
            Us.SqliteReplaceImplicitSchema = true;
        UpsertOpt = std::move(Us);
    }
    if(MatchKeyword("ON")) {
        if(SqliteReplace)
            ParseFail("REPLACE INTO cannot be combined with ON CONFLICT");
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
                if(TokenOpt->Type == TokenType::KEYWORD && TokenOpt->Value == "RETURNING")
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

	bool HasReturning = false;
	bool ReturningAll = false;
	std::vector<std::string> ReturningColumns;
	if(MatchKeyword("RETURNING")) {
		HasReturning = true;
		if(CurrentToken() && CurrentToken()->Type == TokenType::PUNCTUATION && CurrentToken()->Value == "*") {
			ReturningAll = true;
			AdvanceToken();
		} else {
			while(auto Tok = CurrentToken()) {
				if(Tok->Type == TokenType::PUNCTUATION && Tok->Value == ";")
					break;
				if(Tok->Type == TokenType::PUNCTUATION && Tok->Value == ",") {
					AdvanceToken();
					continue;
				}
				if(Tok->Type == TokenType::IDENTIFIER || Tok->Type == TokenType::KEYWORD) {
					ReturningColumns.push_back(Tok->Value);
					AdvanceToken();
					continue;
				}
				ParseFail("RETURNING expects '*' or a comma-separated column list.");
			}
			if(ReturningColumns.empty())
				ParseFail("RETURNING column list must not be empty.");
		}
	}

    auto TableAst = std::make_unique<TableAST>(TableName);
    return std::make_unique<InsertAST>(std::move(TableAst), std::move(Columns), std::move(AllValues),
                                       std::move(UpsertOpt), HasReturning, ReturningAll, std::move(ReturningColumns));
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
	bool HasReturning = false;
	bool ReturningAll = false;
	std::vector<std::string> ReturningColumns;
	if(MatchKeyword("RETURNING")) {
		HasReturning = true;
		if(CurrentToken() && CurrentToken()->Type == TokenType::PUNCTUATION && CurrentToken()->Value == "*") {
			ReturningAll = true;
			AdvanceToken();
		} else {
			while(auto Tok = CurrentToken()) {
				if(Tok->Type == TokenType::PUNCTUATION && Tok->Value == ";")
					break;
				if(Tok->Type == TokenType::PUNCTUATION && Tok->Value == ",") {
					AdvanceToken();
					continue;
				}
				if(Tok->Type == TokenType::IDENTIFIER || Tok->Type == TokenType::KEYWORD) {
					ReturningColumns.push_back(Tok->Value);
					AdvanceToken();
					continue;
				}
				ParseFail("RETURNING expects '*' or a comma-separated column list.");
			}
			if(ReturningColumns.empty())
				ParseFail("RETURNING column list must not be empty.");
		}
	}
    return std::make_unique<UpdateAST>(TableName, std::move(Assignments), std::move(Condition), HasReturning,
        ReturningAll, std::move(ReturningColumns));
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
	bool HasReturning = false;
	bool ReturningAll = false;
	std::vector<std::string> ReturningColumns;
	if(MatchKeyword("RETURNING")) {
		HasReturning = true;
		if(CurrentToken() && CurrentToken()->Type == TokenType::PUNCTUATION && CurrentToken()->Value == "*") {
			ReturningAll = true;
			AdvanceToken();
		} else {
			while(auto Tok = CurrentToken()) {
				if(Tok->Type == TokenType::PUNCTUATION && Tok->Value == ";")
					break;
				if(Tok->Type == TokenType::PUNCTUATION && Tok->Value == ",") {
					AdvanceToken();
					continue;
				}
				if(Tok->Type == TokenType::IDENTIFIER || Tok->Type == TokenType::KEYWORD) {
					ReturningColumns.push_back(Tok->Value);
					AdvanceToken();
					continue;
				}
				ParseFail("RETURNING expects '*' or a comma-separated column list.");
			}
			if(ReturningColumns.empty())
				ParseFail("RETURNING column list must not be empty.");
		}
	}
    return std::make_unique<DeleteAST>(TableName, std::move(Condition), HasReturning, ReturningAll,
        std::move(ReturningColumns));
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

std::string Parser::ParseQualifiedSqlName() {
	auto Id1 = CurrentToken();
	if(!IsSqlSimpleNameToken(Id1))
		ParseFail("Expected column name");
	AdvanceToken();
	if(CurrentToken() && CurrentToken()->Value == ".") {
		AdvanceToken();
		auto Id2 = CurrentToken();
		if(!IsSqlSimpleNameToken(Id2))
			ParseFail("Expected column after '.'");
		AdvanceToken();
		return Id2->Value;
	}
	return Id1->Value;
}

bool Parser::TryParseWindowAggArgument(std::string &OutSourceColumn) {
	auto Tok = CurrentToken();
	if(!Tok)
		return false;
	const bool LooksLikeSum =
	    (Tok->Type == TokenType::KEYWORD && Tok->Value == "SUM") ||
	    (Tok->Type == TokenType::IDENTIFIER && Tok->Value == "SUM");
	const bool LooksLikeMin =
	    (Tok->Type == TokenType::KEYWORD && Tok->Value == "MIN") ||
	    (Tok->Type == TokenType::IDENTIFIER && Tok->Value == "MIN");
	const bool LooksLikeMax =
	    (Tok->Type == TokenType::KEYWORD && Tok->Value == "MAX") ||
	    (Tok->Type == TokenType::IDENTIFIER && Tok->Value == "MAX");
	const bool LooksLikeAvg =
	    (Tok->Type == TokenType::KEYWORD && Tok->Value == "AVG") ||
	    (Tok->Type == TokenType::IDENTIFIER && Tok->Value == "AVG");
	const bool IsAgg = LooksLikeSum || LooksLikeMin || LooksLikeMax || LooksLikeAvg;
	if(!IsAgg)
		return false;
	AdvanceToken();
	if(!CurrentToken() || CurrentToken()->Value != "(")
		return false;
	AdvanceToken();
	OutSourceColumn = ParseQualifiedSqlName();
	if(!CurrentToken() || CurrentToken()->Value != ")")
		ParseFail("Expected ')' after window aggregate argument");
	AdvanceToken();
	return true;
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
            const std::string Pname = ParseQualifiedSqlName();
            if(PartSeen.count(Pname))
                ParseFail("Duplicate column \"" + Pname + "\" in PARTITION BY");
            if(Ws.PartitionBy.size() >= Limits::MaxWindowPartitionColumns)
                ParseFail(
                    "PARTITION BY column list exceeds the configured maximum "
                    "(see Limits::MaxWindowPartitionColumns)");
            PartSeen.insert(Pname);
            Ws.PartitionBy.push_back(Pname);
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
    if(!TryParseWindowAggArgument(Ws.OrderColumn))
        Ws.OrderColumn = ParseQualifiedSqlName();
    Ws.OrderAscending = true;
    if(auto Ot = CurrentToken(); Ot && Ot->Value == "ASC") {
        Ws.OrderAscending = true;
        AdvanceToken();
    } else if(Ot && Ot->Value == "DESC") {
        Ws.OrderAscending = false;
        AdvanceToken();
    }
    auto ParseFrameBound = [&]() -> WindowFrameBound {
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
    if(MatchKeyword("ROWS")) {
        Ws.FrameUnit = WindowFrameUnit::Rows;
        if(MatchKeyword("BETWEEN")) {
            Ws.FrameStart = ParseFrameBound();
            if(!MatchKeyword("AND"))
                ParseFail("Expected AND between window frame bounds");
            Ws.FrameEnd = ParseFrameBound();
        } else {
            /** Shorthand: \c ROWS UNBOUNDED PRECEDING ≡ BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW. */
            Ws.FrameStart = ParseFrameBound();
            if(CurrentToken() && CurrentToken()->Value == "AND") {
                AdvanceToken();
                Ws.FrameEnd = ParseFrameBound();
            } else
                Ws.FrameEnd = WindowFrameBound{WindowFrameBoundKind::CurrentRow, 0};
        }
        Ws.HasExplicitFrame = true;
    } else if(MatchKeyword("RANGE")) {
        if(!MatchKeyword("BETWEEN"))
            ParseFail("Expected BETWEEN after RANGE in window frame");
        Ws.FrameUnit = WindowFrameUnit::Range;
        Ws.FrameStart = ParseFrameBound();
        if(!MatchKeyword("AND"))
            ParseFail("Expected AND between window frame bounds");
        Ws.FrameEnd = ParseFrameBound();
        Ws.HasExplicitFrame = true;
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

ASTNode Parser::ParseInSubqueryPredicate(bool Negated, std::unique_ptr<ExpressionAST> Lhs) {
	std::string LhsCol;
	if(const auto *Cr = dynamic_cast<const ColumnRefAST *>(Lhs.get()))
		LhsCol = Cr->Name;
	else
		ParseFail("IN subquery requires a plain column reference on the left");
	std::string InnerCol;
	if(CurrentToken() && CurrentToken()->Value == "*") {
		AdvanceToken();
	} else if(auto ColTok = CurrentToken();
	          ColTok && (ColTok->Type == TokenType::IDENTIFIER || ColTok->Type == TokenType::KEYWORD)) {
		InnerCol = ColTok->Value;
		AdvanceToken();
	} else {
		ParseFail("IN subquery expects SELECT column or * FROM table");
	}
	if(!MatchKeyword("FROM"))
		ParseFail("IN subquery requires SELECT ... FROM table");
	auto TTok = CurrentToken();
	if(!TTok)
		ParseFail("IN subquery expects table name after FROM");
	if(TTok->Type != TokenType::IDENTIFIER && TTok->Type != TokenType::KEYWORD)
		ParseFail("IN subquery FROM expects a simple table identifier");
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
		ParseFail("Expected ')' closing IN subquery");
	AdvanceToken();
	return std::make_unique<InSubqueryPredAST>(Negated, std::move(LhsCol), std::move(Tbl), std::move(InnerCol),
	                                           std::move(Where));
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

std::unique_ptr<StatementAST> Parser::ParseLoadStatement() {
	AdvanceToken();
	if(!MatchKeyword("DATASET"))
		ParseFail("LOAD expects DATASET.");
	auto Nt = CurrentToken();
	if(!Nt || Nt->Type != TokenType::IDENTIFIER)
		ParseFail("Expected dataset name after LOAD DATASET.");
	std::string DsName = Nt->Value;
	AdvanceToken();
	int64_t VersionId = 0;
	if(MatchKeyword("VERSION")) {
		auto Vt = CurrentToken();
		if(!Vt || Vt->Type != TokenType::LITERAL)
			ParseFail("Expected integer after LOAD DATASET VERSION.");
		VersionId = std::stoll(Vt->Value);
		AdvanceToken();
	}
	if(!MatchKeyword("INTO"))
		ParseFail("LOAD DATASET requires INTO table_name.");
	auto Tt = CurrentToken();
	if(!Tt || Tt->Type != TokenType::IDENTIFIER)
		ParseFail("Expected table name after LOAD DATASET INTO.");
	std::string Target = Tt->Value;
	AdvanceToken();
	if(VersionId == 0 && MatchKeyword("VERSION")) {
		auto Vt = CurrentToken();
		if(!Vt || Vt->Type != TokenType::LITERAL)
			ParseFail("Expected integer after LOAD DATASET VERSION.");
		VersionId = std::stoll(Vt->Value);
		AdvanceToken();
	}
	return std::make_unique<LoadDatasetAST>(std::move(DsName), std::move(Target), VersionId);
}

std::unique_ptr<StatementAST> Parser::ParseVacuumStatement() {
	AdvanceToken();
	std::string Table;
	if(MatchKeyword("TABLE")) {
		auto Tt = CurrentToken();
		if(!Tt || Tt->Type != TokenType::IDENTIFIER)
			ParseFail("Expected table name after VACUUM TABLE.");
		Table = Tt->Value;
		AdvanceToken();
	}
	return std::make_unique<VacuumAST>(std::move(Table));
}

std::unique_ptr<StatementAST> Parser::ParseRepackStatement() {
	AdvanceToken();
	if(!MatchKeyword("TABLE"))
		ParseFail("REPACK requires TABLE.");
	auto Tt = CurrentToken();
	if(!Tt || Tt->Type != TokenType::IDENTIFIER)
		ParseFail("Expected table name after REPACK TABLE.");
	std::string Table = Tt->Value;
	AdvanceToken();
	if(!MatchKeyword("CONCURRENTLY"))
		ParseFail("REPACK TABLE requires CONCURRENTLY.");
	return std::make_unique<RepackConcurrentlyAST>(std::move(Table));
}

std::unique_ptr<StatementAST> Parser::ParseCreateGraphStatement() {
	auto Gn = CurrentToken();
	if(!Gn || Gn->Type != TokenType::IDENTIFIER)
		ParseFail("Expected graph name after CREATE GRAPH.");
	std::string GraphName = Gn->Value;
	AdvanceToken();
	if(!MatchKeyword("VERTEX"))
		ParseFail("CREATE GRAPH requires VERTEX TABLE … EDGE TABLE …");
	if(!MatchKeyword("TABLE"))
		ParseFail("CREATE GRAPH requires VERTEX TABLE …");
	auto Vt = CurrentToken();
	if(!Vt || Vt->Type != TokenType::IDENTIFIER)
		ParseFail("Expected vertex table name.");
	std::string VertexTable = Vt->Value;
	AdvanceToken();
	if(!MatchToken(TokenType::PUNCTUATION, "("))
		ParseFail("Expected '(' after vertex table name.");
	auto Vid = CurrentToken();
	if(!Vid || Vid->Type != TokenType::IDENTIFIER)
		ParseFail("Expected vertex id column.");
	std::string VertexIdCol = Vid->Value;
	AdvanceToken();
	if(!MatchToken(TokenType::PUNCTUATION, ")"))
		ParseFail("Expected ')' after vertex id column.");
	if(!MatchKeyword("EDGE"))
		ParseFail("CREATE GRAPH requires EDGE TABLE …");
	if(!MatchKeyword("TABLE"))
		ParseFail("CREATE GRAPH requires EDGE TABLE …");
	auto Et = CurrentToken();
	if(!Et || Et->Type != TokenType::IDENTIFIER)
		ParseFail("Expected edge table name.");
	std::string EdgeTable = Et->Value;
	AdvanceToken();
	if(!MatchToken(TokenType::PUNCTUATION, "("))
		ParseFail("Expected '(' after edge table name.");
	auto Src = CurrentToken();
	if(!Src || Src->Type != TokenType::IDENTIFIER)
		ParseFail("Expected edge source column.");
	std::string EdgeSrcCol = Src->Value;
	AdvanceToken();
	if(!MatchToken(TokenType::PUNCTUATION, ","))
		ParseFail("Expected ',' between edge columns.");
	auto Dst = CurrentToken();
	if(!Dst || Dst->Type != TokenType::IDENTIFIER)
		ParseFail("Expected edge destination column.");
	std::string EdgeDstCol = Dst->Value;
	AdvanceToken();
	std::string EdgeLabelCol;
	if(MatchToken(TokenType::PUNCTUATION, ",")) {
		auto Lbl = CurrentToken();
		if(!Lbl || Lbl->Type != TokenType::IDENTIFIER)
			ParseFail("Expected edge label column.");
		EdgeLabelCol = Lbl->Value;
		AdvanceToken();
	}
	if(!MatchToken(TokenType::PUNCTUATION, ")"))
		ParseFail("Expected ')' after edge columns.");
	std::string EdgeWeightCol;
	bool Undirected = false;
	if(MatchKeyword("UNDIRECTED"))
		Undirected = true;
	if(MatchKeyword("WEIGHT")) {
		if(!MatchToken(TokenType::PUNCTUATION, "("))
			ParseFail("CREATE GRAPH WEIGHT expects '(' column name ')'");
		auto Wt = CurrentToken();
		if(!Wt || Wt->Type != TokenType::IDENTIFIER)
			ParseFail("Expected weight column name.");
		EdgeWeightCol = Wt->Value;
		AdvanceToken();
		if(!MatchToken(TokenType::PUNCTUATION, ")"))
			ParseFail("Expected ')' after weight column.");
	}
	return std::make_unique<CreateGraphAST>(std::move(GraphName), std::move(VertexTable), std::move(VertexIdCol),
	                                        std::move(EdgeTable), std::move(EdgeSrcCol), std::move(EdgeDstCol),
	                                        std::move(EdgeLabelCol), std::move(EdgeWeightCol), Undirected);
}

std::unique_ptr<StatementAST> Parser::ParseCreateGraphProjectionStatement() {
	auto Pn = CurrentToken();
	if(!Pn || Pn->Type != TokenType::IDENTIFIER)
		ParseFail("Expected projection name after CREATE GRAPH PROJECTION.");
	std::string ProjName = Pn->Value;
	AdvanceToken();
	if(!MatchKeyword("FROM"))
		ParseFail("CREATE GRAPH PROJECTION requires FROM base_graph.");
	auto Bn = CurrentToken();
	if(!Bn || Bn->Type != TokenType::IDENTIFIER)
		ParseFail("Expected base graph name after FROM.");
	std::string BaseName = Bn->Value;
	AdvanceToken();
	if(!MatchKeyword("EDGE"))
		ParseFail("CREATE GRAPH PROJECTION requires EDGE WHERE …");
	if(!MatchKeyword("WHERE"))
		ParseFail("CREATE GRAPH PROJECTION requires EDGE WHERE edge.label = 'value'.");
	auto Ev = CurrentToken();
	if(!Ev || Ev->Type != TokenType::IDENTIFIER)
		ParseFail("CREATE GRAPH PROJECTION WHERE expects edge variable.");
	AdvanceToken();
	if(!MatchToken(TokenType::PUNCTUATION, "."))
		ParseFail("CREATE GRAPH PROJECTION WHERE expects edge.label = 'value'.");
	auto Lc = CurrentToken();
	if(!Lc || Lc->Type != TokenType::IDENTIFIER)
		ParseFail("CREATE GRAPH PROJECTION WHERE expects label column.");
	AdvanceToken();
	if(!MatchToken(TokenType::PUNCTUATION, "="))
		ParseFail("CREATE GRAPH PROJECTION WHERE expects '='.");
	auto Lit = CurrentToken();
	if(!Lit || Lit->Type != TokenType::LITERAL)
		ParseFail("CREATE GRAPH PROJECTION WHERE expects string literal.");
	std::string Filter = Lit->Value;
	AdvanceToken();
	return std::make_unique<CreateGraphProjectionAST>(std::move(ProjName), std::move(BaseName), std::move(Filter));
}

std::unique_ptr<StatementAST> Parser::ParseGraphStatement() {
	AdvanceToken();
	if(MatchKeyword("TRAVERSE")) {
		if(!MatchKeyword("FROM"))
			ParseFail("GRAPH TRAVERSE requires FROM start vertex.");
		auto St = CurrentToken();
		if(!St || (St->Type != TokenType::LITERAL && St->Type != TokenType::IDENTIFIER))
			ParseFail("GRAPH TRAVERSE FROM expects a literal or identifier.");
		std::string StartId = St->Value;
		AdvanceToken();
		if(!MatchKeyword("IN"))
			ParseFail("GRAPH TRAVERSE requires IN graph_name.");
		auto Gn = CurrentToken();
		if(!Gn || Gn->Type != TokenType::IDENTIFIER)
			ParseFail("Expected graph name after IN.");
		std::string GraphName = Gn->Value;
		AdvanceToken();
		int64_t Depth = 1;
		if(MatchKeyword("DEPTH")) {
			auto Dt = CurrentToken();
			if(!Dt || Dt->Type != TokenType::LITERAL)
				ParseFail("GRAPH TRAVERSE DEPTH expects integer literal.");
			Depth = std::stoll(Dt->Value);
			AdvanceToken();
		}
		int64_t Mode = 0;
		if(MatchKeyword("DFS"))
			Mode = 1;
		else
			(void)MatchKeyword("BFS");
		if(!MatchKeyword("INTO"))
			ParseFail("GRAPH TRAVERSE requires INTO result_table.");
		auto Rt = CurrentToken();
		if(!Rt || Rt->Type != TokenType::IDENTIFIER)
			ParseFail("Expected result table name after INTO.");
		std::string Result = Rt->Value;
		AdvanceToken();
		return std::make_unique<GraphTraverseAST>(std::move(GraphName), std::move(StartId), Depth, Mode,
		                                          std::move(Result));
	}
	if(MatchKeyword("SHORTEST")) {
		if(!MatchKeyword("PATH"))
			ParseFail("GRAPH SHORTEST PATH requires PATH keyword.");
		if(!MatchKeyword("FROM"))
			ParseFail("GRAPH SHORTEST PATH requires FROM vertex.");
		auto Fr = CurrentToken();
		if(!Fr || (Fr->Type != TokenType::LITERAL && Fr->Type != TokenType::IDENTIFIER))
			ParseFail("GRAPH SHORTEST PATH FROM expects id literal.");
		std::string FromId = Fr->Value;
		AdvanceToken();
		if(!MatchKeyword("TO"))
			ParseFail("GRAPH SHORTEST PATH requires TO vertex.");
		auto To = CurrentToken();
		if(!To || (To->Type != TokenType::LITERAL && To->Type != TokenType::IDENTIFIER))
			ParseFail("GRAPH SHORTEST PATH TO expects id literal.");
		std::string ToId = To->Value;
		AdvanceToken();
		if(!MatchKeyword("IN"))
			ParseFail("GRAPH SHORTEST PATH requires IN graph_name.");
		auto Gn = CurrentToken();
		if(!Gn || Gn->Type != TokenType::IDENTIFIER)
			ParseFail("Expected graph name after IN.");
		std::string GraphName = Gn->Value;
		AdvanceToken();
		bool Weighted = MatchKeyword("WEIGHTED");
		if(!MatchKeyword("INTO"))
			ParseFail("GRAPH SHORTEST PATH requires INTO result_table.");
		auto Rt = CurrentToken();
		if(!Rt || Rt->Type != TokenType::IDENTIFIER)
			ParseFail("Expected result table name after INTO.");
		std::string Result = Rt->Value;
		AdvanceToken();
		return std::make_unique<GraphShortestPathAST>(std::move(GraphName), std::move(FromId), std::move(ToId),
		                                                Weighted, std::move(Result));
	}
	if(MatchKeyword("PAGERANK")) {
		if(!MatchKeyword("IN"))
			ParseFail("GRAPH PAGERANK requires IN graph_name.");
		auto Gn = CurrentToken();
		if(!Gn || Gn->Type != TokenType::IDENTIFIER)
			ParseFail("Expected graph name after IN.");
		std::string GraphName = Gn->Value;
		AdvanceToken();
		int64_t DampingMillis = 850;
		int64_t Iterations = 20;
		if(MatchKeyword("DAMPING")) {
			auto Dt = CurrentToken();
			if(!Dt || Dt->Type != TokenType::LITERAL)
				ParseFail("GRAPH PAGERANK DAMPING expects decimal literal (e.g. 0.85).");
			const double D = std::stod(Dt->Value);
			DampingMillis = static_cast<int64_t>(D * 1000.0);
			AdvanceToken();
		}
		if(MatchKeyword("ITERATIONS")) {
			auto It = CurrentToken();
			if(!It || It->Type != TokenType::LITERAL)
				ParseFail("GRAPH PAGERANK ITERATIONS expects integer literal.");
			Iterations = std::stoll(It->Value);
			AdvanceToken();
		}
		if(!MatchKeyword("INTO"))
			ParseFail("GRAPH PAGERANK requires INTO result_table.");
		auto Rt = CurrentToken();
		if(!Rt || Rt->Type != TokenType::IDENTIFIER)
			ParseFail("Expected result table name after INTO.");
		std::string Result = Rt->Value;
		AdvanceToken();
		return std::make_unique<GraphPageRankAST>(std::move(GraphName), DampingMillis, Iterations, std::move(Result));
	}
	if(MatchKeyword("MATCH")) {
		if(!MatchToken(TokenType::PUNCTUATION, "("))
			ParseFail("GRAPH MATCH expects '(' pattern.");
		auto A = CurrentToken();
		if(!A || A->Type != TokenType::IDENTIFIER)
			ParseFail("GRAPH MATCH pattern expects vertex variable.");
		(void)A;
		AdvanceToken();
		if(!MatchToken(TokenType::PUNCTUATION, ")"))
			ParseFail("GRAPH MATCH pattern: expected ')' after source vertex.");
		std::vector<GraphMatchPatternSegment> Segments;
		std::string LabelFilter;
		while(MatchToken(TokenType::PUNCTUATION, "-")) {
			bool Reverse = false;
			if(MatchToken(TokenType::PUNCTUATION, "<")) {
				if(!MatchToken(TokenType::PUNCTUATION, "-"))
					ParseFail("GRAPH MATCH pattern: expected '<-['.");
				Reverse = true;
			}
			if(!MatchToken(TokenType::PUNCTUATION, "["))
				ParseFail("GRAPH MATCH pattern: expected '['.");
			if(MatchToken(TokenType::PUNCTUATION, ":")) {
				auto Lbl = CurrentToken();
				if(!Lbl || Lbl->Type != TokenType::IDENTIFIER)
					ParseFail("GRAPH MATCH pattern expects edge label.");
				LabelFilter = Lbl->Value;
				AdvanceToken();
			} else {
				auto E = CurrentToken();
				if(E && E->Type == TokenType::IDENTIFIER) {
					AdvanceToken();
					if(MatchToken(TokenType::PUNCTUATION, ":")) {
						auto Lbl = CurrentToken();
						if(!Lbl || Lbl->Type != TokenType::IDENTIFIER)
							ParseFail("GRAPH MATCH pattern expects edge label.");
						LabelFilter = Lbl->Value;
						AdvanceToken();
					}
				}
			}
			int64_t MinHops = 1;
			int64_t MaxHops = 1;
			if(MatchToken(TokenType::PUNCTUATION, "*")) {
				MinHops = 1;
				MaxHops = 5;
				bool SawMin = false;
				while(const auto Tok = CurrentToken()) {
					if(Tok->Type == TokenType::PUNCTUATION && Tok->Value == "]")
						break;
					if(Tok->Type == TokenType::LITERAL) {
						try {
							const int64_t V = std::stoll(Tok->Value);
							if(!SawMin) {
								MinHops = V;
								SawMin = true;
							} else {
								MaxHops = V;
							}
						} catch(...) {
						}
					}
					AdvanceToken();
				}
			}
			if(!MatchToken(TokenType::PUNCTUATION, "]"))
				ParseFail("GRAPH MATCH pattern: expected ']' after edge.");
			if(Reverse) {
				if(!MatchToken(TokenType::PUNCTUATION, "-"))
					ParseFail("GRAPH MATCH pattern: expected '-'.");
			} else if(!MatchToken(TokenType::PUNCTUATION, "->")) {
				if(!MatchToken(TokenType::PUNCTUATION, "-"))
					ParseFail("GRAPH MATCH pattern: expected '->'.");
				if(!MatchToken(TokenType::PUNCTUATION, ">"))
					ParseFail("GRAPH MATCH pattern: expected '>'.");
			}
			if(!MatchToken(TokenType::PUNCTUATION, "("))
				ParseFail("GRAPH MATCH pattern: expected '(' for target vertex.");
			auto B = CurrentToken();
			if(!B || B->Type != TokenType::IDENTIFIER)
				ParseFail("GRAPH MATCH pattern expects target vertex variable.");
			GraphMatchPatternSegment Seg;
			Seg.MinHops = MinHops;
			Seg.MaxHops = MaxHops;
			Seg.Reverse = Reverse;
			Seg.TargetVertexVar = B->Value;
			Seg.EdgeLabel = LabelFilter;
			Seg.EndOnProduct = (Segments.size() % 2) == 0;
			AdvanceToken();
			if(!MatchToken(TokenType::PUNCTUATION, ")"))
				ParseFail("GRAPH MATCH pattern: expected ')' after target vertex.");
			Segments.push_back(std::move(Seg));
		}
		std::string Anchor;
		if(MatchKeyword("FROM")) {
			auto St = CurrentToken();
			if(!St || (St->Type != TokenType::LITERAL && St->Type != TokenType::IDENTIFIER))
				ParseFail("GRAPH MATCH FROM expects vertex id.");
			Anchor = St->Value;
			AdvanceToken();
		}
		if(!MatchKeyword("IN"))
			ParseFail("GRAPH MATCH requires IN graph_name.");
		auto Gn = CurrentToken();
		if(!Gn || Gn->Type != TokenType::IDENTIFIER)
			ParseFail("Expected graph name after IN.");
		std::string GraphName = Gn->Value;
		AdvanceToken();
		std::string VertexWhereVar;
		std::string VertexWhereCol;
		std::string VertexWhereValue;
		if(MatchKeyword("WHERE")) {
			auto Col = CurrentToken();
			if(!Col || Col->Type != TokenType::IDENTIFIER)
				ParseFail("GRAPH MATCH WHERE expects predicate.");
			const std::string VarName = Col->Value;
			AdvanceToken();
			if(!MatchToken(TokenType::PUNCTUATION, "."))
				ParseFail("GRAPH MATCH WHERE expects var.column = value.");
			auto Lc = CurrentToken();
			if(!Lc || Lc->Type != TokenType::IDENTIFIER)
				ParseFail("GRAPH MATCH WHERE expects column name.");
			const std::string ColName = Lc->Value;
			AdvanceToken();
			if(!MatchToken(TokenType::PUNCTUATION, "="))
				ParseFail("GRAPH MATCH WHERE expects '='.");
			auto Lit = CurrentToken();
			if(!Lit || (Lit->Type != TokenType::LITERAL && Lit->Type != TokenType::IDENTIFIER))
				ParseFail("GRAPH MATCH WHERE expects literal.");
			if(ColName == "label") {
				LabelFilter = Lit->Value;
			} else {
				VertexWhereVar = VarName;
				VertexWhereCol = ColName;
				VertexWhereValue = Lit->Value;
				if(Anchor.empty())
					Anchor = Lit->Value;
			}
			AdvanceToken();
		}
		if(!MatchKeyword("INTO"))
			ParseFail("GRAPH MATCH requires INTO result_table.");
		auto Rt = CurrentToken();
		if(!Rt || Rt->Type != TokenType::IDENTIFIER)
			ParseFail("Expected result table name after INTO.");
		std::string Result = Rt->Value;
		AdvanceToken();
		int64_t MinHops = 1;
		int64_t MaxHops = 1;
		bool Reverse = false;
		if(!Segments.empty()) {
			MinHops = Segments.front().MinHops;
			MaxHops = Segments.front().MaxHops;
			Reverse = Segments.front().Reverse;
		}
		auto Out = std::make_unique<GraphMatchAST>(std::move(GraphName), std::move(LabelFilter), MinHops, MaxHops,
		                                           std::move(Anchor), Reverse, std::move(Result));
		Out->Segments = std::move(Segments);
		Out->VertexWhereVar = std::move(VertexWhereVar);
		Out->VertexWhereCol = std::move(VertexWhereCol);
		Out->VertexWhereValue = std::move(VertexWhereValue);
		return Out;
	}
	ParseFail("GRAPH statement must be TRAVERSE, MATCH, SHORTEST PATH, or PAGERANK.");
}

std::unique_ptr<StatementAST> Parser::ParseStatement() {
    if (auto Token = CurrentToken()) {
        if(Token->Type == TokenType::KEYWORD && Token->Value == "REPLACE") {
            AdvanceToken();
            if(!MatchKeyword("INTO"))
                ParseFail("Expected INTO after REPLACE");
            return ParseInsertStatement(true);
        }
		if(Token->Type == TokenType::KEYWORD && Token->Value == "MATCH")
			return ParseCypherMatchStatement();
		if(Token->Type == TokenType::KEYWORD && Token->Value == "GRAPH")
			return ParseGraphStatement();
        if(Token->Type == TokenType::KEYWORD && Token->Value == "PRAGMA")
            return ParsePragmaStatement();
        if(Token->Type == TokenType::KEYWORD && Token->Value == "EXPLAIN")
            return ParseExplainStatement();
        if(Token->Type == TokenType::KEYWORD && Token->Value == "WITH")
            return ParseWithStatement();
        if(Token->Type == TokenType::KEYWORD && Token->Value == "LOAD")
            return ParseLoadStatement();
        if(Token->Type == TokenType::KEYWORD && Token->Value == "VACUUM")
            return ParseVacuumStatement();
        if(Token->Type == TokenType::KEYWORD && Token->Value == "REPACK")
            return ParseRepackStatement();
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
        if(Token->Type == TokenType::KEYWORD && Token->Value == "COMMENT")
            return ParseCommentStatement();
        if(Token->Type == TokenType::KEYWORD && Token->Value == "SET")
            return ParseSetStatement();
        if(Token->Type == TokenType::KEYWORD && Token->Value == "SHOW")
            return ParseShowStatement();
        if(Token->Type == TokenType::KEYWORD && Token->Value == "DESCRIBE")
            return ParseDescribeStatement();
        if(Token->Type == TokenType::KEYWORD && Token->Value == "DESC")
            return ParseDescribeStatement();
        if(Token->Type == TokenType::KEYWORD && Token->Value == "CALL") {
            AdvanceToken();
            return ParseCallProcedureStatement("call");
        }
        if(Token->Type == TokenType::KEYWORD && Token->Value == "EXEC") {
            AdvanceToken();
            return ParseCallProcedureStatement("exec");
        }
        if(Token->Type == TokenType::KEYWORD && Token->Value == "EXECUTE") {
            AdvanceToken();
            return ParseCallProcedureStatement("execute");
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
                return ParseInsertStatement(false);
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

ASTNode Parser::ParseCommentStatement() {
	AdvanceToken();
	if(!MatchKeyword("ON"))
		ParseFail("COMMENT expects ON TABLE|COLUMN");
	auto Node = std::make_unique<CommentOnAST>();
	if(MatchKeyword("TABLE")) {
		auto T = CurrentToken();
		if(!T || T->Type != TokenType::IDENTIFIER)
			ParseFail("COMMENT ON TABLE expects table name");
		Node->Kind = CommentOnAST::TargetKind::Table;
		Node->TableName = T->Value;
		AdvanceToken();
	} else if(MatchKeyword("COLUMN")) {
		auto T = CurrentToken();
		if(!T || T->Type != TokenType::IDENTIFIER)
			ParseFail("COMMENT ON COLUMN expects table.column");
		Node->TableName = T->Value;
		AdvanceToken();
		if(!CurrentToken() || CurrentToken()->Value != ".")
			ParseFail("COMMENT ON COLUMN expects table.column");
		AdvanceToken();
		auto C = CurrentToken();
		if(!C || C->Type != TokenType::IDENTIFIER)
			ParseFail("COMMENT ON COLUMN expects table.column");
		Node->Kind = CommentOnAST::TargetKind::Column;
		Node->ColumnName = C->Value;
		AdvanceToken();
	} else
		ParseFail("COMMENT ON requires TABLE or COLUMN");

	if(!MatchKeyword("IS"))
		ParseFail("COMMENT ON requires IS '<comment>'");
	auto V = CurrentToken();
	if(!V || V->Type != TokenType::LITERAL)
		ParseFail("COMMENT ON IS expects a string literal");
	Node->Comment = V->Value;
	AdvanceToken();
	return Node;
}

namespace {

std::string FoldPragmaName(std::string Name) {
	for(char &C : Name)
		C = static_cast<char>(std::tolower(static_cast<unsigned char>(C)));
	return Name;
}

std::unique_ptr<PragmaAST> ResolvePragmaName(const std::string &NameLower, std::string Val, int64_t IntVal) {
	if(NameLower == "enable_profiling" || NameLower == "profiling")
		return std::make_unique<PragmaAST>(PragmaAST::Kind::Profile, Val.empty() ? std::string("on") : Val);
	if(NameLower == "disable_profiling")
		return std::make_unique<PragmaAST>(PragmaAST::Kind::Profile, "off");
	if(NameLower == "enable_optimizer")
		return std::make_unique<PragmaAST>(PragmaAST::Kind::OptLevel, "2", 2);
	if(NameLower == "disable_optimizer")
		return std::make_unique<PragmaAST>(PragmaAST::Kind::OptLevel, "0", 0);
	if(NameLower == "explain_analyze")
		return std::make_unique<PragmaAST>(PragmaAST::Kind::Explain, "analyze");
	if(NameLower == "profile" || NameLower == "astral_profile")
		return std::make_unique<PragmaAST>(PragmaAST::Kind::Profile, Val);
	if(NameLower == "region" || NameLower == "astral_region")
		return std::make_unique<PragmaAST>(PragmaAST::Kind::Region, Val);
	if(NameLower == "vector_batch_size") {
		IntVal = Val.empty() ? 1024 : std::stoll(Val);
		return std::make_unique<PragmaAST>(PragmaAST::Kind::VectorBatchSize, Val, IntVal);
	}
	if(NameLower == "join_strategy")
		return std::make_unique<PragmaAST>(PragmaAST::Kind::JoinStrategy, Val);
	if(NameLower == "zone_map_adaptive")
		return std::make_unique<PragmaAST>(PragmaAST::Kind::ZoneMapAdaptive, Val);
	if(NameLower == "lazy_materialization")
		return std::make_unique<PragmaAST>(PragmaAST::Kind::LazyMaterialization, Val);
	if(NameLower == "vm")
		return std::make_unique<PragmaAST>(PragmaAST::Kind::Vm, Val);
	if(NameLower == "jit") {
		const std::string L = FoldPragmaName(Val);
		if(L == "on" || L == "1" || L == "true" || L == "yes")
			return std::make_unique<PragmaAST>(PragmaAST::Kind::JitEnabled, "on");
		if(L == "off" || L == "0" || L == "false" || L == "no")
			return std::make_unique<PragmaAST>(PragmaAST::Kind::JitEnabled, "off");
		return std::make_unique<PragmaAST>(PragmaAST::Kind::Vm, "jit");
	}
	if(NameLower == "opt_level" || NameLower == "optimizer") {
		IntVal = Val.empty() ? 2 : std::stoll(Val);
		return std::make_unique<PragmaAST>(PragmaAST::Kind::OptLevel, Val, IntVal);
	}
	if(NameLower == "verify_fast_path")
		return std::make_unique<PragmaAST>(PragmaAST::Kind::VerifyFastPath, Val);
	if(NameLower == "explain")
		return std::make_unique<PragmaAST>(PragmaAST::Kind::Explain, Val);
	if(NameLower == "dump_profile")
		return std::make_unique<PragmaAST>(PragmaAST::Kind::DumpProfile, Val);
	if(NameLower == "reset_profile")
		return std::make_unique<PragmaAST>(PragmaAST::Kind::ResetProfile);
	if(NameLower == "use_precomputed")
		return std::make_unique<PragmaAST>(PragmaAST::Kind::UsePrecomputed, Val);
	if(NameLower == "query_checkpoint_on_signal")
		return std::make_unique<PragmaAST>(PragmaAST::Kind::QueryCheckpointOnSignal, Val);
	if(NameLower == "query_checkpoint_interval") {
		IntVal = Val.empty() ? 0 : std::stoll(Val);
		return std::make_unique<PragmaAST>(PragmaAST::Kind::QueryCheckpointInterval, Val, IntVal);
	}
	if(NameLower == "resume_query_checkpoint")
		return std::make_unique<PragmaAST>(PragmaAST::Kind::ResumeQueryCheckpoint, Val);
	if(NameLower == "foreign_keys")
		return std::make_unique<PragmaAST>(PragmaAST::Kind::ForeignKeys, Val);
	if(NameLower == "opt_confidence_floor" || NameLower == "confidence_floor") {
		IntVal = 0;
		return std::make_unique<PragmaAST>(PragmaAST::Kind::OptConfidenceFloor, Val, IntVal);
	}
	if(NameLower == "enable_verification" || NameLower == "verify")
		return std::make_unique<PragmaAST>(PragmaAST::Kind::VerifyFastPath,
		                                   Val.empty() ? std::string("on") : Val);
	if(NameLower == "disable_verification")
		return std::make_unique<PragmaAST>(PragmaAST::Kind::VerifyFastPath, "off");
	if(NameLower == "profile_output" || NameLower == "profiling_output")
		return std::make_unique<PragmaAST>(PragmaAST::Kind::DumpProfile, Val);
	if(NameLower == "journal_mode" || NameLower == "synchronous" || NameLower == "cache_size" ||
	   NameLower == "temp_store" || NameLower == "mmap_size" || NameLower == "memory_limit" ||
	   NameLower == "wal_checkpoint" || NameLower == "checkpoint" || NameLower == "table_info" ||
	   NameLower == "integrity_check" || NameLower == "enable_external_access" ||
	   NameLower == "disabled_optimizers" || NameLower == "threads" || NameLower == "force_parallelism" ||
	   NameLower == "preserve_insertion_order")
		return std::make_unique<PragmaAST>(PragmaAST::Kind::CompatAck, NameLower, 0);
	return nullptr;
}

} // namespace

std::unique_ptr<StatementAST> Parser::ParsePragmaStatement() {
	AdvanceToken();
	const std::optional<Token> NameTok = CurrentToken();
	if(!NameTok || (NameTok->Type != TokenType::IDENTIFIER && NameTok->Type != TokenType::KEYWORD))
		ParseFail("PRAGMA expects a directive name");
	std::string PragmaName = NameTok->Value;
	AdvanceToken();
	if(MatchToken(TokenType::PUNCTUATION, ".")) {
		const std::optional<Token> QualTok = CurrentToken();
		if(!QualTok || (QualTok->Type != TokenType::IDENTIFIER && QualTok->Type != TokenType::KEYWORD))
			ParseFail("PRAGMA expects directive name after '.'");
		PragmaName = QualTok->Value;
		AdvanceToken();
	}
	const std::string NameLower = FoldPragmaName(PragmaName);

	std::string Val;
	int64_t IntVal = 0;
	const auto ReadOnePragmaValue = [&]() -> bool {
		if(MatchKeyword("NULL")) {
			Val = "null";
			return true;
		}
		if(MatchKeyword("ON") || MatchKeyword("OFF") || MatchKeyword("TRUE") || MatchKeyword("FALSE") ||
		   MatchKeyword("YES") || MatchKeyword("NO")) {
			if(CurrentIndex_ > 0)
				Val = Tokens_[CurrentIndex_ - 1].Value;
			return true;
		}
		if(MatchToken(TokenType::LITERAL)) {
			if(CurrentIndex_ > 0)
				Val = Tokens_[CurrentIndex_ - 1].Value;
			return true;
		}
		if(const std::optional<Token> T = CurrentToken()) {
			if(T->Type == TokenType::IDENTIFIER || T->Type == TokenType::KEYWORD) {
				Val = T->Value;
				AdvanceToken();
				return true;
			}
		}
		return false;
	};

	if(MatchToken(TokenType::PUNCTUATION, "=") || MatchToken(TokenType::PUNCTUATION, ":"))
		(void)ReadOnePragmaValue();
	else if(MatchToken(TokenType::PUNCTUATION, "(")) {
		(void)ReadOnePragmaValue();
		while(MatchToken(TokenType::PUNCTUATION, ","))
			(void)ReadOnePragmaValue();
		if(!MatchToken(TokenType::PUNCTUATION, ")"))
			ParseFail("PRAGMA expects ')' after parenthesized value");
	} else
		(void)ReadOnePragmaValue();

	if(auto Resolved = ResolvePragmaName(NameLower, Val, IntVal))
		return Resolved;
	ParseFail("Unknown PRAGMA: " + PragmaName);
}

std::unique_ptr<StatementAST> Parser::ParseExplainStatement() {
	AdvanceToken();
	const bool Analyze = MatchKeyword("ANALYZE");
	std::unique_ptr<StatementAST> Inner;
	if(const std::optional<Token> T = CurrentToken()) {
		if(T->Type == TokenType::SELECT)
			Inner = ParseSelectStatement();
		else if(T->Type == TokenType::KEYWORD && T->Value == "WITH")
			Inner = ParseWithStatement();
	}
	if(!Inner)
		ParseFail("EXPLAIN expects SELECT or WITH … SELECT");
	return std::make_unique<ExplainSelectAST>(Analyze, std::move(Inner));
}

ASTNode Parser::ParseSetStatement() {
	AdvanceToken();
	bool SessionScope = false;
	if(MatchKeyword("TRANSACTIONS"))
		SessionScope = true;
	else if(!MatchKeyword("TRANSACTION"))
		ParseFail("SET expects TRANSACTION or TRANSACTIONS");
	if(!MatchKeyword("ISOLATION") || !MatchKeyword("LEVEL"))
		ParseFail("SET TRANSACTION expects ISOLATION LEVEL");

	TransactionIsolationLevel Iso = TransactionIsolationLevel::Unspecified;
	if(MatchKeyword("SERIALIZABLE"))
		Iso = TransactionIsolationLevel::Serializable;
	else if(MatchKeyword("READ")) {
		if(!MatchKeyword("COMMITTED"))
			ParseFail("READ must be followed by COMMITTED");
		Iso = TransactionIsolationLevel::ReadCommitted;
	} else if(MatchKeyword("REPEATABLE")) {
		if(!MatchKeyword("READ"))
			ParseFail("REPEATABLE must be followed by READ");
		Iso = TransactionIsolationLevel::RepeatableRead;
	} else
		ParseFail("Unsupported isolation level");
	return std::make_unique<TransactionAST>(TransactionType::SET_TRANSACTION, Iso, SessionScope);
}

ASTNode Parser::ParseShowStatement() {
	AdvanceToken();
	if(MatchKeyword("TABLES"))
		return std::make_unique<ShowTablesAST>();
	ParseFail("SHOW currently supports SHOW TABLES");
}

ASTNode Parser::ParseDescribeStatement() {
	AdvanceToken();
	auto T = CurrentToken();
	if(!T || T->Type != TokenType::IDENTIFIER)
		ParseFail("DESCRIBE expects table name");
	std::string Table = T->Value;
	AdvanceToken();
	return std::make_unique<DescribeTableAST>(std::move(Table));
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
			ParseFail("Expected FORMAT name (CSV, JSON, TSV, XLSX)");
		std::string F = Tk->Value;
		AdvanceToken();
		for(char &Ch : F)
			Ch = static_cast<char>(std::toupper(static_cast<unsigned char>(Ch)));
		if(F == "EXCEL")
			F = "XLSX";
		if(F != "JSON" && F != "CSV" && F != "TSV" && F != "XLSX")
			ParseFail("FORMAT must be CSV, JSON, TSV, or XLSX");
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
	if(MatchKeyword("TRIGGER"))
		return ParseAlterTriggerStatement();
	if(MatchKeyword("USER")) {
		auto NameTok = CurrentToken();
		if(!NameTok || NameTok->Type != TokenType::IDENTIFIER)
			ParseFail("ALTER USER expects a user name");
		std::string UserName = NameTok->Value;
		AdvanceToken();
		std::string Password = ParseIdentifiedByPassword();
		return std::make_unique<AlterUserPasswordAST>(std::move(UserName), std::move(Password));
	}
	if(!MatchKeyword("TABLE"))
		ParseFail("ALTER expects USER or TABLE");
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



