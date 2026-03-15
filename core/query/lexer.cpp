// FluxDB - Professional Database Lexer
// Tokenizes GQL/SQL queries for the query engine

#include "../headers/lexer.h"
#include <cctype>
#include <stdexcept>
#include <algorithm>

namespace fluxdb {
namespace query {

// ─────────────────────────────────────────
//  Constructor
// ─────────────────────────────────────────

Lexer::Lexer(std::string_view sql) : sql_(sql), pos_(0) {}

// ─────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────

char Lexer::current() {
    if (pos_ >= sql_.size()) return '\0';
    return sql_[pos_];
}

char Lexer::peek_next() {
    if (pos_ + 1 >= sql_.size()) return '\0';
    return sql_[pos_ + 1];
}

void Lexer::skip_whitespace() {
    while (pos_ < sql_.size() && std::isspace((unsigned char)sql_[pos_])) {
        pos_++;
    }
}

// ─────────────────────────────────────────
//  Readers
// ─────────────────────────────────────────

Token Lexer::read_keyword() {
    size_t start = pos_;

    while (pos_ < sql_.size() && (std::isalnum((unsigned char)sql_[pos_]) || sql_[pos_] == '_')) {
        pos_++;
    }

    std::string lexeme = std::string(sql_.substr(start, pos_ - start));
    std::string upper = lexeme;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return std::toupper(c); });

    // Keywords SQL estandar
    if (upper == "SELECT")   return {TokenType::SELECT,   lexeme, start};
    if (upper == "FROM")     return {TokenType::FROM,     lexeme, start};
    if (upper == "TO")       return {TokenType::TO,       lexeme, start};
    if (upper == "WHERE")    return {TokenType::WHERE,    lexeme, start};
    if (upper == "AND")      return {TokenType::AND,      lexeme, start};
    if (upper == "OR")       return {TokenType::OR,       lexeme, start};
    if (upper == "NOT")      return {TokenType::NOT,      lexeme, start};
    if (upper == "INSERT")   return {TokenType::INSERT,   lexeme, start};
    if (upper == "INTO")     return {TokenType::INTO,     lexeme, start};
    if (upper == "VALUES")   return {TokenType::VALUES,   lexeme, start};
    if (upper == "UPDATE")   return {TokenType::UPDATE,   lexeme, start};
    if (upper == "SET")      return {TokenType::SET,      lexeme, start};
    if (upper == "DELETE")   return {TokenType::DELETE,   lexeme, start};
    if (upper == "CREATE")   return {TokenType::CREATE,   lexeme, start};
    if (upper == "TABLE")    return {TokenType::TABLE,    lexeme, start};
    if (upper == "DROP")     return {TokenType::DROP,     lexeme, start};
    if (upper == "ALTER")    return {TokenType::ALTER,    lexeme, start};
    if (upper == "INDEX")    return {TokenType::INDEX,    lexeme, start};
    if (upper == "JOIN")     return {TokenType::JOIN,     lexeme, start};
    if (upper == "ON")       return {TokenType::ON,       lexeme, start};
    if (upper == "LIMIT")    return {TokenType::LIMIT,    lexeme, start};
    if (upper == "OFFSET")   return {TokenType::OFFSET,   lexeme, start};
    if (upper == "ORDER")    return {TokenType::ORDER,    lexeme, start};
    if (upper == "BY")       return {TokenType::BY,       lexeme, start};
    if (upper == "ASC")      return {TokenType::ASC,      lexeme, start};
    if (upper == "DESC")     return {TokenType::DESC,     lexeme, start};
    if (upper == "GROUP")    return {TokenType::GROUP,    lexeme, start};
    if (upper == "HAVING")   return {TokenType::HAVING,   lexeme, start};
    if (upper == "DISTINCT") return {TokenType::DISTINCT, lexeme, start};

    // Tipos de datos
    if (upper == "INT")      return {TokenType::INT,      lexeme, start};
    if (upper == "FLOAT")    return {TokenType::FLOAT,    lexeme, start};
    if (upper == "TEXT")     return {TokenType::TEXT,     lexeme, start};
    if (upper == "BOOL")     return {TokenType::BOOL,     lexeme, start};
    if (upper == "NULL")     return {TokenType::KNULL,    lexeme, start};

    // Booleanos
    if (upper == "TRUE")     return {TokenType::BTRUE,    lexeme, start};
    if (upper == "FALSE")    return {TokenType::BFALSE,   lexeme, start};

    //Mundos e Entidades
    if (upper == "SPAWN")      return {TokenType::SPAWN,      lexeme, start}; // crear entidad
    if (upper == "DESPAWN")    return {TokenType::DESPAWN,     lexeme, start}; // eliminar entidad
    if (upper == "ENTITY")     return {TokenType::ENTITY,      lexeme, start};
    if (upper == "COMPONENT")  return {TokenType::COMPONENT,   lexeme, start};
    if (upper == "PREFAB")     return {TokenType::PREFAB,      lexeme, start}; // plantilla de entidad
    if (upper == "NEAR")       return {TokenType::NEAR,        lexeme, start}; // NEAR (x, y) WITHIN 50
    if (upper == "LISTEN")     return {TokenType::LISTEN,      lexeme, start};

    //Especiales
    if (upper == "WITHIN")     return {TokenType::WITHIN,      lexeme, start};
    if (upper == "OUTSIDE")    return {TokenType::OUTSIDE,     lexeme, start};
    if (upper == "OVERLAPS")   return {TokenType::OVERLAPS,    lexeme, start}; // colision AABB
    if (upper == "RAYCAST")    return {TokenType::RAYCAST,     lexeme, start}; // query de rayo

    //Tiempo / ticks
    if (upper == "SINCE")      return {TokenType::SINCE,       lexeme, start}; // SINCE 60 TICKS
    if (upper == "TICKS")      return {TokenType::TICKS,       lexeme, start};
    if (upper == "FRAMES")     return {TokenType::FRAMES,      lexeme, start};
    if (upper == "EVERY")      return {TokenType::EVERY,       lexeme, start}; // EVERY 10 TICKS
    if (upper == "AFTER")      return {TokenType::AFTER,       lexeme, start};
    if (upper == "AT")         return {TokenType::AT,          lexeme, start};

    //Saves / Snapshots
    if (upper == "SAVE")       return {TokenType::SAVE,        lexeme, start};
    if (upper == "LOAD")       return {TokenType::LOAD,        lexeme, start};
    if (upper == "SNAPSHOT")   return {TokenType::SNAPSHOT,    lexeme, start};
    if (upper == "RESTORE")    return {TokenType::RESTORE,     lexeme, start};
    if (upper == "CHECKPOINT") return {TokenType::CHECKPOINT,  lexeme, start};

    // Tipos nativos de juegos
    if (upper == "VEC2")    return {TokenType::VECTOR2,     lexeme, start};
    if (upper == "VEC3")    return {TokenType::VECTOR3,     lexeme, start};
    if (upper == "COLOR")      return {TokenType::COLOR,       lexeme, start};
    if (upper == "RECT")       return {TokenType::RECT,        lexeme, start};
    if (upper == "CIRCLE")     return {TokenType::CIRCLE,      lexeme, start};
    if (upper == "RAY")        return {TokenType::RAY,         lexeme, start};
    if (upper == "QUATERNION") return {TokenType::QUATERNION,    lexeme, start};
    if (upper == "AABB")       return {TokenType::AABB,        lexeme, start};

    // 🔔 Eventos

    if (upper == "EMIT")       return {TokenType::EMIT,        lexeme, start};
    if (upper == "ON")         return {TokenType::ON,          lexeme, start};
    if (upper == "ONCE")       return {TokenType::ONCE,        lexeme, start};
    if (upper == "LISTENER")   return {TokenType::LISTENER,    lexeme, start};
    if (upper == "EVENT")      return {TokenType::EVENT,       lexeme, start};

    // Queries avanzadas
    if (upper == "FIND")       return {TokenType::FIND,       lexeme, start}; // alternativa a SELECT para juegos
    if (upper == "ALL")        return {TokenType::ALL,        lexeme, start}; // FIND ALL entities
    if (upper == "ANY")        return {TokenType::ANY,        lexeme, start};
    if (upper == "IN")         return {TokenType::IN,         lexeme, start}; // WHERE tag IN ("enemy", "boss")
    if (upper == "BETWEEN")    return {TokenType::BETWEEN,    lexeme, start}; // WHERE health BETWEEN 0 AND 100
    if (upper == "LIKE")       return {TokenType::LIKE,       lexeme, start}; // WHERE name LIKE "gob%"
    if (upper == "IS")         return {TokenType::IS,         lexeme, start}; // WHERE component IS NULL

    // ECS
    if (upper == "HAS")        return {TokenType::HAS,        lexeme, start}; // WHERE HAS COMPONENT health
    if (upper == "WITH")       return {TokenType::WITH,       lexeme, start}; // SPAWN prefab WITH health = 100
    if (upper == "ATTACH")     return {TokenType::ATTACH,     lexeme, start}; // ATTACH COMPONENT a entidad
    if (upper == "DETACH")     return {TokenType::DETACH,     lexeme, start}; // DETACH COMPONENT de entidad
    if (upper == "WORLD")      return {TokenType::WORLD,      lexeme, start}; // referencia al mundo activo

    // Transacciones
    if (upper == "BEGIN")      return {TokenType::BEGIN,      lexeme, start}; // BEGIN TRANSACTION
    if (upper == "COMMIT")     return {TokenType::COMMIT,     lexeme, start};
    if (upper == "ROLLBACK")   return {TokenType::ROLLBACK,   lexeme, start};
    if (upper == "TRANSACTION") return {TokenType::TRANSACTION, lexeme, start};

    // Agregaciones
    if (upper == "COUNT")      return {TokenType::COUNT,      lexeme, start}; // COUNT(entities)
    if (upper == "SUM")        return {TokenType::SUM,        lexeme, start};
    if (upper == "AVG")        return {TokenType::AVG,        lexeme, start};
    if (upper == "MIN")        return {TokenType::MIN,        lexeme, start};
    if (upper == "MAX")        return {TokenType::MAX,        lexeme, start};

    // Tipos extra
    if (upper == "TRANSFORM")  return {TokenType::TRANSFORM,  lexeme, start}; // tipo nativo Transform
    if (upper == "UUID")       return {TokenType::UUID,       lexeme, start}; // IDs únicos
    if (upper == "TIMESTAMP")  return {TokenType::TIMESTAMP,  lexeme, start}; // tiempo de creación
    if (upper == "ARRAY")      return {TokenType::ARRAY,      lexeme, start}; // listas de componentes

    // Si no matchea nada → es un identificador
    return {TokenType::IDENT, lexeme, start};
}

Token Lexer::read_quoted_identifier() {
    char open  = sql_[pos_];
    char close = (open == '[') ? ']' : open; // [ident] o `ident` o "ident"
    pos_++; // saltar apertura
    size_t start = pos_;

    while (pos_ < sql_.size() && sql_[pos_] != close) {
        pos_++;
    }

    std::string ident = std::string(sql_.substr(start, pos_ - start));

    if (pos_ < sql_.size()) pos_++; // saltar cierre

    return {TokenType::IDENT, ident, start};
}

Token Lexer::read_number() {
    size_t start = pos_;
    bool has_dot = false;

    while (pos_ < sql_.size()) {
        char c = sql_[pos_];
        if (std::isdigit((unsigned char)c)) {
            pos_++;
        } else if (c == '.' && !has_dot && std::isdigit((unsigned char)peek_next())) {
            has_dot = true;
            pos_++;
        } else {
            break;
        }
    }

    std::string num = std::string(sql_.substr(start, pos_ - start));
    TokenType type  = has_dot ? TokenType::FLOAT_LITERAL : TokenType::INT_LITERAL;
    return {type, num, start};
}

Token Lexer::read_string_literal() {
    pos_++; // saltar comilla inicial '
    size_t start = pos_;

    while (pos_ < sql_.size() && sql_[pos_] != '\'') {
        // Soporte para escape \'
        if (sql_[pos_] == '\\' && pos_ + 1 < sql_.size() && sql_[pos_ + 1] == '\'') {
            pos_ += 2;
        } else {
            pos_++;
        }
    }

    std::string str = std::string(sql_.substr(start, pos_ - start));

    if (pos_ < sql_.size()) pos_++; // saltar comilla final '

    return {TokenType::STRING_LITERAL, str, start};
}

// ─────────────────────────────────────────
//  Tokenizer principal
// ─────────────────────────────────────────

Token Lexer::next_token() {
    skip_whitespace();

    if (pos_ >= sql_.size()) {
        return {TokenType::END_OF_FILE, "", pos_};
    }

    char c = current();

    // Identificadores y keywords
    if (std::isalpha((unsigned char)c) || c == '_') {
        return read_keyword();
    }

    // Identificadores entre comillas/corchetes
    if (c == '`' || c == '[' || c == '"') {
        return read_quoted_identifier();
    }

    // Numeros
    if (std::isdigit((unsigned char)c)) {
        return read_number();
    }

    // String literals con comilla simple
    if (c == '\'') {
        return read_string_literal();
    }

    // Comentarios -- (ignorar hasta fin de linea)
    if (c == '-' && peek_next() == '-') {
        while (pos_ < sql_.size() && sql_[pos_] != '\n') pos_++;
        return next_token(); // llamada recursiva para el siguiente token real
    }

    // En next_token(), después del check de comentarios --
    if (c == '/' && peek_next() == '*') {
        pos_ += 2;
        while (pos_ < sql_.size()) {
            if (sql_[pos_] == '*' && peek_next() == '/') {
                pos_ += 2;
                break;
            }
            pos_++;
        }
        return next_token();
    }

    // Operadores y simbolos
    switch (c) {
        case '=':
            pos_++;
            return {TokenType::EQ, "=", pos_ - 1};

        case '<':
            if (peek_next() == '=') {
                pos_ += 2;
                return {TokenType::LE, "<=", pos_ - 2};
            }
            if (peek_next() == '>') {
                pos_ += 2;
                return {TokenType::NE, "<>", pos_ - 2};
            }
            pos_++;
            return {TokenType::LT, "<", pos_ - 1};

        case '>':
            if (peek_next() == '=') {
                pos_ += 2;
                return {TokenType::GE, ">=", pos_ - 2};
            }
            pos_++;
            return {TokenType::GT, ">", pos_ - 1};

        case '!':
            if (peek_next() == '=') {
                pos_ += 2;
                return {TokenType::NE, "!=", pos_ - 2};
            }
            pos_++;
            return {TokenType::UNKNOWN, "!", pos_ - 1};

        case '+':
            pos_++;
            return {TokenType::PLUS, "+", pos_ - 1};

        case '-':
            pos_++;
            return {TokenType::MINUS, "-", pos_ - 1};

        case '*':
            pos_++;
            return {TokenType::ASTERISK, "*", pos_ - 1};

        case '/':
            pos_++;
            return {TokenType::SLASH, "/", pos_ - 1};

        case ',':
            pos_++;
            return {TokenType::COMMA, ",", pos_ - 1};

        case '(':
            pos_++;
            return {TokenType::LPAREN, "(", pos_ - 1};

        case ')':
            pos_++;
            return {TokenType::RPAREN, ")", pos_ - 1};

        case '[':
            pos_++;
            return {TokenType::LBRACKET, "[", pos_ - 1};

        case ']':
            pos_++;
            return {TokenType::RBRACKET, "]", pos_ - 1};

        case ';':
            pos_++;
            return {TokenType::SEMICOLON, ";", pos_ - 1};
        
        case '{':
            pos_++;
            return {TokenType::LBRACE, "{", pos_ - 1};

        case '}':
            pos_++;
            return {TokenType::RBRACE, "}", pos_ - 1};

        case '.':
            pos_++;
            return {TokenType::DOT, ".", pos_ - 1}; // para entity.health

        case ':':
            pos_++;
            return {TokenType::COLON, ":", pos_ - 1}; // para SPAWN prefab { health: 100 }

        case '%':
            pos_++;
            return {TokenType::PERCENT, "%", pos_ - 1}; // para modulo

        case '&':
            if (peek_next() == '&') {
                pos_ += 2;
                return {TokenType::AND_AND, "&&", pos_ - 2};
            }
            pos_++;
            return {TokenType::UNKNOWN, "&", pos_ - 1};

        case '|':
            if (peek_next() == '|') {
                pos_ += 2;
                return {TokenType::OR_OR, "||", pos_ - 2};
            }
            pos_++;
            return {TokenType::UNKNOWN, "|", pos_ - 1};

        default:
            pos_++;
            return {TokenType::UNKNOWN, std::string(1, c), pos_ - 1};
    }
}



} // namespace query
} // namespace veldradb