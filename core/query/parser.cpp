// VeldraDB - Parser
// Converts tokens into an AST compatible with the Planner

#include "../headers/parser.h"
#include <stdexcept>
#include <string_view>

namespace fluxdb {
namespace query {

// ─────────────────────────────────────────
//  Constructor
// ─────────────────────────────────────────

Parser::Parser(std::string_view sql) : lexer_(sql) {
    current_ = lexer_.next_token();
    peek_    = lexer_.next_token();
}

// ─────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────

void Parser::advance() {
    current_ = peek_;
    peek_    = lexer_.next_token();
}

bool Parser::check(TokenType type) {
    return current_.type == type;
}

bool Parser::match(TokenType type) {
    if (check(type)) { advance(); return true; }
    return false;
}

Token Parser::expect(TokenType type, const std::string& msg) {
    if (!check(type)) {
        throw std::runtime_error(
            "Error en pos " + std::to_string(current_.pos) +
            ": se esperaba " + msg +
            ", se encontro '" + current_.value + "'"
        );
    }
    Token t = current_;
    advance();
    return t;
}

// ─────────────────────────────────────────
//  Entry point
// ─────────────────────────────────────────

std::unique_ptr<ASTNode> Parser::parse() {
    auto node = parse_statement();
    match(TokenType::SEMICOLON);
    return node;
}

std::unique_ptr<ASTNode> Parser::parse_statement() {
    if (check(TokenType::LISTEN)) {
        advance();
        auto stmt = parse_select();
        stmt->is_listen = true;
        return stmt;
    }
    if (check(TokenType::SELECT))   return parse_select();
    if (check(TokenType::INSERT))   return parse_insert();
    if (check(TokenType::UPDATE))   return parse_update();
    if (check(TokenType::DELETE))   return parse_delete();
    if (check(TokenType::CREATE))   return parse_create_table();
    if (check(TokenType::DROP))     return parse_drop_table();
    if (check(TokenType::FIND))     return parse_find();
    if (check(TokenType::SPAWN))    return parse_spawn();
    if (check(TokenType::SNAPSHOT)) return parse_snapshot();
    if (check(TokenType::RESTORE))  return parse_restore();

    throw std::runtime_error(
        "Statement desconocido: '" + current_.value + "'"
    );
}

// ─────────────────────────────────────────
//  SELECT
// ─────────────────────────────────────────

std::unique_ptr<SelectStatement> Parser::parse_select() {
    auto stmt = std::make_unique<SelectStatement>();
    expect(TokenType::SELECT, "SELECT");

    if (check(TokenType::DISTINCT)) {
        stmt->distinct = true;
        advance();
    }

    // Columnas: * o lista
    if (match(TokenType::ASTERISK)) {
        stmt->columns.push_back("*");
    } else {
        stmt->columns.push_back(expect(TokenType::IDENT, "columna").value);
        while (match(TokenType::COMMA))
            stmt->columns.push_back(expect(TokenType::IDENT, "columna").value);
    }

    expect(TokenType::FROM, "FROM");
    stmt->table = expect(TokenType::IDENT, "nombre de tabla").value;

    // Deterministic Time-Travel parsing: AT TICK X
    if (match(TokenType::AT)) {
        if (check(TokenType::TICKS) || check(TokenType::IDENT)) { 
            advance(); 
        }
        stmt->at_tick = std::stoull(expect(TokenType::INT_LITERAL, "numero de tick").value);
    }

    if (check(TokenType::WHERE))    stmt->where    = parse_where();
    if (check(TokenType::ORDER))    stmt->order_by = parse_order_by();
    if (check(TokenType::LIMIT))    stmt->limit    = parse_limit();

    return stmt;
}

// ─────────────────────────────────────────
//  INSERT
// ─────────────────────────────────────────

std::unique_ptr<InsertStatement> Parser::parse_insert() {
    auto stmt = std::make_unique<InsertStatement>();
    expect(TokenType::INSERT, "INSERT");
    expect(TokenType::INTO,   "INTO");
    stmt->table = expect(TokenType::IDENT, "nombre de tabla").value;

    if (match(TokenType::LPAREN)) {
        stmt->columns.push_back(expect(TokenType::IDENT, "columna").value);
        while (match(TokenType::COMMA))
            stmt->columns.push_back(expect(TokenType::IDENT, "columna").value);
        expect(TokenType::RPAREN, ")");
    }

    expect(TokenType::VALUES, "VALUES");
    expect(TokenType::LPAREN, "(");
    stmt->values.push_back(parse_primary());
    while (match(TokenType::COMMA))
        stmt->values.push_back(parse_primary());
    expect(TokenType::RPAREN, ")");

    return stmt;
}

// ─────────────────────────────────────────
//  UPDATE
// ─────────────────────────────────────────

std::unique_ptr<UpdateStatement> Parser::parse_update() {
    auto stmt = std::make_unique<UpdateStatement>();
    expect(TokenType::UPDATE, "UPDATE");
    stmt->table = expect(TokenType::IDENT, "nombre de tabla").value;
    expect(TokenType::SET, "SET");

    do {
        std::string col = expect(TokenType::IDENT, "columna").value;
        expect(TokenType::EQ, "=");
        stmt->assignments.emplace_back(col, parse_primary());
    } while (match(TokenType::COMMA));

    if (check(TokenType::WHERE)) stmt->where = parse_where();

    return stmt;
}

// ─────────────────────────────────────────
//  DELETE
// ─────────────────────────────────────────

std::unique_ptr<DeleteStatement> Parser::parse_delete() {
    auto stmt = std::make_unique<DeleteStatement>();
    expect(TokenType::DELETE, "DELETE");
    expect(TokenType::FROM, "FROM");
    stmt->table = expect(TokenType::IDENT, "nombre de tabla").value;

    if (check(TokenType::WHERE)) stmt->where = parse_where();

    return stmt;
}

// ─────────────────────────────────────────
//  CREATE TABLE
// ─────────────────────────────────────────

std::unique_ptr<CreateTableStatement> Parser::parse_create_table() {
    auto stmt = std::make_unique<CreateTableStatement>();
    expect(TokenType::CREATE, "CREATE");
    expect(TokenType::TABLE,  "TABLE");
    stmt->table_name = expect(TokenType::IDENT, "nombre de tabla").value;

    expect(TokenType::LPAREN, "(");
    do {
        std::string col_name = expect(TokenType::IDENT, "nombre de columna").value;
        std::string col_type = expect(TokenType::IDENT, "tipo de columna").value;
        stmt->columns.emplace_back(col_name, col_type);
    } while (match(TokenType::COMMA));
    expect(TokenType::RPAREN, ")");

    return stmt;
}

// ─────────────────────────────────────────
//  DROP TABLE
// ─────────────────────────────────────────

std::unique_ptr<DropTableStatement> Parser::parse_drop_table() {
    auto stmt = std::make_unique<DropTableStatement>();
    expect(TokenType::DROP,  "DROP");
    expect(TokenType::TABLE, "TABLE");
    stmt->table_name = expect(TokenType::IDENT, "nombre de tabla").value;
    return stmt;
}

// ─────────────────────────────────────────
//  FIND
// ─────────────────────────────────────────

std::unique_ptr<FindStatement> Parser::parse_find() {
    auto stmt = std::make_unique<FindStatement>();
    expect(TokenType::FIND, "FIND");
    stmt->table = expect(TokenType::IDENT, "nombre de tabla").value;

    if (match(TokenType::NEAR)) {
        stmt->has_near = true;
        expect(TokenType::LPAREN, "(");
        stmt->near_x = parse_number_as_float();
        expect(TokenType::COMMA, ",");
        stmt->near_y = parse_number_as_float();
        if (match(TokenType::COMMA))
            stmt->near_z = parse_number_as_float();
        expect(TokenType::RPAREN, ")");
        expect(TokenType::WITHIN, "WITHIN");
        stmt->within = parse_number_as_float();
    }

    if (check(TokenType::WHERE)) stmt->where = parse_where();
    if (check(TokenType::LIMIT)) stmt->limit = parse_limit();

    return stmt;
}

// ─────────────────────────────────────────
//  SPAWN
// ─────────────────────────────────────────

std::unique_ptr<SpawnStatement> Parser::parse_spawn() {
    auto stmt = std::make_unique<SpawnStatement>();
    expect(TokenType::SPAWN, "SPAWN");
    match(TokenType::PREFAB); // opcional
    stmt->prefab = expect(TokenType::STRING_LITERAL, "nombre de prefab").value;

    if (match(TokenType::WITH)) {
        do {
            std::string key = expect(TokenType::IDENT, "componente").value;
            expect(TokenType::EQ, "=");
            stmt->components.emplace_back(key, parse_primary());
        } while (match(TokenType::COMMA));
    }

    return stmt;
}

// ─────────────────────────────────────────
//  SNAPSHOT / RESTORE
// ─────────────────────────────────────────

std::unique_ptr<SnapshotStatement> Parser::parse_snapshot() {
    auto stmt = std::make_unique<SnapshotStatement>();
    expect(TokenType::SNAPSHOT, "SNAPSHOT");
    expect(TokenType::TO, "TO");
    stmt->slot = expect(TokenType::STRING_LITERAL, "slot").value;
    return stmt;
}

std::unique_ptr<RestoreStatement> Parser::parse_restore() {
    auto stmt = std::make_unique<RestoreStatement>();
    expect(TokenType::RESTORE, "RESTORE");
    expect(TokenType::FROM, "FROM");
    stmt->slot = expect(TokenType::STRING_LITERAL, "slot").value;
    return stmt;
}

// ─────────────────────────────────────────
//  Clausulas
// ─────────────────────────────────────────

std::unique_ptr<WhereClause> Parser::parse_where() {
    auto clause = std::make_unique<WhereClause>();
    expect(TokenType::WHERE, "WHERE");
    clause->condition = parse_expression();
    return clause;
}

std::unique_ptr<LimitClause> Parser::parse_limit() {
    auto clause = std::make_unique<LimitClause>();
    expect(TokenType::LIMIT, "LIMIT");
    clause->value = std::stoi(expect(TokenType::INT_LITERAL, "numero").value);
    return clause;
}

std::unique_ptr<OrderByClause> Parser::parse_order_by() {
    auto clause = std::make_unique<OrderByClause>();
    expect(TokenType::ORDER, "ORDER");
    expect(TokenType::BY, "BY");
    clause->column = expect(TokenType::IDENT, "columna").value;
    if (match(TokenType::DESC)) clause->ascending = false;
    else match(TokenType::ASC);
    return clause;
}

// ─────────────────────────────────────────
//  Expresiones
// ─────────────────────────────────────────

std::unique_ptr<ASTNode> Parser::parse_expression() {
    return parse_or();
}

std::unique_ptr<ASTNode> Parser::parse_or() {
    auto left = parse_and();
    while (check(TokenType::OR) || check(TokenType::OR_OR)) {
        Token op = current_; advance();
        auto node = std::make_unique<BinaryExpr>();
        node->left  = std::move(left);
        node->op    = op;
        node->right = parse_and();
        left = std::move(node);
    }
    return left;
}

std::unique_ptr<ASTNode> Parser::parse_and() {
    auto left = parse_comparison();
    while (check(TokenType::AND) || check(TokenType::AND_AND)) {
        Token op = current_; advance();
        auto node = std::make_unique<BinaryExpr>();
        node->left  = std::move(left);
        node->op    = op;
        node->right = parse_comparison();
        left = std::move(node);
    }
    return left;
}

std::unique_ptr<ASTNode> Parser::parse_comparison() {
    auto left = parse_term();
    while (check(TokenType::EQ)  || check(TokenType::NE) ||
           check(TokenType::LT)  || check(TokenType::GT) ||
           check(TokenType::LE)  || check(TokenType::GE)) {
        Token op = current_; advance();
        auto node = std::make_unique<BinaryExpr>();
        node->left  = std::move(left);
        node->op    = op;
        node->right = parse_term();
        left = std::move(node);
    }
    return left;
}

std::unique_ptr<ASTNode> Parser::parse_term() {
    auto left = parse_factor();
    while (check(TokenType::PLUS) || check(TokenType::MINUS)) {
        Token op = current_; advance();
        auto node = std::make_unique<BinaryExpr>();
        node->left  = std::move(left);
        node->op    = op;
        node->right = parse_factor();
        left = std::move(node);
    }
    return left;
}

std::unique_ptr<ASTNode> Parser::parse_factor() {
    auto left = parse_unary();
    while (check(TokenType::ASTERISK) || check(TokenType::SLASH) || check(TokenType::PERCENT)) {
        Token op = current_; advance();
        auto node = std::make_unique<BinaryExpr>();
        node->left  = std::move(left);
        node->op    = op;
        node->right = parse_unary();
        left = std::move(node);
    }
    return left;
}

std::unique_ptr<ASTNode> Parser::parse_unary() {
    if (check(TokenType::NOT) || check(TokenType::MINUS)) {
        Token op = current_; advance();
        auto node = std::make_unique<UnaryExpr>();
        node->op      = op;
        node->operand = parse_primary();
        return node;
    }
    return parse_primary();
}

std::unique_ptr<ASTNode> Parser::parse_primary() {
    if (check(TokenType::INT_LITERAL)    || check(TokenType::FLOAT_LITERAL) ||
        check(TokenType::STRING_LITERAL) || check(TokenType::BTRUE)         ||
        check(TokenType::BFALSE)         || check(TokenType::KNULL)) {
        auto node   = std::make_unique<LiteralExpr>();
        node->token = current_;
        advance();
        return node;
    }

    if (check(TokenType::IDENT)) {
        std::string name = current_.value;
        advance();
        
        if (match(TokenType::LPAREN)) {
            auto node = std::make_unique<CallExpr>();
            node->function_name = name;
            
            if (!check(TokenType::RPAREN)) {
                do {
                    node->arguments.push_back(parse_expression());
                } while (match(TokenType::COMMA));
            }
            expect(TokenType::RPAREN, ")");
            return node;
        }

        auto node  = std::make_unique<IdentExpr>();
        node->name = name;
        return node;
    }

    if (match(TokenType::LPAREN)) {
        auto expr = parse_expression();
        expect(TokenType::RPAREN, ")");
        return expr;
    }

    throw std::runtime_error(
        "Expresion inesperada: '" + current_.value + "'"
    );
}

float Parser::parse_number_as_float() {
    if (check(TokenType::INT_LITERAL) || check(TokenType::FLOAT_LITERAL)) {
        float val = std::stof(current_.value);
        advance();
        return val;
    }
    throw std::runtime_error("Se esperaba un numero literal (int o float)");
}

} // namespace query
} // namespace veldradb