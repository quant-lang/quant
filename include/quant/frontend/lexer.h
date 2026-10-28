#pragma once

#include <iostream>
#include <string>
#include <fstream>
#include <vector>

#include "token.h"
#include "quant/support/compiler_context.h"

#include "utils/file_manager.h"
#include "utils/logger.h"

namespace quant::lx{

    class Lexer {
        private:
            std::string buffer;
            size_t pos;
            int start;

            CompilerContext& ctx;
            int token_line;
            int token_column;

            char peek();
            char advance();
            bool match(char expected);
            bool is_at_end();

            Token make_token(TokenType);
            Token make_int(uint64_t v);
            Token make_float(double v);

            Token radix_number(int base);   // 0x, 0b, 0o
            TokenType consume_int_suffix(); // i8, ..., u64
            Token number();
            Token identifier();
            Token string();
            Token char_literal();

        public:
            Token next_token();

        Lexer(std::string source, CompilerContext& _ctx)
    : buffer(std::move(source)),
      pos(0),
      start(0),
      ctx(_ctx)
{
    ctx.srcloc.line = 1;
    ctx.srcloc.column = 1;
}
    };
}
