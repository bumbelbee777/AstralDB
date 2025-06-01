#pragma once

#include <IO/Logger.hxx>
#include <Database/User.hxx>
#include <SQL/Bytecode.hxx>
#include <DS/Tree.hxx>
#include <DS/BPlusTree.hxx>
#include <DS/RadixTree.hxx>
#include <cstdio>
#include <optional>
#include <string>
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
        : LHS(std::move(LHS)), RHS(std::move(RHS)), Op(std::move(Op)) {}
    Bytecode EmitBytecode() const override;
};

struct GrantAST : public ExpressionAST {
    std::string Username;
    Permissions Perms;
    std::string TableName;
    GrantAST(std::string User, Permissions Perms, std::string Table = "")
        : Username(std::move(User)), Perms(Perms), TableName(std::move(Table)) {}
    Bytecode EmitBytecode() const override;
};

struct RevokeAST : public ExpressionAST {
    std::string UserName;
    Permissions Perms;
    std::string TableName;
    RevokeAST(std::string User, Permissions Permissions, std::string Table = "")
        : UserName(std::move(User)), Perms(Permissions), TableName(std::move(Table)) {}
    Bytecode EmitBytecode() const override;
};

using ASTNode = std::unique_ptr<ExpressionAST>;
using ASTType = std::vector<ASTNode>;

// Hybrid AST structure: switches between BPlusTree and RadixTree based on depth
class HybridAST {
	using KeyType = std::string;
	using ValueType = ASTNode;
	using BPTree = BPlusTree<KeyType, ValueType>;
	using RTree = DS::RadixTree<KeyType, ValueType>;

	static constexpr size_t SwitchDepth = 3;
	std::unique_ptr<BPTree> BPTreeRoot_;
	std::unique_ptr<RTree> RadixRoot_;
	bool UseRadix_ = false;

public:
	HybridAST();
	void Add(const KeyType &Key, ValueType Node, size_t Depth = 0);
	template<typename Func>
	void Traverse(Func &&F, size_t Depth = 0) const;
	bool Empty() const;
	ExpressionAST *Find(const KeyType &Key, size_t Depth = 0);
};

extern Tree<ASTNode> AST;

class Parser {
    std::string_view Query_;
    TokenStream Tokens_;
    size_t CurrentIndex_ = 0;

    int GetTokenPrecedence(const Token &Token);

    bool IsEOF() { return CurrentIndex_ >= Tokens_.size(); }

    TokenStream Tokenize();

    ASTNode ParsePrimary();

    bool IsConstraint(const std::string &TokenValue);
    
    bool IsKeyword(const std::string &TokenValue);
    
    std::optional<Token> CurrentToken() const {
        if(CurrentIndex_ < Tokens_.size()) return Tokens_[CurrentIndex_];
        return std::nullopt;
    }

    void AdvanceToken() {
        if (!IsEOF()) ++CurrentIndex_;
    }

    bool MatchToken(TokenType ExpectedType) {
        if(auto Token = CurrentToken(); Token && Token->Type == ExpectedType) {
            AdvanceToken();
            return true;
        }
        return false;
    }

    bool MatchToken(const Token &Other) {
        if(auto Token = CurrentToken(); MatchToken(Other.Type) && MatchKeyword(Other.Value))
            return true;
        return false;
    }

    bool MatchKeyword(const std::string &ExpectedKeyword) {
        if (auto Token = CurrentToken()) {
            if (Token->Type == TokenType::KEYWORD && Token->Value == ExpectedKeyword) {
                AdvanceToken();
                return true;
            }
        }
        return false;
    }

    bool MatchToken(TokenType ExpectedType, const std::string& ExpectedValue) {
        if (auto Token = CurrentToken()) {
            if (Token->Type == ExpectedType && Token->Value == ExpectedValue) {
                AdvanceToken();
                return true;
            }
        }
        return false;
    }

    Tree<ASTNode> BuildAST() const;

    ASTNode ParseExpression();
    ASTNode ParseCreateStatement();
    ASTNode ParseSelectStatement();
    ASTNode ParseInsertStatement();
    ASTNode ParseUpdateStatement();
    ASTNode ParseDeleteStatement();
    ASTNode ParseWhereClause();
    ASTNode ParseBinaryOperation();
    ASTNode ParseBinaryOperation(int MinPrec, ASTNode LHS);
    ASTNode ParseGrantStatement();
    ASTNode ParseRevokeStatement();
public:
    explicit Parser(std::string_view Query) : Query_(Query) {
        std::cout << "[Parser] Initializing with query:\n\"" << Query << "\"\n";
        // Tokenization phase
        std::cout << "[Tokenizer] Starting tokenization...\n";
        Tokens_ = Tokenize();
        std::cout << "[Tokenizer] Produced " << Tokens_.size() << " tokens:\n";
        for (const auto& token : Tokens_)
            std::cout << "[" << static_cast<int>(token.Type) << ": " << token.Value << "] ";
        std::cout << "\n";
        CurrentIndex_ = 0;
        // Parsing phase
        try {
            std::cout << "[Parser] Starting AST construction...\n";
            // Parse all statements, not just one!
            auto NewAST = BuildAST();
            AST = std::move(NewAST);
            std::cout << "[Parser] Successfully parsed all statements\n";
        }
        catch(const std::exception& e) {
            std::cerr << "[ERROR] Parsing failed: " << e.what() << "\n";
            throw;
        }
        // AST diagnostics
        std::cout << "[AST] Final AST structure:\n";
        DumpAST();
        std::cout << "[AST] Construction completed successfully\n";
    }

    std::unique_ptr<ExpressionAST> ParseStatement();

    void DumpTokens() const;

    void DumpAST() {
        std::function<void(const Tree<ASTNode>::Node&, int)> DumpNode = [&](const Tree<ASTNode>::Node& Node, int Depth) {
            std::cout << std::string(Depth * 2, ' ') << Node.Value->EmitBytecode() << '\n';
            for (const auto& Child : Node.Children)
                DumpNode(*Child, Depth + 1);
        };
    }
};

Bytecode BuildBytecode(Logger* Logger = nullptr);
}
}