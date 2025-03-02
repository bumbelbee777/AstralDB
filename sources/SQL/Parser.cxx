#include "SQL/Bytecode.hxx"
#include <SQL/SQL.hxx>
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
    auto It = PrecedenceMap.find(Token.Type);
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
            Tokens.push_back(Token{"Number", std::string(Query_.substr(Start, Position - Start))});
        }
        else if(CurrentChar == '\'' || CurrentChar == '"') {
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
            Tokens.push_back(Token{"String", Literal});
            Position++;
        }
        else if (std::isalnum(CurrentChar) || CurrentChar == '_') {
            size_t Start = Position;
            while(Position < Query_.size() && (std::isalnum(Query_[Position]) || Query_[Position] == '_'))
                Position++;
            Tokens.push_back(Token{"Word", std::string(Query_.substr(Start, Position - Start))});
        }
        else {
            std::string OperatorStr;
            if(Position + 1 < Query_.size()) {
                std::string TwoChars = std::string(Query_.substr(Position, 2));
                if(TwoChars == "<=" || TwoChars == ">=" || TwoChars == "!=" || TwoChars == "==") {
                    OperatorStr = TwoChars;
                    Position += 2;
                    Tokens.push_back(Token{"Operator", OperatorStr});
                    continue;
                }
            }
            OperatorStr = std::string(1, CurrentChar);
            Position++;
            if(std::ispunct(CurrentChar))
                Tokens.push_back(Token{"Punctuation", OperatorStr});
            else
                Tokens.push_back(Token{"Symbol", OperatorStr});
        }
    }
    return Tokens;
}

Tree<std::vector<std::unique_ptr<ExpressionAST>>> Parser::BuildAST() const {
    Tree<std::vector<std::unique_ptr<ExpressionAST>>> Result;
    std::vector<std::unique_ptr<ExpressionAST>> Statements;
    size_t Index = 0;
    while(Index < Tokens_.size()) {
        try {
            auto Statement = const_cast<Parser*>(this)->ParseStatement();
            if(Statement)
                Statements.push_back(std::move(Statement));
            else
                throw std::runtime_error("Failed to parse statement.");
        } catch(const std::exception &e) {
            std::cerr << "Error parsing statement: " << e.what() << '\n';
            break;
        }
    }
    if(Statements.empty())
        throw std::runtime_error("No valid statements were parsed.");
    Result.SetRoot(std::move(Statements));
    return Result;
}

void Parser::DumpTokens() const {
    for(const auto &TokenItem : Tokens_)
        std::cout << "[" << TokenItem.Type << ": " << TokenItem.Value << "] ";
    std::cout << std::endl;
}

std::unique_ptr<ExpressionAST> Parser::ParsePrimary() {
    if(!CurrentToken())
        throw std::runtime_error("Unexpected end of input in primary expression");
    if(CurrentToken()->Value == "(") {
        AdvanceToken();
        auto Expr = ParseExpression();
        if(!MatchToken(")"))
            throw std::runtime_error("Expected ')' in primary expression");
        return Expr;
    }
    auto Literal = std::make_unique<LiteralAST>(CurrentToken()->Value);
    AdvanceToken();
    return Literal;
}

std::unique_ptr<ExpressionAST> Parser::ParseCreateStatement() {
    AdvanceToken();
    if(!MatchToken("TABLE"))
        throw std::runtime_error("Expected 'TABLE' after 'CREATE'.");
    auto TableToken = CurrentToken();
    if(!TableToken)
        throw std::runtime_error("Expected table name after 'CREATE TABLE'.");
    std::string TableName = TableToken->Value;
    AdvanceToken();
    if(!MatchToken("("))
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

std::unique_ptr<ExpressionAST> Parser::ParseSelectStatement() {
    AdvanceToken();
    std::vector<std::string> Columns;
    while(auto TokenOpt = CurrentToken()) {
        if(TokenOpt->Value == "FROM") break;
        std::string Col = TokenOpt->Value;
        if(!Col.empty() && Col.back() == ',') Col.pop_back();
        Columns.push_back(Col);
        AdvanceToken();
    }
    if(!MatchToken("FROM"))
        throw std::runtime_error("Expected FROM in SELECT statement");
    if(auto TableToken = CurrentToken()) {
        std::string TableName = TableToken->Value;
        AdvanceToken();
        auto TableAst = std::make_unique<TableAST>(TableName);
        return std::make_unique<SelectAST>(Columns, std::move(TableAst));
    }
    throw std::runtime_error("Expected table name after FROM in SELECT statement");
}

std::unique_ptr<ExpressionAST> Parser::ParseInsertStatement() {
    AdvanceToken();
    if(!MatchToken("INTO"))
        throw std::runtime_error("Expected INTO after INSERT");
    auto TableToken = CurrentToken();
    if(!TableToken)
        throw std::runtime_error("Expected table name after INSERT INTO");
    std::string TableName = TableToken->Value;
    AdvanceToken();
    if(!MatchToken("("))
        throw std::runtime_error("Expected '(' after table name in INSERT");
    std::vector<std::string> Columns;
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
    if(!MatchToken("VALUES"))
        throw std::runtime_error("Expected VALUES in INSERT statement");
    if(!MatchToken("("))
        throw std::runtime_error("Expected '(' before values in INSERT");
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
    return std::make_unique<InsertAST>(std::move(TableAst), Columns, Values);
}

std::unique_ptr<ExpressionAST> Parser::ParseUpdateStatement() {
    AdvanceToken();
    auto TableToken = CurrentToken();
    if(!TableToken)
        throw std::runtime_error("Expected table name after UPDATE");
    std::string TableName = TableToken->Value;
    AdvanceToken();
    if(!MatchToken("SET"))
        throw std::runtime_error("Expected SET in UPDATE statement");
    std::vector<std::pair<std::string, std::string>> Assignments;
    while(auto TokenOpt = CurrentToken()) {
        if(TokenOpt->Value == "WHERE") break;
        std::string ColumnName = TokenOpt->Value;
        AdvanceToken();
        if(!MatchToken("="))
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

std::unique_ptr<ExpressionAST> Parser::ParseDeleteStatement() {
    AdvanceToken();
    if(!MatchToken("FROM"))
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
    Tokens_ = Tokenize();
    CurrentIndex_ = 0;
    if(auto CurrentTok = CurrentToken()) {
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
        else
            throw std::runtime_error("Unrecognized statement type: " + FirstValue);
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

void Parser::DumpAST() {
    for(auto &Node: AST) {
        Bytecode Dummy = Node->EmitBytecode();
        std::cout << Disassemble(Dummy);
    }
}
}
}
