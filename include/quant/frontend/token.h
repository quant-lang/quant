#pragma once
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

#include "utils/logger.h"

namespace quant{
    enum TokenType {
        // special
        TOKEN_EOF,
        TOKEN_ILLEGAL,

        // identifiers & literals
        TOKEN_IDENT,
        TOKEN_NUMBER,
        TOKEN_STRING,
        TOKEN_CHAR_LITERAL,

        // keywords
        TOKEN_IF,
        TOKEN_ELSE,
        TOKEN_WHILE,
        TOKEN_FOR,
        TOKEN_SWITCH,
        TOKEN_CASE,
        TOKEN_DEFAULT,
        TOKEN_RETURN,
        TOKEN_BREAK,
        TOKEN_CONTINUE,
        TOKEN_FUNC,
        TOKEN_STRUCT,
        TOKEN_MUT,
        TOKEN_AT, // @
        TOKEN_DOT, // .
        TOKEN_HASH, // #
        TOKEN_NAMESPACE,
        TOKEN_LOAD,
        TOKEN_USING,
        TOKEN_MODULE,
        TOKEN_EXTERN,
        TOKEN_AS,
        TOKEN_SIZEOF,
        TOKEN_REGION,
        TOKEN_ENUM,
        TOKEN_TRUE,
        TOKEN_FALSE,
        TOKEN_NULLPTR,

        // types
        TOKEN_VOID,
        TOKEN_BOOL,

        TOKEN_I8,
        TOKEN_I16,
        TOKEN_I32,
        TOKEN_I64,

        TOKEN_U8,
        TOKEN_U16,
        TOKEN_U32,
        TOKEN_U64,

        TOKEN_F32,
        TOKEN_F64,

        TOKEN_STR_TYPE,
        TOKEN_CHAR_TYPE,

        // operators
        TOKEN_PLUS,      // +
        TOKEN_MINUS,     // -
        TOKEN_STAR,      // *
        TOKEN_SLASH,     // /
        TOKEN_NOT,        // !
        TOKEN_AMP,         // &
        TOKEN_PIPE,        // |
        TOKEN_AMP_AMP,     // &&
        TOKEN_PIPE_PIPE,   // ||
        TOKEN_COLON,      // :

        TOKEN_PLUS_EQ,      // +=
        TOKEN_MINUS_EQ,     // -=
        TOKEN_STAR_EQ,      // *=
        TOKEN_SLASH_EQ,     // /=
        TOKEN_AMP_EQ,       // &=
        TOKEN_PIPE_EQ,      // |=
        TOKEN_PLUS_PLUS,    // ++
        TOKEN_MINUS_MINUS, // --

        TOKEN_EQ,        // =
        TOKEN_EQEQ,      // ==
        TOKEN_NEQ,       // !=
        TOKEN_EQA,       // =>

        TOKEN_LT,        // <
        TOKEN_LTE,       // <=
        TOKEN_GT,        // >
        TOKEN_GTE,       // >=
        TOKEN_COLON_COLON, // ::

        // delimiters
        TOKEN_LPAREN,    // (
        TOKEN_RPAREN,    // )
        TOKEN_LBRACE,    // {
        TOKEN_RBRACE,    // }
        TOKEN_LBRACKET,  // [
        TOKEN_RBRACKET,  // ]
        TOKEN_COMMA,     // ,
        TOKEN_SEMICOLON // ;
    };

    struct Token {
        TokenType type;
        std::string_view text;

        union {
            double number;    // TOKEN_NUMBER
            uint8_t char_val; // TOKEN_CHAR_LITERAL
        };
        uint64_t inum = 0;    // TOKEN_NUMBER (integer literal, raw u64)

        SourceLocation loc;

        [[nodiscard]] bool is_type() const noexcept{
             return type >= TOKEN_VOID && type <= TOKEN_CHAR_TYPE;
        }
    };
    std::string token_type_to_string(TokenType type);
}
