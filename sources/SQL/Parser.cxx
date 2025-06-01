#include <SQL/SQL.hxx>
#include <Database/User.hxx>
#include <cctype>
#include <iostream>
#include <stdexcept>
#include <unordered_set>

namespace AstralDB {
namespace SQL {

int Parser::GetTokenPrecedence(const Token &Token) {
    static std::unordered_map<std::string, int> PrecedenceMap = {
        {"OR", 1},
        {"AND", 2},
        {"=", 3}, {"!=", 3},
        {"<", 4}, {"<=", 4}, {">", 4}, {">=", 4},
        {"+", 5}, {"-", 5},
        {"*", 6}, {"/", 6}, {"%", 6}
    };
    auto It = PrecedenceMap.find(Token.Value);
    return (It != PrecedenceMap.end()) ? It->second : -1;
}

bool Parser::IsConstraint(const std::string &TokenValue) {
    static const std::unordered_set<std::string> Constraints = {
        "PRIMARY", "KEY", "NOT", "NULL", "UNIQUE", "AUTO_INCREMENT"
    };
    return Constraints.find(TokenValue) != Constraints.end();
}

TokenStream Parser::Tokenize() {
    TokenStream Tokens;
    size_t Position = 0;
    while(Position < Query_.size()) {
        while(Position < Query_.size() && std::isspace(Query_[Position]))
            Position++;
        if(Position >= Query_.size()) break;
        char CurrentChar = Query_[Position];
        if(std::isdigit(CurrentChar)) {
            size_t Start = Position;
            while(Position < Query_.size() && std::isdigit(Query_[Position]))
                Position++;
            if(Position < Query_.size() && Query_[Position] == '.') {
                Position++;
                while(Position < Query_.size() && std::isdigit(Query_[Position]))
                    Position++;
            }
            Tokens.push_back(Token{TokenType::LITERAL, std::string(Query_.substr(Start, Position - Start))});
        }
        else if(CurrentChar == '\'' || CurrentChar == '\"') {
            char QuoteChar = CurrentChar;
            Position++;
            size_t Start = Position;
            while(Position < Query_.size() && Query_[Position] != QuoteChar) {
                if(Query_[Position] == '\\' && Position + 1 < Query_.size())
                    Position += 2;
                else
                    Position++;
            }
            if(Position >= Query_.size())
                throw std::runtime_error("Unterminated string literal");
            std::string Literal = std::string(Query_.substr(Start, Position - Start));
            Tokens.push_back(Token{TokenType::LITERAL, Literal});
            Position++;
        }
        else if (std::isalnum(CurrentChar) || CurrentChar == '_') {
            size_t Start = Position;
            while(Position < Query_.size() && (std::isalnum(Query_[Position]) || Query_[Position] == '_'))
                Position++;
            std::string value = std::string(Query_.substr(Start, Position - Start));
            TokenType type = IsKeyword(value) ? TokenType::KEYWORD : TokenType::IDENTIFIER;
            Tokens.push_back(Token{type, value});
        }
        else {
            std::string OperatorStr;
            if(Position + 1 < Query_.size()) {
                std::string TwoChars = std::string(Query_.substr(Position, 2));
                if(TwoChars == "<=" || TwoChars == ">=" || TwoChars == "!=" || TwoChars == "==") {
                    OperatorStr = TwoChars;
                    Position += 2;
                    Tokens.push_back(Token{TokenType::PUNCTUATION, OperatorStr});
                    continue;
                }
            }
            OperatorStr = std::string(1, CurrentChar);
            Position++;
            if(std::ispunct(CurrentChar))
                Tokens.push_back(Token{TokenType::PUNCTUATION, OperatorStr});
            else
                Tokens.push_back(Token{TokenType::SYMBOL, OperatorStr});
        }
    }
    return Tokens;
}

Tree<ASTNode> Parser::BuildAST() const {
    std::cout << "[BuildAST] Starting AST build. Token count: " << Tokens_.size() << std::endl;
    Tree<ASTNode> Result;
    ASTType Statements;
    size_t Index = 0;
    while(Index < Tokens_.size()) {
        std::cout << "[BuildAST] Parsing statement at token index: " << Index << std::endl;
        try {
            auto Statement = const_cast<Parser*>(this)->ParseStatement();
            if(Statement) {
                std::cout << "[BuildAST] Parsed statement at index " << Index << std::endl;
                Statements.push_back(std::move(Statement));
            } else {
                std::cerr << "[BuildAST] Failed to parse statement at index " << Index << std::endl;
                throw std::runtime_error("Failed to parse statement.");
            }
        } catch (const std::exception& e) {
            std::cerr << "[BuildAST] Error parsing statement at token " << Index << ": " << e.what() << '\n';
            const_cast<Parser*>(this)->AdvanceToken();
            continue;
        }
        // Defensive: advance token if not already advanced
        const_cast<Parser*>(this)->AdvanceToken();
        Index = const_cast<Parser*>(this)->CurrentIndex_;
    }
    if(Statements.empty())
        throw std::runtime_error("No valid statements were parsed.");
    for(auto&& Stmt : Statements) {
        std::cout << "[BuildAST] Adding statement to AST tree." << std::endl;
        Result.Add(std::move(Stmt));
    }
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
        throw std::runtime_error("Unexpected end of input in primary expression");
    if(CurrentToken()->Value == "(") {
        AdvanceToken();
        auto Expr = ParseExpression();
        if(!MatchToken(TokenType::PUNCTUATION))
            throw std::runtime_error("Expected ')' in primary expression");
        return Expr;
    }
    auto Literal = std::make_unique<LiteralAST>(CurrentToken()->Value);
    AdvanceToken();
    return Literal;
}

ASTNode Parser::ParseCreateStatement() {
    AdvanceToken(); // consume CREATE
    if (!MatchKeyword("TABLE"))
        throw std::runtime_error("Expected 'TABLE' after 'CREATE'.");
    auto TableToken = CurrentToken();
    if(!TableToken)
        throw std::runtime_error("Expected table name after 'CREATE TABLE'.");
    std::string TableName = TableToken->Value;
    AdvanceToken();
    if(!MatchToken(TokenType::PUNCTUATION))
        throw std::runtime_error("Expected '(' after table name in 'CREATE TABLE'.");
    std::vector<ColumnDefinition> Columns;
    while(true) {
        auto ColumnToken = CurrentToken();
        if(!ColumnToken)
            throw std::runtime_error("Unexpected end of input while parsing column definition.");
        if(ColumnToken->Value == ")") {
            AdvanceToken();
            break;
        }
        std::string ColumnName = ColumnToken->Value;
        AdvanceToken();
        auto TypeToken = CurrentToken();
        if(!TypeToken)
            throw std::runtime_error("Expected data type after column name in 'CREATE TABLE'.");
        std::string ColumnType = TypeToken->Value;
        AdvanceToken();
        std::vector<std::string> Constraints;
        while(CurrentToken() && IsConstraint(CurrentToken()->Value)) {
            Constraints.push_back(CurrentToken()->Value);
            AdvanceToken();
        }
        Columns.emplace_back(ColumnName, ColumnType, Constraints);
        if(CurrentToken() && CurrentToken()->Value == ",") {
            AdvanceToken();
        } else if(CurrentToken() && CurrentToken()->Value == ")") {
            AdvanceToken();
            break;
        } else {
            throw std::runtime_error("Expected ',' or ')' in 'CREATE TABLE' statement.");
        }
    }
    return std::make_unique<CreateAST>(TableName, Columns);
}

ASTNode Parser::ParseSelectStatement() {
    AdvanceToken();
    std::vector<std::string> Columns;
    while(auto TokenOpt = CurrentToken()) {
        if(TokenOpt->Value == "FROM") break;
        std::string Col = TokenOpt->Value;
        if(!Col.empty() && Col.back() == ',') Col.pop_back();
        Columns.push_back(Col);
        AdvanceToken();
    }
    if(!MatchKeyword("FROM"))
        throw std::runtime_error("Expected FROM in SELECT statement");
    if(auto TableToken = CurrentToken()) {
        std::string TableName = TableToken->Value;
        AdvanceToken();
        auto TableAst = std::make_unique<TableAST>(TableName);
        return std::make_unique<SelectAST>(Columns, std::move(TableAst));
    }
    throw std::runtime_error("Expected table name after FROM in SELECT statement");
}

ASTNode Parser::ParseInsertStatement() {
    AdvanceToken();
    if(!MatchKeyword("INTO"))
        throw std::runtime_error("Expected INTO after INSERT");
    auto TableToken = CurrentToken();
    if(!TableToken)
        throw std::runtime_error("Expected table name after INSERT INTO");
    std::string TableName = TableToken->Value;
    AdvanceToken();
    std::vector<std::string> Columns;
    // Check if next token is '('
    if(CurrentToken() && CurrentToken()->Value == "(") {
        AdvanceToken();
        while (auto TokenOpt = CurrentToken()) {
            if(TokenOpt->Value == ")") {
                AdvanceToken();
                break;
            }
            std::string Col = TokenOpt->Value;
            if(!Col.empty() && Col.back() == ',') Col.pop_back();
            Columns.push_back(Col);
            AdvanceToken();
        }
    }
    // Now expect VALUES
    if(!MatchKeyword("VALUES"))
        throw std::runtime_error("Expected VALUES in INSERT statement");
    // Fix: Only advance if the next token is '('
    if(!CurrentToken() || CurrentToken()->Value != "(")
        throw std::runtime_error("Expected '(' before values in INSERT");
    AdvanceToken(); // consume '('
    std::vector<std::string> Values;
    while(auto TokenOpt = CurrentToken()) {
        if(TokenOpt->Value == ")") {
            AdvanceToken();
            break;
        }
        std::string Val = TokenOpt->Value;
        if (!Val.empty() && Val.back() == ',')
            Val.pop_back();
        Values.push_back(Val);
        AdvanceToken();
    }
    auto TableAst = std::make_unique<TableAST>(TableName);
    std::cout << "[ParseInsertStatement] Table: " << TableName << ", Columns: [";
    for (const auto& c : Columns) std::cout << c << ", ";
    std::cout << "] Values: [";
    for (const auto& v : Values) std::cout << v << ", ";
    std::cout << "]\n";
    return std::make_unique<InsertAST>(std::move(TableAst), Columns, Values);
}

ASTNode Parser::ParseUpdateStatement() {
    AdvanceToken();
    auto TableToken = CurrentToken();
    if(!TableToken)
        throw std::runtime_error("Expected table name after UPDATE");
    std::string TableName = TableToken->Value;
    AdvanceToken();
    if(!MatchKeyword("SET"))
        throw std::runtime_error("Expected SET in UPDATE statement");
    std::vector<std::pair<std::string, std::string>> Assignments;
    while(auto TokenOpt = CurrentToken()) {
        if(TokenOpt->Value == "WHERE") break;
        std::string ColumnName = TokenOpt->Value;
        AdvanceToken();
        if(!MatchToken(TokenType::PUNCTUATION))
            throw std::runtime_error("Expected '=' in assignment of UPDATE statement");
        auto ValueToken = CurrentToken();
        if (!ValueToken)
            throw std::runtime_error("Expected value in assignment of UPDATE statement");
        std::string Value = ValueToken->Value;
        AdvanceToken();
        Assignments.push_back({ColumnName, Value});
        if(CurrentToken() && CurrentToken()->Value == ",")
            AdvanceToken();
    }
    std::unique_ptr<ExpressionAST> Condition = nullptr;
    if(CurrentToken() && CurrentToken()->Value == "WHERE") {
        AdvanceToken();
        Condition = ParseBinaryOperation();
    }
    return std::make_unique<UpdateAST>(TableName, std::move(Assignments), std::move(Condition));
}

ASTNode Parser::ParseDeleteStatement() {
    AdvanceToken();
    if(!MatchKeyword("FROM"))
        throw std::runtime_error("Expected FROM in DELETE statement");
    auto TableToken = CurrentToken();
    if(!TableToken)
        throw std::runtime_error("Expected table name in DELETE statement");
    std::string TableName = TableToken->Value;
    AdvanceToken();
    std::unique_ptr<ExpressionAST> Condition = nullptr;
    if(CurrentToken() && CurrentToken()->Value == "WHERE") {
        AdvanceToken();
        Condition = ParseBinaryOperation();
    }
    return std::make_unique<DeleteAST>(TableName, std::move(Condition));
}

ASTNode Parser::ParseGrantStatement() {
    AdvanceToken(); // consume GRANT
    // Expect permission type (SELECT, INSERT, UPDATE, DELETE, etc.)
    if (!CurrentToken()) throw std::runtime_error("Expected permission after GRANT");
    std::string PermissionString = CurrentToken()->Value;
    Permissions Perms;
    if (PermissionString == "SELECT") Perms = Permissions::Select;
    else if (PermissionString == "INSERT") Perms = Permissions::Insert;
    else if (PermissionString == "UPDATE") Perms = Permissions::Update;
    else if (PermissionString == "DELETE") Perms = Permissions::Delete;
    else if (PermissionString == "TRUNCATE") Perms = Permissions::Truncate;
    else if (PermissionString == "REFERENCES") Perms = Permissions::References;
    else if (PermissionString == "TRIGGER") Perms = Permissions::Trigger;
    else if (PermissionString == "ALL") Perms = Permissions::All;
    else throw std::runtime_error("Unknown permission in GRANT: " + PermissionString);
    AdvanceToken();
    if (!MatchKeyword("ON")) throw std::runtime_error("Expected ON after permission in GRANT");
    std::string tableName;
    if (CurrentToken() && CurrentToken()->Type == TokenType::IDENTIFIER) {
        tableName = CurrentToken()->Value;
        AdvanceToken();
    }
    if (!MatchKeyword("TO")) throw std::runtime_error("Expected TO after table in GRANT");
    if (!CurrentToken()) throw std::runtime_error("Expected user after TO in GRANT");
    std::string Username = CurrentToken()->Value;
    AdvanceToken();
    return std::make_unique<GrantAST>(Username, Perms, tableName);
}

std::unique_ptr<ExpressionAST> Parser::ParseRevokeStatement() {
    AdvanceToken(); // consume REVOKE
    // Expect permission type (SELECT, INSERT, UPDATE, DELETE, etc.)
    if (!CurrentToken()) throw std::runtime_error("Expected permission after REVOKE");
    std::string PermissionString = CurrentToken()->Value;
    Permissions Perms;
    if (PermissionString == "SELECT") Perms = Permissions::Select;
    else if (PermissionString == "INSERT") Perms = Permissions::Insert;
    else if (PermissionString == "UPDATE") Perms = Permissions::Update;
    else if (PermissionString == "DELETE") Perms = Permissions::Delete;
    else if (PermissionString == "TRUNCATE") Perms = Permissions::Truncate;
    else if (PermissionString == "REFERENCES") Perms = Permissions::References;
    else if (PermissionString == "TRIGGER") Perms = Permissions::Trigger;
    else if (PermissionString == "ALL") Perms = Permissions::All;
    else throw std::runtime_error("Unknown permission in REVOKE: " + PermissionString);
    AdvanceToken();
    if (!MatchKeyword("ON")) throw std::runtime_error("Expected ON after permission in REVOKE");
    std::string tableName;
    if (CurrentToken() && CurrentToken()->Type == TokenType::IDENTIFIER) {
        tableName = CurrentToken()->Value;
        AdvanceToken();
    }
    if (!MatchKeyword("FROM")) throw std::runtime_error("Expected FROM after table in REVOKE");
    if (!CurrentToken()) throw std::runtime_error("Expected user after FROM in REVOKE");
    std::string Username = CurrentToken()->Value;
    AdvanceToken();
    return std::make_unique<RevokeAST>(Username, Perms, tableName);
}

std::unique_ptr<ExpressionAST> Parser::ParseWhereClause() {
    if(CurrentToken() && CurrentToken()->Value == "WHERE") {
        AdvanceToken();
        return ParseBinaryOperation();
    }
    return nullptr;
}

std::unique_ptr<ExpressionAST> Parser::ParseBinaryOperation() {
    auto LHS = ParsePrimary();
    return ParseBinaryOperation(0, std::move(LHS));
}

std::unique_ptr<ExpressionAST> Parser::ParseStatement() {
    std::cout << "[ParseStatement] Called at token index: " << CurrentIndex_ << std::endl;
    if(auto CurrentTok = CurrentToken()) {
        std::cout << "[ParseStatement] First token: " << CurrentTok->Value << std::endl;
        std::string FirstValue = CurrentTok->Value;
        if(FirstValue == "SELECT")
            return ParseSelectStatement();
        else if(FirstValue == "INSERT")
            return ParseInsertStatement();
        else if(FirstValue == "UPDATE")
            return ParseUpdateStatement();
        else if(FirstValue == "DELETE")
            return ParseDeleteStatement();
        else if(FirstValue == "CREATE")
            return ParseCreateStatement();
        else if(FirstValue == "GRANT")
            return ParseGrantStatement();
        else if(FirstValue == "REVOKE")
            return ParseRevokeStatement();
        else
            std::cerr << "[ParseStatement] Unknown statement type: " << FirstValue << std::endl;
    }
    throw std::runtime_error("Empty query");
}

std::unique_ptr<ExpressionAST> Parser::ParseBinaryOperation(int MinPrec, std::unique_ptr<ExpressionAST> LHS) {
    while(true) {
        if(!CurrentToken()) break;
        int CurrentPrec = GetTokenPrecedence(*CurrentToken());
        if(CurrentPrec < MinPrec) break;
        std::string Op = CurrentToken()->Value;
        AdvanceToken();
        auto RHS = ParsePrimary();
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
        LHS = std::make_unique<BinaryOpAST>(std::move(LHS), Op, std::move(RHS));
    }
    return LHS;
}

std::unique_ptr<ExpressionAST> Parser::ParseExpression() {
    return ParseBinaryOperation();
}

bool Parser::IsKeyword(const std::string &Token) {
    static const std::unordered_set<std::string> Keywords = {
        "CREATE", "INSERT", "INTO", "VALUES", "UPDATE", "SET",
        "WHERE", "DELETE", "FROM", "TABLE"
    };
    return Keywords.find(Token) != Keywords.end();
}
}
}
