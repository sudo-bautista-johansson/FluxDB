#pragma once
#include "lexer.h"
#include <memory>
#include <vector>
#include <string>
#include <cstdint>

namespace fluxdb {
namespace query {

// ─────────────────────────────────────────
//  Nodos base del AST
// ─────────────────────────────────────────

struct ASTNode {
    virtual ~ASTNode() = default;
};

// ─────────────────────────────────────────
//  Expresiones
// ─────────────────────────────────────────

struct LiteralExpr : ASTNode {
    Token token;
};

struct IdentExpr : ASTNode {
    std::string name;
};

struct CallExpr : ASTNode {
    std::string function_name;
    std::vector<std::unique_ptr<ASTNode>> arguments;
};

struct BinaryExpr : ASTNode {
    std::unique_ptr<ASTNode> left;
    Token op;
    std::unique_ptr<ASTNode> right;
};

struct UnaryExpr : ASTNode {
    Token op;
    std::unique_ptr<ASTNode> operand;
};

// ─────────────────────────────────────────
//  Clausulas
// ─────────────────────────────────────────

struct WhereClause {
    std::unique_ptr<ASTNode> condition;
};

struct LimitClause {
    int value = -1;
};

struct OrderByClause {
    std::string column;
    bool ascending = true;
};

// ─────────────────────────────────────────
//  Statements SQL
// ─────────────────────────────────────────

struct SelectStatement : ASTNode {
    std::vector<std::string> columns;
    std::string table;
    std::unique_ptr<WhereClause> where;
    std::unique_ptr<LimitClause> limit;
    std::unique_ptr<OrderByClause> order_by;
    bool distinct = false;
    uint64_t at_tick = UINT64_MAX; // Time-Travel: UINT64_MAX means current live state
    bool is_listen = false; // Spatial Pub/Sub
};

struct InsertStatement : ASTNode {
    std::string table;
    std::vector<std::string> columns;
    std::vector<std::unique_ptr<ASTNode>> values;
};

struct UpdateStatement : ASTNode {
    std::string table;
    std::vector<std::pair<std::string, std::unique_ptr<ASTNode>>> assignments;
    std::unique_ptr<WhereClause> where;
};

struct DeleteStatement : ASTNode {
    std::string table;
    std::unique_ptr<WhereClause> where;
};

struct CreateTableStatement : ASTNode {
    std::string table_name;
    std::vector<std::pair<std::string, std::string>> columns; // nombre, tipo
};

struct DropTableStatement : ASTNode {
    std::string table_name;
};

// ─────────────────────────────────────────
//  Statements de juegos
// ─────────────────────────────────────────

struct FindStatement : ASTNode {
    std::string table;
    std::unique_ptr<WhereClause> where;
    std::unique_ptr<LimitClause> limit;
    bool has_near = false;
    float near_x = 0, near_y = 0, near_z = 0;
    float within = 0;
};

struct SpawnStatement : ASTNode {
    std::string prefab;
    std::vector<std::pair<std::string, std::unique_ptr<ASTNode>>> components;
};

struct SnapshotStatement : ASTNode {
    std::string slot;
};

struct RestoreStatement : ASTNode {
    std::string slot;
};

// ─────────────────────────────────────────
//  Parser
// ─────────────────────────────────────────

class Parser {
public:
    explicit Parser(std::string_view sql);
    std::unique_ptr<ASTNode> parse();

private:
    Lexer lexer_;
    Token current_;
    Token peek_;

    void advance();
    Token expect(TokenType type, const std::string& msg);
    bool check(TokenType type);
    bool match(TokenType type);

    std::unique_ptr<ASTNode> parse_statement();

    // SQL
    std::unique_ptr<SelectStatement>      parse_select();
    std::unique_ptr<InsertStatement>      parse_insert();
    std::unique_ptr<UpdateStatement>      parse_update();
    std::unique_ptr<DeleteStatement>      parse_delete();
    std::unique_ptr<CreateTableStatement> parse_create_table();
    std::unique_ptr<DropTableStatement>   parse_drop_table();

    // Juegos
    std::unique_ptr<FindStatement>      parse_find();
    std::unique_ptr<SpawnStatement>     parse_spawn();
    std::unique_ptr<SnapshotStatement>  parse_snapshot();
    std::unique_ptr<RestoreStatement>   parse_restore();

    // Clausulas
    std::unique_ptr<WhereClause>   parse_where();
    std::unique_ptr<LimitClause>   parse_limit();
    std::unique_ptr<OrderByClause> parse_order_by();

    // Expresiones (por precedencia)
    std::unique_ptr<ASTNode> parse_expression();
    std::unique_ptr<ASTNode> parse_or();
    std::unique_ptr<ASTNode> parse_and();
    std::unique_ptr<ASTNode> parse_comparison();
    std::unique_ptr<ASTNode> parse_term();
    std::unique_ptr<ASTNode> parse_factor();
    std::unique_ptr<ASTNode> parse_unary();
    std::unique_ptr<ASTNode> parse_primary();

    // Auxiliares
    float parse_number_as_float();
};

} // namespace query
} // namespace veldradb