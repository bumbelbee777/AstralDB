#pragma once

#include <SQL/Bytecode.hxx>
#include <DS/Tree.hxx>
#include <DS/BPlusTree.hxx>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <optional>

namespace AstralDB {
namespace SQL {
enum class TokenType {
    IDENTIFIER,
    KEYWORD,
    PUNCTUATION,
    LITERAL,
    WHITESPACE,
    EOF_,
    SYMBOL // Added SYMBOL
};

struct Token {
    TokenType Type;
    std::string Value;
};

using TokenStream = std::vector<Token>;

struct ColumnDefinition {
    std::string Name;
    std::string Type;
    std::vector<std::string> Constraints;

    ColumnDefinition(std::string name, std::string type, std::vector<std::string> constraints = {})
        : Name(std::move(name)), Type(std::move(type)), Constraints(std::move(constraints)) {}
};

struct ExpressionAST {
    virtual ~ExpressionAST() = default;
    virtual Bytecode EmitBytecode() const = 0;
};

struct LiteralAST : public ExpressionAST {
    std::string Value;

    explicit LiteralAST(std::string Value) : Value(std::move(Value)) {}
    Bytecode EmitBytecode() const override;
};

struct TableAST : public ExpressionAST {
    std::string TableName;

    explicit TableAST(std::string Name) : TableName(std::move(Name)) {}
    Bytecode EmitBytecode() const override;
};

/* ColumnAST : public ExpressionAST {
    std::string ColumnName;
public:
    explicit ColumnAST(std::string ColumnName) : ColumnName(std::move(ColumnName)) {}
};*/

struct CreateAST : public ExpressionAST {
    std::string TableName;
    std::vector<ColumnDefinition> Columns;

    explicit CreateAST(std::string TableName, std::vector<ColumnDefinition> Columns) : TableName(std::move(TableName)),
                        Columns(std::move(Columns)) {}
    Bytecode EmitBytecode() const override;
};

struct SelectAST : public ExpressionAST {
    std::vector<std::string> Columns;
    std::unique_ptr<TableAST> Table;

    SelectAST(std::vector<std::string> Columns, std::unique_ptr<TableAST> Table)
        : Columns(std::move(Columns)), Table(std::move(Table)) {}
    Bytecode EmitBytecode() const override;
};

struct InsertAST : public ExpressionAST {
    std::unique_ptr<TableAST> Table;
    std::vector<std::string> Columns;
    std::vector<std::string> Values;
public:
    InsertAST(std::unique_ptr<TableAST> Table, std::vector<std::string> Columns, std::vector<std::string> Values)
        : Table(std::move(Table)), Columns(std::move(Columns)), Values(std::move(Values)) {}
    Bytecode EmitBytecode() const override;
};

struct UpdateAST : public ExpressionAST {
    std::string TableName;
    std::vector<std::pair<std::string, std::string>> Assignments;
    std::unique_ptr<ExpressionAST> Condition;

    UpdateAST(std::string TableName, 
              std::vector<std::pair<std::string, std::string>> Assignments,
              std::unique_ptr<ExpressionAST> Condition)
        : TableName(std::move(TableName)),  Assignments(std::move(Assignments)), 
          Condition(std::move(Condition)) {}
    Bytecode EmitBytecode() const override;
};

struct DeleteAST : public ExpressionAST {
    std::string TableName;
    std::unique_ptr<ExpressionAST> Condition;

    DeleteAST(std::string TableName, std::unique_ptr<ExpressionAST> Condition)
        : TableName(std::move(TableName)), Condition(std::move(Condition)) {}
    Bytecode EmitBytecode() const override;
};

struct BinaryOpAST : public ExpressionAST {
    std::unique_ptr<ExpressionAST> LHS, RHS;
    std::string Op;

    BinaryOpAST(std::unique_ptr<ExpressionAST> LHS, std::string Op, std::unique_ptr<ExpressionAST> RHS)
        : LHS(std::move(LHS)), Op(std::move(Op)), RHS(std::move(RHS)) {}
    Bytecode EmitBytecode() const override;
};

using ASTNode = std::unique_ptr<ExpressionAST>;
using ASTType = std::vector<ASTNode>;

extern ASTType AST;

class Parser {
    std::string_view Query_;
    TokenStream Tokens_;
    size_t CurrentIndex_ = 0;

    int GetTokenPrecedence(const Token &Token);

    bool IsEOF() { return CurrentIndex_ >= Tokens_.size(); }

    TokenStream Tokenize();

    std::unique_ptr<ExpressionAST> ParsePrimary();

    bool IsConstraint(const std::string &TokenValue);

    std::optional<Token> CurrentToken() const {
        if(CurrentIndex_ < Tokens_.size()) return Tokens_[CurrentIndex_];
        return std::nullopt;
    }

    void AdvanceToken() {
        while(CurrentIndex_ < Tokens_.size() && 
            (IsEOF() || 
                std::all_of(Tokens_[CurrentIndex_].Value.begin(), Tokens_[CurrentIndex_].Value.end(), [](unsigned char c) {
                    return std::isspace(c);
                }))) {
            ++CurrentIndex_;
        }
    }

    bool MatchToken(TokenType ExpectedType) {
        if(auto Token = CurrentToken()) {
            if(Token->Type == ExpectedType) {
                AdvanceToken();
                return true;
            }
        }
        return false;
    }

    bool MatchKeyword(const std::string &ExpectedKeyword) {
        if(auto Token = CurrentToken()) {
            if(Token->Value == ExpectedKeyword) {
                AdvanceToken();
                return true;
            }
        }
        return false;
    }

    Tree<std::vector<std::unique_ptr<ExpressionAST>>> BuildAST() const;

    std::unique_ptr<ExpressionAST> ParseExpression();
    std::unique_ptr<ExpressionAST> ParseCreateStatement();
    std::unique_ptr<ExpressionAST> ParseSelectStatement();
    std::unique_ptr<ExpressionAST> ParseInsertStatement();
    std::unique_ptr<ExpressionAST> ParseUpdateStatement();
    std::unique_ptr<ExpressionAST> ParseDeleteStatement();
    std::unique_ptr<ExpressionAST> ParseWhereClause();
    std::unique_ptr<ExpressionAST> ParseBinaryOperation();
    std::unique_ptr<ExpressionAST> ParseBinaryOperation(int MinPrec, std::unique_ptr<ExpressionAST> LHS);
public:
    explicit Parser(std::string_view Query) : Query_(Query) {
        Tokens_ = Tokenize();
        CurrentIndex_ = 0;
        try { 
            ParseStatement(); 
        }
        catch(...) {
            std::cout << "Something went wrong\n";
        }
        auto NewAST = BuildAST();
        AST.clear();
        AST = std::move(NewAST.GetRoot());
        DumpTokens();
    }

    std::unique_ptr<ExpressionAST> ParseStatement();

    void DumpTokens() const;

    void DumpAST();
};
}
}
