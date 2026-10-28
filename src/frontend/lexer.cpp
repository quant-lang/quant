#include <cerrno>
#include <cmath>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>

#include "quant/frontend/lexer.h"
#include "quant/frontend/token.h"

constexpr unsigned int str_hash(const char* str, int h = 0){
    return !str[h] ? 5381 : (str_hash(str, h + 1) * 33) ^ str[h];
}
namespace quant::lx {
    static int digit_value(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 99;
    }
    static TokenType int_suffix_token(std::string_view text) {
        if (text == "i8")  return TOKEN_I8;
        if (text == "i16") return TOKEN_I16;
        if (text == "i32") return TOKEN_I32;
        if (text == "i64") return TOKEN_I64;
        if (text == "u8")  return TOKEN_U8;
        if (text == "u16") return TOKEN_U16;
        if (text == "u32") return TOKEN_U32;
        if (text == "u64") return TOKEN_U64;
        if (text == "u" || text == "U") return TOKEN_U32; // C-like shorthand
        return TOKEN_EOF;
    }
    char Lexer::peek() {
        if (pos >= buffer.size()) return '\0';
        return buffer[pos];
    }
    char Lexer::advance() {
        char c = peek();

        if (c == '\0') return c;

        pos++;

        if (c == '\n') {
            ctx.srcloc.line++;
            ctx.srcloc.column = 1;
        } else {
            ctx.srcloc.column++;
        }

        return c;
    }
    bool Lexer::match(char expected) {
        if (peek() != expected) return false;

        advance();
        return true;
    }
    bool Lexer::is_at_end() {
        return peek() == '\0';
    }
    Token Lexer::next_token() {
        while (std::isspace((unsigned char)peek())) {
            advance();
        }

        start = pos;
        token_line = ctx.srcloc.line;
        token_column = ctx.srcloc.column;

        char c = advance();

        if (std::isdigit((unsigned char)c)) {
            if (c == '0') {
                if (match('x') || match('X')) return radix_number(16);
                if (match('b') || match('B')) return radix_number(2);
                if (match('o') || match('O')) return radix_number(8);
            }
            return number();
        } else if (std::isalpha((unsigned char)c) || c == '_') {
            return identifier();
        } else if (c == '"') {
            return string();
        } else if (c == '\'') {
            return char_literal();
        }

        switch (c) {
            case '(': return make_token(TOKEN_LPAREN);
            case ')': return make_token(TOKEN_RPAREN);
            case '{': return make_token(TOKEN_LBRACE);
            case '}': return make_token(TOKEN_RBRACE);
            case '[': return make_token(TOKEN_LBRACKET);
            case ']': return make_token(TOKEN_RBRACKET);
            case ',': return make_token(TOKEN_COMMA);
            case '=':
                if(match('=')){
                    return make_token(TOKEN_EQEQ);
                }
                else if(match('>')){
                    return make_token(TOKEN_EQA);
                }
                else{
                    return make_token(TOKEN_EQ);
                }

            case '!': return make_token(match('=') ? TOKEN_NEQ : TOKEN_NOT);
            case '&':
                if (match('&')) return make_token(TOKEN_AMP_AMP);
                if (match('=')) return make_token(TOKEN_AMP_EQ);
                return make_token(TOKEN_AMP);
            case '|':
                if (match('|')) return make_token(TOKEN_PIPE_PIPE);
                if (match('=')) return make_token(TOKEN_PIPE_EQ);
                return make_token(TOKEN_PIPE);
            case '<': return make_token(match('=') ? TOKEN_LTE : TOKEN_LT);
            case '>': return make_token(match('=') ? TOKEN_GTE : TOKEN_GT);
            case '+':
                if(match('=')) return make_token(TOKEN_PLUS_EQ);
                if(match('+')) return make_token(TOKEN_PLUS_PLUS);
                return make_token(TOKEN_PLUS);
            case '-':
                if(match('=')) return make_token(TOKEN_MINUS_EQ);
                if(match('-')) return make_token(TOKEN_MINUS_MINUS);
                return make_token(TOKEN_MINUS);
            case '*': return make_token(match('=') ? TOKEN_STAR_EQ: TOKEN_STAR);
            case ';': return make_token(TOKEN_SEMICOLON);
            case '@': return make_token(TOKEN_AT);
            case '#': return make_token(TOKEN_HASH);
            case '.': return make_token(TOKEN_DOT);
            case ':': return make_token(match(':') ? TOKEN_COLON_COLON: TOKEN_COLON);
            case '/':
                if (match('/')) {
                    while (peek() != '\n' && !is_at_end()) advance();
                    return next_token();
                }
                else if (match('*')) {
                    while (!is_at_end()) {
                        if (peek() == '*' && pos + 1 < buffer.size() && buffer[pos + 1] == '/') {
                            advance();
                            advance();
                            break;
                        }
                        if (peek() == '\n') {
                            ctx.srcloc.line++;
                            ctx.srcloc.column = 1;
                        }
                        advance();
                    }
                    return next_token();
                }
                else if (match('=')){
                    return make_token(TOKEN_SLASH_EQ);
                }
                return make_token(TOKEN_SLASH);
            case '\0': return make_token(TOKEN_EOF);
            default: return make_token(TOKEN_ILLEGAL);
        }
    }
    Token Lexer::make_token(TokenType type) {
        return Token{
            type,
            std::string_view(buffer.data() + start, pos - start),
            {},
            0,
            {ctx.srcloc.file, token_line, token_column, static_cast<int>(pos - start)}
        };
    }
    Token Lexer::make_int(uint64_t v) {
        Token tok = make_token(TOKEN_INT);
        tok.inum = v;
        return tok;
    }

    Token Lexer::make_float(double v) {
        Token tok = make_token(TOKEN_FLOAT);
        tok.number = v;
        return tok;
    }

    TokenType Lexer::consume_int_suffix() {
        size_t suffix_start = pos;
        while (std::isalnum((unsigned char)peek()) || peek() == '_')
            advance();

        if (pos == suffix_start)
            return TOKEN_EOF;

        std::string_view text(buffer.data() + suffix_start, pos - suffix_start);
        TokenType type = int_suffix_token(text);
        return type == TOKEN_EOF ? TOKEN_ILLEGAL : type;
    }

    Token Lexer::radix_number(int base) {
        uint64_t value = 0;
        int digits = 0;
        bool overflow = false;

        while (true) {
            char c = peek();
            if (c == '_') {
                advance();
                continue;
            }

            if (!std::isalnum((unsigned char)c))
                break;

            int d = digit_value(c);
            if (d >= base) {
                // A letter/underscore here may start an explicit type suffix
                // (0xFFu8); an invalid base digit is always an error.
                if (std::isalpha((unsigned char)c) || c == '_')
                    break;

                while (std::isalnum((unsigned char)peek()) || peek() == '_')
                    advance();
                return make_token(TOKEN_ILLEGAL);
            }

            if (value > (UINT64_MAX - static_cast<uint64_t>(d)) /
                            static_cast<uint64_t>(base))
                overflow = true;
            value = value * static_cast<uint64_t>(base) + static_cast<uint64_t>(d);
            digits++;
            advance();
        }

        TokenType suffix = TOKEN_EOF;
        if (std::isalpha((unsigned char)peek())) {
            suffix = consume_int_suffix();
            if (suffix == TOKEN_ILLEGAL) return make_token(TOKEN_ILLEGAL);
        }

        if (digits == 0 || overflow) return make_token(TOKEN_ILLEGAL);

        Token tok = make_int(value);
        tok.suffix = suffix;
        return tok;
    }
    Token Lexer::number() {
        bool is_float = false;

        while (std::isdigit((unsigned char)peek()) || peek() == '_') advance();

        if (peek() == '.' && pos + 1 < buffer.size() &&
            std::isdigit((unsigned char)buffer[pos + 1])) {
            is_float = true;
            advance();
            while (std::isdigit((unsigned char)peek()) || peek() == '_')
                advance();
        }

        if (peek() == 'e' || peek() == 'E') {
            is_float = true;
            advance();

            if (peek() == '+' || peek() == '-')
                advance();

            if (!std::isdigit((unsigned char)peek())) {
                return make_token(TOKEN_ILLEGAL);   // 1e, 1e+
            }
            while (std::isdigit((unsigned char)peek()))
                advance();
        }

        // Keep the numeric part separate from an optional type suffix.
        const size_t numeric_end = pos;
        TokenType suffix = TOKEN_EOF;
        if (std::isalpha((unsigned char)peek())) {
            suffix = consume_int_suffix();
            if (suffix == TOKEN_ILLEGAL || is_float)
                return make_token(TOKEN_ILLEGAL);
        }

        std::string text;
        for (size_t i = start; i < numeric_end; i++)
            if (buffer[i] != '_')
                text.push_back(buffer[i]);

        if (is_float) {
            errno = 0;
            double v = std::strtod(text.c_str(), nullptr);
            if (errno == ERANGE || !std::isfinite(v))
                return make_token(TOKEN_ILLEGAL);
            return make_float(v);
        }

        errno = 0;
        uint64_t v = std::strtoull(text.c_str(), nullptr, 10);

        if (errno == ERANGE)
            return make_token(TOKEN_ILLEGAL);

        Token tok = make_int(v);
        tok.suffix = suffix;
        return tok;
    }
    Token Lexer::identifier() {
        while (std::isalnum((unsigned char)peek()) || peek() == '_') {
            advance();
        }

        std::string text(buffer.data() + start, pos - start);

        switch(str_hash(text.c_str())) {
            case str_hash("if"): return make_token(TOKEN_IF);
            case str_hash("else"): return make_token(TOKEN_ELSE);
            case str_hash("while"):return make_token(TOKEN_WHILE);
            case str_hash("for"):return make_token(TOKEN_FOR);
            case str_hash("switch"):return make_token(TOKEN_SWITCH);
            case str_hash("case"):return make_token(TOKEN_CASE);
            case str_hash("default"):return make_token(TOKEN_DEFAULT);
            case str_hash("return"):return make_token(TOKEN_RETURN);
            case str_hash("break"):return make_token(TOKEN_BREAK);
            case str_hash("continue"):return make_token(TOKEN_CONTINUE);
            case str_hash("func"): return make_token(TOKEN_FUNC);

            case str_hash("mut"): return make_token(TOKEN_MUT);
            case str_hash("struct"): return make_token(TOKEN_STRUCT);
            case str_hash("namespace"): return make_token(TOKEN_NAMESPACE);
            case str_hash("load"): return make_token(TOKEN_LOAD);
            case str_hash("using"): return make_token(TOKEN_USING);
            case str_hash("module"): return make_token(TOKEN_MODULE);
            case str_hash("extern"): return make_token(TOKEN_EXTERN);

            // Types
            case str_hash("void"): return make_token(TOKEN_VOID);
            case str_hash("bool"): return make_token(TOKEN_BOOL);
            case str_hash("i8"):   return make_token(TOKEN_I8);
            case str_hash("i16"):  return make_token(TOKEN_I16);
            case str_hash("i32"):  return make_token(TOKEN_I32);
            case str_hash("i64"):  return make_token(TOKEN_I64);
            case str_hash("u8"):   return make_token(TOKEN_U8);
            case str_hash("u16"):  return make_token(TOKEN_U16);
            case str_hash("u32"):  return make_token(TOKEN_U32);
            case str_hash("u64"):  return make_token(TOKEN_U64);
            case str_hash("f32"):  return make_token(TOKEN_F32);
            case str_hash("f64"):  return make_token(TOKEN_F64);
            case str_hash("str"):  return make_token(TOKEN_STR_TYPE);
            case str_hash("char"): return make_token(TOKEN_CHAR_TYPE);
            case str_hash("as"): return make_token(TOKEN_AS);
            case str_hash("sizeof"): return make_token(TOKEN_SIZEOF);
            case str_hash("region"): return make_token(TOKEN_REGION);
            case str_hash("enum"): return make_token(TOKEN_ENUM);
            case str_hash("true"): return make_token(TOKEN_TRUE);
            case str_hash("false"): return make_token(TOKEN_FALSE);
            case str_hash("nullptr"): return make_token(TOKEN_NULLPTR);
        }
        return make_token(TOKEN_IDENT);
    }
    Token Lexer::string() {
        while (peek() != '"' && !is_at_end()) {
            if (peek() == '\\') {
                advance();
                if (!is_at_end()) advance();
                continue;
            }
            if (peek() == '\n') {
                ctx.srcloc.line++;
                ctx.srcloc.column = 1;
            }
            advance();
        }
        if (is_at_end()) {
            return make_token(TOKEN_ILLEGAL);
        }
        advance();
        return Token{
            TOKEN_STRING,
            std::string_view(buffer.data() + start + 1, pos - start - 2),
            {},
            0,
            {ctx.srcloc.file, token_line, token_column, static_cast<int>(pos - start)}
        };
    }
    Token Lexer::char_literal() {
        uint8_t value = 0;

        if (is_at_end()) {
            return make_token(TOKEN_ILLEGAL);
        }

        char c = advance();
        if (c == '\'') {
            return make_token(TOKEN_ILLEGAL); // empty
        }

        if (c == '\\') {
            if (is_at_end()) {
                return make_token(TOKEN_ILLEGAL);
            }
            char esc = advance();
            switch (esc) {
                case 'n':  value = '\n'; break;
                case 't':  value = '\t'; break;
                case 'r':  value = '\r'; break;
                case '\\': value = '\\'; break;
                case '\'': value = '\''; break;
                case '"':  value = '"';  break;
                case '0':  value = '\0'; break;
                default:
                    return make_token(TOKEN_ILLEGAL);
            }
        } else {
            value = static_cast<uint8_t>(c);
        }

        if (is_at_end() || peek() != '\'') {
            return make_token(TOKEN_ILLEGAL); // missing close or multi-char
        }
        advance(); // consume closing '

        Token tok = make_token(TOKEN_CHAR_LITERAL);
        tok.char_val = value;
        return tok;
    }
}
