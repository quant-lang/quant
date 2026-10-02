#pragma once

#include <iostream>
#include <vector>
#include <queue>

#include "token.h"
#include "lexer.h"
#include "ast.h"

namespace quant::ps {

    enum class DeclKind {
        None,
        Var,
        Func
    };

    class Parser {
        public:
            Parser(lx::Lexer& lex, CompilerContext& ctx);

            std::vector<ast::Stmt*> parse();

        private:
            lx::Lexer& lexer;
            CompilerContext& ctx;

            Token current;
            Token previous;

            std::deque<Token> buffer;

            const std::vector<std::string>* current_type_params = nullptr;

            void sync();

            ErrorBag::ParserException error(const SourceLocation& loc, int length, const std::string& msg);

            // core token control
            Token advance();
            bool check(TokenType type);
            bool match(TokenType type);
            Token expect(TokenType type, const char* msg);
            Token peek(int offset);

            // statements
            ast::Stmt parse_statement();
            ast::VarDecl parse_var_decl();
            ast::StructDecl parse_struct_decl();
            ast::Block* parse_block();
            ast::Block* parse_statement_body();
            ast::NamespaceStmt parse_namespace_stmt();
            ast::ModuleDecl parse_module_decl();
            ast::LoadStmt parse_load();
            ast::UsingStmt parse_using();
            ast::IfStmt parse_if();
            ast::ElseIfStmt* parse_else_if();
            ast::SwitchStmt parse_switch();
            std::vector<ast::Attribute> parse_attributes();
            ast::WhileStmt parse_while();
            ast::Stmt parse_for();
            ast::ReturnStmt parse_return();
            ast::FuncStmt parse_func(bool is_extern, const char* struct_name = nullptr,
                                     const std::vector<std::string>* outer_type_params = nullptr);
            ast::RegionStmt parse_region();
            ast::EnumDecl parse_enum_decl();
            std::vector<ast::FuncArg> parse_func_args(const std::vector<std::string>* type_params = nullptr);

            // expressions(Pratt)
            ast::Expr* parse_expr(int precedence = 0);
            ast::Expr* parse_prefix();
            ast::Expr* parse_postfix(ast::Expr* left);

            // helpers
            ast::Expr* make_binary(ast::Expr* left, ast::Expr* right, TokenType op);
            ast::Expr* make_cast(ast::Expr* value, const ast::Type* target, ast::CastKind kind);
            const ast::Type* parse_type(bool allow_implicit_void = false, const std::vector<std::string>* type_params = nullptr);
            DeclKind declaration_kind();
            bool looks_like_generic_args();
        };

    enum class Precedence {
        Lowest = 0,
        Assignment = 1,
        NullCoalesce = 2,
        Equality = 3,
        Additive = 4,
        Multiplicative = 5,
        Prefix = 6,
        Call = 7
    };
}
