#pragma once
#include <string>
#include <string_view>
#include <algorithm>

namespace fluxdb {
namespace query {

enum class TokenType {
    // ── Keywords SQL estándar ──────────────────
    SELECT, FROM, TO, WHERE, AND, OR, NOT,
    INSERT, INTO, VALUES,
    UPDATE, SET,
    DELETE,
    CREATE, TABLE, DROP, ALTER,
    INDEX, JOIN, ON,
    LIMIT, OFFSET, ORDER, BY, ASC, DESC,
    GROUP, HAVING, DISTINCT, LISTEN,

    // ── Tipos de datos básicos ─────────────────
    INT, FLOAT, TEXT, BOOL, KNULL,

    // ── Literales ─────────────────────────────
    INT_LITERAL, FLOAT_LITERAL, STRING_LITERAL,
    BTRUE, BFALSE,

    // ── Identificadores ───────────────────────
    IDENT,

    // ── Operadores ────────────────────────────
    EQ, NE, LT, GT, LE, GE,
    PLUS, MINUS, ASTERISK, SLASH, PERCENT,
    AND_AND, OR_OR,

    // ── Símbolos ──────────────────────────────
    COMMA, LPAREN, RPAREN, SEMICOLON,
    LBRACKET, RBRACKET,
    LBRACE, RBRACE,
    DOT, COLON,

    // ── Entidades / Mundo ─────────────────────
    SPAWN, DESPAWN, ENTITY, COMPONENT,
    TAG, PREFAB, WORLD,

    // ── Espaciales ────────────────────────────
    NEAR, WITHIN, OUTSIDE, OVERLAPS, RAYCAST,

    // ── Tiempo / Ticks ────────────────────────
    SINCE, TICKS, FRAMES, EVERY, AFTER, AT,

    // ── Saves / Snapshots ─────────────────────
    SAVE, LOAD, SNAPSHOT, RESTORE, CHECKPOINT,

    // ── Tipos nativos de juegos ───────────────
    VECTOR2, VECTOR3, COLOR, RECT, CIRCLE,
    RAY, QUATERNION, AABB, TRANSFORM,

    // ── Eventos ───────────────────────────────
    EMIT, ONCE, LISTENER, EVENT,

    // ── Queries avanzadas ─────────────────────
    FIND, ALL, ANY, IN, BETWEEN, LIKE, IS,

    // ── ECS ───────────────────────────────────
    HAS, WITH, ATTACH, DETACH,

    // ── Transacciones ─────────────────────────
    BEGIN, COMMIT, ROLLBACK, TRANSACTION,

    // ── Agregaciones ──────────────────────────
    COUNT, SUM, AVG, MIN, MAX,

    // ── Tipos extra ───────────────────────────
    UUID, TIMESTAMP, ARRAY,

    // ── Especiales ────────────────────────────
    END_OF_FILE,
    UNKNOWN
};

struct Token {
    TokenType type;
    std::string value;
    size_t pos;
};

class Lexer {
public:
    Lexer(std::string_view sql);
    Token next_token();

private:
    void skip_whitespace();
    Token read_keyword();
    Token read_quoted_identifier();
    Token read_number();
    Token read_string_literal();
    char current();
    char peek_next();

    std::string_view sql_;
    size_t pos_;
};

} // namespace query
} // namespace veldradb