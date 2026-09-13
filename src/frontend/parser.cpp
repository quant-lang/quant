#include "quant/frontend/parser.h"
#include "quant/frontend/ast.h"
#include "quant/frontend/token.h"
#include "quant/support/alloc.h"
#include "quant/support/symbol_path.h"
#include "utils/logger.h"

#include <utility>

using namespace utils::logger;

namespace quant::ps {

namespace {

bool is_type_token(TokenType t) {
    switch (t) {
        case TOKEN_VOID: case TOKEN_BOOL:
        case TOKEN_I8: case TOKEN_I16: case TOKEN_I32: case TOKEN_I64:
        case TOKEN_U8: case TOKEN_U16: case TOKEN_U32: case TOKEN_U64:
        case TOKEN_F32: case TOKEN_F64:
        case TOKEN_STR_TYPE:
        case TOKEN_CHAR_TYPE:
        case TOKEN_STAR:
        case TOKEN_AMP:
            return true;
        default:
            return false;
    }
}

std::string process_escapes(const std::string& raw, SourceLocation loc) {
    std::string result;
    result.reserve(raw.size());

    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\') {
            if (i + 1 >= raw.size()) {
                error(loc, "Invalid escape sequence at end of string");
                result += '\\';
                continue;
            }
            char c = raw[++i]; // consume the escape char
            switch (c) {
                case 'n':  result += '\n'; break;
                case 't':  result += '\t'; break;
                case 'r':  result += '\r'; break;
                case '\\': result += '\\'; break;
                case '"':  result += '"';  break;
                case '\'': result += '\''; break;
                case '0':  result += '\0'; break;
                default:
                    error(loc, "Invalid escape sequence: \\" + std::string(1, c));
                    result += c;
                    break;
            }
        } else {
            result += raw[i];
        }
    }

    return result;
}

ast::BinaryOp get_op_from_token(TokenType type) {
    switch (type) {
        case TOKEN_PLUS:  return ast::BinaryOp::Add;
        case TOKEN_MINUS: return ast::BinaryOp::Sub;
        case TOKEN_STAR:  return ast::BinaryOp::Mul;
        case TOKEN_SLASH: return ast::BinaryOp::Div;
        case TOKEN_EQEQ:   return ast::BinaryOp::Eq;
        case TOKEN_NEQ:    return ast::BinaryOp::Neq;
        case TOKEN_LT:     return ast::BinaryOp::Lt;
        case TOKEN_LTE:    return ast::BinaryOp::Lte;
        case TOKEN_GT:     return ast::BinaryOp::Gt;
        case TOKEN_GTE:    return ast::BinaryOp::Gte;
        case TOKEN_AMP:    return ast::BinaryOp::BitAnd;
        case TOKEN_PIPE:   return ast::BinaryOp::BitOr;
        case TOKEN_AMP_AMP: return ast::BinaryOp::LogicAnd;
        case TOKEN_PIPE_PIPE: return ast::BinaryOp::LogicOr;
        default:           return ast::BinaryOp::Add;
    }
}

int get_precedence(TokenType t) {
    switch (t) {
        case TOKEN_EQ:
        case TOKEN_PLUS_EQ:
        case TOKEN_MINUS_EQ:
        case TOKEN_STAR_EQ:
        case TOKEN_SLASH_EQ:
        case TOKEN_AMP_EQ:
        case TOKEN_PIPE_EQ:
            return 1;

        case TOKEN_PIPE_PIPE:
            return 2;

        case TOKEN_AMP_AMP:
            return 3;

        case TOKEN_PIPE:
            return 4;

        case TOKEN_AMP:
            return 5;

        case TOKEN_EQEQ:
        case TOKEN_NEQ:
            return 6;

        case TOKEN_LT:
        case TOKEN_LTE:
        case TOKEN_GT:
        case TOKEN_GTE:
            return 7;

        case TOKEN_PLUS:
        case TOKEN_MINUS:
            return 8;

        case TOKEN_STAR:
        case TOKEN_SLASH:
            return 9;

        default:
            return -1;
    }
}

template <class T>
ast::Expr* make_expr(CompilerContext& ctx, T&& kind, SourceLocation loc = {}) {
    auto* e = memory::make_default<ast::Expr>(ctx.ast_arena);
    e->kind = std::forward<T>(kind);
    e->loc = loc;
    return e;
}

// Rewrite a type so that bare struct types matching a generic parameter name
// become generic parameters. Needed because the return type of a generic
// function is parsed before the `<T, ...>` parameter list is seen.
const ast::Type* bind_type_params(CompilerContext& ctx, const ast::Type* t, const std::vector<std::string>& params) {
    if (!t) return nullptr;

    if (t->kind == ast::TypeKind::Struct) {
        if (t->type_args.empty()) {
            for (const auto& p : params) {
                if (t->struct_name == p) {
                    return ctx.types.get_generic_param(p);
                }
            }
            return t;
        }

        std::vector<const ast::Type*> new_args;
        bool changed = false;
        for (const auto* arg : t->type_args) {
            const ast::Type* new_arg = bind_type_params(ctx, arg, params);
            new_args.push_back(new_arg);
            if (new_arg != arg) changed = true;
        }
        if (changed) {
            std::string base = t->struct_name;
            std::string unmangled;
            if (ctx.types.is_mangled_name(base, unmangled)) {
                base = unmangled;
            }
            return ctx.types.get_deferred_generic(base, new_args);
        }
        return t;
    }

    if (t->kind == ast::TypeKind::Pointer || t->kind == ast::TypeKind::Reference) {
        const ast::Type* new_pointed = bind_type_params(ctx, t->pointed, params);
        if (new_pointed != t->pointed) {
            return t->kind == ast::TypeKind::Pointer
                ? ctx.types.get_pointer(new_pointed)
                : ctx.types.get_reference(new_pointed);
        }
        return t;
    }

    return t;
}

} // namespace

Parser::Parser(lx::Lexer& lex, CompilerContext& ctx_)
    : lexer(lex), ctx(ctx_) {
    current = lexer.next_token();
}

std::vector<ast::Stmt*> Parser::parse() {
    std::vector<ast::Stmt*> out;

    while (!check(TOKEN_EOF)) {
        out.push_back(memory::make<ast::Stmt>(ctx.ast_arena, parse_statement()));
    }

    return out;
}

Token Parser::advance() {
    previous = current;

    if (!buffer.empty()) {
        current = buffer.front();
        buffer.pop_front();
    } else {
        current = lexer.next_token();
    }

    return previous;
}

bool Parser::check(TokenType type) {
    return current.type == type;
}

bool Parser::match(TokenType type) {
    if (!check(type)) return false;
    advance();
    return true;
}

void Parser::sync() {
    int nesting = 0;
    while (!check(TOKEN_EOF)) {
        if (check(TOKEN_SEMICOLON) && nesting == 0) return;
        if (check(TOKEN_RBRACE)) {
            if (nesting == 0) return;
            nesting--;
        }
        if (check(TOKEN_LBRACE)) nesting++;
        switch (current.type) {
            case TOKEN_STRUCT: case TOKEN_IF:
            case TOKEN_WHILE: case TOKEN_FOR: case TOKEN_SWITCH: case TOKEN_RETURN: case TOKEN_NAMESPACE:
            case TOKEN_BREAK: case TOKEN_CONTINUE:
            case TOKEN_MODULE: case TOKEN_LOAD: case TOKEN_USING: case TOKEN_AT:
            case TOKEN_EXTERN: case TOKEN_REGION: case TOKEN_ENUM:
                if (nesting == 0) return;
            default: break;
        }
        advance();
    }
}

Token Parser::expect(TokenType type, const char* msg) {
    if (!check(type)) {
        ctx.errors.add(current.loc, current.text.length(), msg);

        if (type == TOKEN_LBRACE || type == TOKEN_RBRACE ||
            type == TOKEN_SEMICOLON || type == TOKEN_RPAREN) {
            sync();
            // Don't advance past a closing brace we synced to
            if ((type == TOKEN_SEMICOLON || type == TOKEN_RPAREN) &&
                check(TOKEN_RBRACE)) {
                return current;
            }
        }

        Token bad = current;
        if (!check(TOKEN_EOF)) {
            advance();
        }
        return bad;
    }

    return advance();
}

Token Parser::peek(int n) {
    while (buffer.size() <= (size_t)n) {
        buffer.push_back(lexer.next_token());
    }
    return buffer[n];
}

bool Parser::looks_like_generic_args() {
    // `current` is '<'. Scan forward to decide whether the tokens inside the
    // angle brackets form a comma-separated type argument list (which may
    // contain nested generics, e.g. Vec<Token>) closed by a matching '>' that
    // is immediately followed by '(' or '{'.
    // Depth starts at 1 because the opening '<' is `current`.
    int depth = 1;
    for (int i = 0; ; ++i) {
        Token t = peek(i);
        switch (t.type) {
            case TOKEN_IDENT:
            case TOKEN_COLON_COLON:
            case TOKEN_VOID: case TOKEN_BOOL:
            case TOKEN_I8: case TOKEN_I16: case TOKEN_I32: case TOKEN_I64:
            case TOKEN_U8: case TOKEN_U16: case TOKEN_U32: case TOKEN_U64:
            case TOKEN_F32: case TOKEN_F64:
            case TOKEN_STR_TYPE: case TOKEN_CHAR_TYPE:
            case TOKEN_STAR: case TOKEN_AMP:
                break;
            case TOKEN_LT:
                ++depth;
                break;
            case TOKEN_GT:
                --depth;
                if (depth == 0) {
                    Token after = peek(i + 1);
                    return after.type == TOKEN_LPAREN || after.type == TOKEN_LBRACE;
                }
                break;
            case TOKEN_COMMA:
                break;
            default:
                return false;
        }
    }
}

// Statements

ast::Stmt Parser::parse_statement() {
    std::vector<ast::Attribute> attrs;
    if (check(TOKEN_AT)) {
        attrs = parse_attributes();
    }

    if (match(TOKEN_RETURN)) {
        return ast::Stmt{ ast::ReturnStmt{ parse_return() } };
    }

    if (match(TOKEN_BREAK)) {
        expect(TOKEN_SEMICOLON, "Expected ';' after break");
        return ast::Stmt{ ast::BreakStmt{} };
    }

    if (match(TOKEN_CONTINUE)) {
        expect(TOKEN_SEMICOLON, "Expected ';' after continue");
        return ast::Stmt{ ast::ContinueStmt{} };
    }

    if (match(TOKEN_IF)) {
        return ast::Stmt{ ast::IfStmt{ parse_if() } };
    }

    if (match(TOKEN_SWITCH)) {
        return ast::Stmt{ ast::SwitchStmt{ parse_switch() } };
    }

    if (match(TOKEN_WHILE)) {
        return ast::Stmt{ ast::WhileStmt{ parse_while() } };
    }

    if (match(TOKEN_FOR)) {
        return parse_for();
    }

    if (match(TOKEN_EXTERN)) {
        auto func = parse_func(true);
        func.attributes = std::move(attrs);
        return ast::Stmt{ std::move(func) };
    }

    if (match(TOKEN_STRUCT)) {
        auto decl = parse_struct_decl();
        decl.attributes = std::move(attrs);
        return ast::Stmt{ std::move(decl) };
    }

    if (match(TOKEN_NAMESPACE)) {
        return ast::Stmt{ ast::NamespaceStmt{ parse_namespace_stmt() } };
    }

    if (match(TOKEN_MODULE)) {
        auto decl = parse_module_decl();
        decl.attributes = std::move(attrs);
        return ast::Stmt{ std::move(decl) };
    }

    if (match(TOKEN_LOAD)) {
        return ast::Stmt{ ast::LoadStmt{ parse_load() } };
    }

    if (match(TOKEN_USING)) {
        return ast::Stmt{ ast::UsingStmt{ parse_using() } };
    }

    if (match(TOKEN_REGION)){
        return ast::Stmt{ast::RegionStmt{ parse_region() } };
    }

    if (match(TOKEN_ENUM)) {
        auto decl = parse_enum_decl();
        decl.attributes = std::move(attrs);
        return ast::Stmt{ std::move(decl) };
    }

    switch (declaration_kind()) {
        case DeclKind::Func: {
            auto func = parse_func(false);
            func.attributes = std::move(attrs);
            return ast::Stmt{ std::move(func) };
        }
        case DeclKind::Var: {
            auto var = parse_var_decl();
            var.attributes = std::move(attrs);
            return ast::Stmt{ std::move(var) };
        }
        case DeclKind::None:
            break;
    }

    ast::Expr* expr = parse_expr(0);
    expect(TOKEN_SEMICOLON, "Expected ';' after expression");
    return ast::Stmt{ ast::ExprStmt{ expr } };
}

DeclKind Parser::declaration_kind() {
    // Scan a leading `mut`, pointer/reference prefixes, and a type name
    // (builtin keyword or qualified identifier, optionally generic), followed
    // by an identifier. If that identifier is followed by '(' it's a function
    // declaration (with optional `<T, ...>` type parameters), otherwise a
    // variable declaration.
    int i = 0;
    // `current` holds the first token; peek(0) is the token after it.
    auto at = [this](int n) { return n == 0 ? current : peek(n - 1); };

    if (at(i).type == TOKEN_MUT) ++i;

    while (at(i).type == TOKEN_STAR || at(i).type == TOKEN_AMP) ++i;

    Token t = at(i);
    if (t.is_type()) {
        ++i;
    } else if (t.type == TOKEN_IDENT) {
        ++i;
        while (at(i).type == TOKEN_COLON_COLON) {
            if (at(i + 1).type != TOKEN_IDENT) return DeclKind::None;
            i += 2;
        }
        if (at(i).type == TOKEN_LT) {
            int depth = 1;
            ++i;
            while (depth > 0) {
                Token p = at(i);
                if (p.type == TOKEN_LT) ++depth;
                else if (p.type == TOKEN_GT) --depth;
                else if (p.type == TOKEN_EOF || p.type == TOKEN_SEMICOLON) return DeclKind::None;
                ++i;
            }
        }
    } else {
        return DeclKind::None;
    }

    if (at(i).type != TOKEN_IDENT) return DeclKind::None;
    ++i;

    if (at(i).type == TOKEN_LT) {
        int depth = 1;
        ++i;
        while (depth > 0) {
            Token p = at(i);
            if (p.type == TOKEN_LT) ++depth;
            else if (p.type == TOKEN_GT) --depth;
            else if (p.type == TOKEN_EOF || p.type == TOKEN_SEMICOLON) return DeclKind::None;
            ++i;
        }
    }

    return at(i).type == TOKEN_LPAREN ? DeclKind::Func : DeclKind::Var;
}

ast::VarDecl Parser::parse_var_decl() {
    ast::VarDecl ret;
    ret.is_mut = match(TOKEN_MUT);

    ret.type = parse_type(false, current_type_params);

    Token name = expect(TOKEN_IDENT, "Expected variable name");
    ret.name = name.text;

    ret.value = nullptr;

    if (match(TOKEN_EQ)) {
        ret.value = parse_expr(0);
    }

    expect(TOKEN_SEMICOLON, "Expected ';' after declaration");

    return ret;
}

ast::StructDecl Parser::parse_struct_decl() {
    ast::StructDecl ret;

    ret.name = std::string(expect(TOKEN_IDENT, "Expected struct name").text);

    if(match(TOKEN_LT)){
        do{
            Token param = expect(TOKEN_IDENT, "Expected type parameter name");
            ret.type_params.push_back(std::string(param.text));
        } while(match(TOKEN_COMMA));
        expect(TOKEN_GT, "Expected '>' after type parameters");
    }
    expect(TOKEN_LBRACE, "Expected '{' after struct name");

    // Methods declared inside the struct are bound to it through `struct_name`.
    // Keep the name in the arena so the pointer stays valid for the whole
    // compilation (the StructDecl itself may be moved while being lowered).
    auto* struct_name = memory::make<std::string>(ctx.ast_arena, ret.name);

    auto is_priv = [](const std::vector<ast::Attribute>& attrs) {
        for (const auto& a : attrs) {
            if (a.name == "private") return true;
        }
        return false;
    };

    while (!check(TOKEN_RBRACE) && !check(TOKEN_EOF)) {
        std::vector<ast::Attribute> member_attrs = parse_attributes();

        switch (declaration_kind()) {
            case DeclKind::Var: {
                ast::StructField field;

                field.attributes = std::move(member_attrs);
                field.is_mut = match(TOKEN_MUT);
                field.is_private = is_priv(field.attributes);

                field.type = parse_type(false, &ret.type_params);

                Token field_name = expect(TOKEN_IDENT, "Expected field name");
                field.name = field_name.text;

                field.default_value = nullptr;
                if (match(TOKEN_EQ)) {
                    field.default_value = parse_expr(0);
                }

                expect(TOKEN_SEMICOLON, "Expected ';' after field");

                ret.fields.push_back(std::move(field));
                break;
            }
            case DeclKind::Func: {
                ast::FuncStmt fn = parse_func(false, struct_name->c_str(), &ret.type_params);
                fn.attributes = std::move(member_attrs);
                fn.is_private = is_priv(fn.attributes);
                ret.fields.push_back(std::move(fn));
                break;
            }
            case quant::ps::DeclKind::None:
                ctx.errors.add(current.loc, current.text.length(), "Unexpected Declaration");
                advance();
                break;
        }
    }

    expect(TOKEN_RBRACE, "Expected '}' after struct body");
    expect(TOKEN_SEMICOLON, "Expected ';' after struct body");

    return ret;
}

std::vector<ast::Attribute> Parser::parse_attributes() {
    std::vector<ast::Attribute> attrs;
    while (check(TOKEN_AT)) {
        advance();
        ast::Attribute attr;
        attr.name = expect(TOKEN_IDENT, "Expected attribute name").text;
        if (match(TOKEN_LPAREN)) {
            while (!check(TOKEN_RPAREN) && !check(TOKEN_EOF)){
                attr.args.push_back(parse_expr(0));
                if (!check(TOKEN_RPAREN)) {
                    expect(TOKEN_COMMA, "Expected ',' after attribute argument");
                }
            }
            expect(TOKEN_RPAREN, "Expected ')' after attribute arguments");
        }
        attrs.push_back(std::move(attr));
    }
    return attrs;
}

ast::IfStmt Parser::parse_if() {
    ast::IfStmt ret;

    expect(TOKEN_LPAREN, "Expected '('");
    ret.condition = parse_expr(0);
    expect(TOKEN_RPAREN, "Expected ')'");

    ret.then_block = parse_statement_body();

    ret.else_if = nullptr;
    ret.else_block = nullptr;

    if (match(TOKEN_ELSE)) {
        if (check(TOKEN_IF)) {
            advance();
            ret.else_if = parse_else_if();
        } else {
            ret.else_block = parse_statement_body();
        }
    }

    return ret;
}

ast::ElseIfStmt* Parser::parse_else_if() {
    auto* ret = memory::make_default<ast::ElseIfStmt>(ctx.ast_arena);

    expect(TOKEN_LPAREN, "Expected '('");
    ret->condition = parse_expr(0);
    expect(TOKEN_RPAREN, "Expected ')'");

    ret->then_block = parse_statement_body();

    ret->else_block = nullptr;
    ret->next = nullptr;

    if (match(TOKEN_ELSE)) {
        if (check(TOKEN_IF)) {
            advance();
            ret->next = parse_else_if();
        } else {
            ret->else_block = parse_statement_body();
        }
    }

    return ret;
}

ast::WhileStmt Parser::parse_while() {
    ast::WhileStmt ret;

    expect(TOKEN_LPAREN, "Expected '('");
    ret.condition = parse_expr(0);
    expect(TOKEN_RPAREN, "Expected ')'");

    ret.body = parse_statement_body();

    return ret;
}

ast::Stmt Parser::parse_for() {
    // `for (init; cond; step) body` is desugared into a WhileStmt that keeps
    // the step in `for_step`: the loop re-checks `cond` after `body` + `step`,
    // and `continue` jumps to `step` (C semantics), not to the condition.
    const SourceLocation loc = previous.loc;

    expect(TOKEN_LPAREN, "Expected '(' after for");

    ast::Stmt* init_stmt = nullptr;
    if (!check(TOKEN_SEMICOLON)) {
        if (declaration_kind() == DeclKind::Var) {
            init_stmt = memory::make<ast::Stmt>(ctx.ast_arena, parse_var_decl());
        } else {
            ast::Expr* expr = parse_expr(0);
            init_stmt = memory::make<ast::Stmt>(ctx.ast_arena, ast::ExprStmt{ expr });
            expect(TOKEN_SEMICOLON, "Expected ';' after for initializer");
        }
    } else {
        expect(TOKEN_SEMICOLON, "Expected ';' after for initializer");
    }

    ast::Expr* cond = nullptr;
    if (!check(TOKEN_SEMICOLON)) {
        cond = parse_expr(0);
    }
    expect(TOKEN_SEMICOLON, "Expected ';' after for condition");

    ast::Expr* step = nullptr;
    if (!check(TOKEN_RPAREN)) {
        step = parse_expr(0);
    }
    expect(TOKEN_RPAREN, "Expected ')' after for clauses");

    ast::Block* body = parse_statement_body();

    auto* block = memory::make_default<ast::Block>(ctx.ast_arena);

    if (init_stmt) {
        block->stmts.push_back(init_stmt);
    }

    ast::WhileStmt while_node;
    while_node.condition = cond ? cond : make_expr(ctx, ast::BoolExpr{ true }, loc);

    auto* while_body = memory::make_default<ast::Block>(ctx.ast_arena);
    if (body) {
        for (auto* stmt : body->stmts) {
            while_body->stmts.push_back(stmt);
        }
    }
    while_node.body = while_body;
    while_node.for_step = step;

    block->stmts.push_back(
        memory::make<ast::Stmt>(ctx.ast_arena, ast::WhileStmt{ while_node })
    );

    ast::Stmt ret;
    ret.kind = ast::BlockStmt{ block };
    ret.loc = loc;
    return ret;
}

ast::SwitchStmt Parser::parse_switch() {
    ast::SwitchStmt ret;

    expect(TOKEN_LPAREN, "Expected '(' after switch");
    ret.condition = parse_expr(0);
    expect(TOKEN_RPAREN, "Expected ')'");

    expect(TOKEN_LBRACE, "Expected '{' after switch condition");

    ret.default_block = nullptr;

    while (!check(TOKEN_RBRACE) && !check(TOKEN_EOF)) {
if (match(TOKEN_CASE)) {
            ast::CaseStmt cs;
            while (true) {
                cs.values.push_back(parse_expr(0));
                expect(TOKEN_COLON, "Expected ':' after case value");
                if (!check(TOKEN_CASE)) break;
                advance(); // consume the next 'case' keyword (shared body)
            }
            cs.body = parse_statement_body();
            ret.cases.push_back(std::move(cs));
        } else if (match(TOKEN_DEFAULT)) {
            if (ret.default_block) {
                ctx.errors.add(previous.loc, previous.text.length(), "Multiple 'default' blocks in switch");
            }
            expect(TOKEN_COLON, "Expected ':' after default");
            ret.default_block = parse_statement_body();
        } else {
            ctx.errors.add(current.loc, current.text.length(), "Expected 'case' or 'default' in switch body");
            while (!check(TOKEN_EOF) && !check(TOKEN_CASE) &&
                   !check(TOKEN_DEFAULT) && !check(TOKEN_RBRACE)) {
                advance();
            }
        }
    }

    expect(TOKEN_RBRACE, "Expected '}' after switch body");

    return ret;
}

ast::ReturnStmt Parser::parse_return() {
    ast::ReturnStmt ret;
    ret.value = nullptr;

    if (!check(TOKEN_SEMICOLON)) {
        ret.value = parse_expr(0);
    }

    expect(TOKEN_SEMICOLON, "Expected ';'");

    return ret;
}

ast::FuncStmt Parser::parse_func(bool is_extern, const char* struct_name,
                                 const std::vector<std::string>* outer_type_params) {
    ast::FuncStmt ret;

    // Method return types may reference the enclosing generic struct's type
    // params (e.g. `T get()` inside `struct Box<T>`); combine those with the
    // function's own type params so they bind to Generic types.
    std::vector<std::string> combined_params;
    const std::vector<std::string>* eff_params = nullptr;
    if (outer_type_params) {
        combined_params = *outer_type_params;
        eff_params = &combined_params;
    }

    ret.return_type = parse_type(false, eff_params ? eff_params : current_type_params);

    Token name = expect(TOKEN_IDENT, "Expected function name");
    ret.name = name.text;

    if(match(TOKEN_LT)){
        do{
            Token param = expect(TOKEN_IDENT, "Expected type parameter name");
            ret.type_params.push_back(std::string(param.text));
        } while(match(TOKEN_COMMA));
        expect(TOKEN_GT, "Expected '>' after type parameters");
    }

    if (!ret.type_params.empty()) {
        ret.return_type = bind_type_params(ctx, ret.return_type, ret.type_params);
        combined_params.insert(combined_params.end(), ret.type_params.begin(), ret.type_params.end());
        eff_params = &combined_params;
    }

    if (!eff_params) {
        eff_params = current_type_params;
    }

    ret.is_extern = is_extern;
    ret.has_body = false;
    ret.body = nullptr;
    ret.struct_name = std::move(struct_name);

    const auto* saved_type_params = current_type_params;
    current_type_params = eff_params;

    expect(TOKEN_LPAREN, "Expected '('");
    ret.args = parse_func_args(current_type_params);
    expect(TOKEN_RPAREN, "Expected ')'");

    if (check(TOKEN_LBRACE)) {
        ret.body = parse_block();
        ret.has_body = true;
    } else {
        expect(TOKEN_SEMICOLON, "Expected ';' after function declaration");
    }

    current_type_params = saved_type_params;

    return ret;
}

ast::RegionStmt Parser::parse_region(){
    ast::RegionStmt ret;

    ret.name = expect(TOKEN_IDENT, "Expected region name").text;
    ret.body = parse_block();

    return ret;
}
ast::EnumDecl Parser::parse_enum_decl() {
    ast::EnumDecl ret;

    ret.name = std::string(expect(TOKEN_IDENT, "Expected enum name").text);

    expect(TOKEN_LBRACE, "Expected '{' after enum name");

    while (!check(TOKEN_RBRACE) && !check(TOKEN_EOF)) {
        Token name = expect(TOKEN_IDENT, "Expected variant name");
        ret.variants.push_back(std::string(name.text));

        if (!check(TOKEN_RBRACE)) {
            expect(TOKEN_COMMA, "Expected ',' after variant");
        }
    }

    expect(TOKEN_RBRACE, "Expected '}' after enum body");
    expect(TOKEN_SEMICOLON, "Expected ';' after enum body");

    return ret;
}
std::vector<ast::FuncArg> Parser::parse_func_args(const std::vector<std::string>* type_params) {
    std::vector<ast::FuncArg> args;

    if (check(TOKEN_RPAREN)) {
        return args;
    }

    while (true) {
        bool is_mut = match(TOKEN_MUT);

        const ast::Type* type = parse_type(true, type_params);

        Token name = expect(TOKEN_IDENT, "Expected argument name");

        args.push_back({
            std::string(name.text),
            type,
            is_mut
        });

        if (!match(TOKEN_COMMA)) {
            break;
        }
    }

    return args;
}

ast::NamespaceStmt Parser::parse_namespace_stmt() {
    ast::NamespaceStmt ret;
    ret.name = expect(TOKEN_IDENT, "Expected namespace name").text;
    ret.body = parse_block();
    return ret;
}

// Parse a statement; if it's a single statement not wrapped in braces
// (e.g. if (x) return 1;), the statement itself is the body
ast::Block* Parser::parse_statement_body() {
    auto* block = memory::make_default<ast::Block>(ctx.ast_arena);
    if (check(TOKEN_LBRACE)) {
        advance();
        while (!check(TOKEN_RBRACE) && !check(TOKEN_EOF)) {
            block->stmts.push_back(
                memory::make<ast::Stmt>(ctx.ast_arena, parse_statement())
            );
        }
        expect(TOKEN_RBRACE, "Expected '}'");
        return block;
    }

    block->stmts.push_back(
        memory::make<ast::Stmt>(ctx.ast_arena, parse_statement())
    );
    return block;
}

ast::Block* Parser::parse_block() {
    if (!check(TOKEN_LBRACE)) {
        ctx.errors.add(current.loc, current.text.length(), "Expected '{'");
        auto* block = memory::make_default<ast::Block>(ctx.ast_arena);
        return block;
    }
    advance();

    auto* block = memory::make_default<ast::Block>(ctx.ast_arena);

    while (!check(TOKEN_RBRACE) && !check(TOKEN_EOF)) {
        block->stmts.push_back(
            memory::make<ast::Stmt>(ctx.ast_arena, parse_statement())
        );
    }

    if (!check(TOKEN_RBRACE)) {
        ctx.errors.add(current.loc, current.text.length(), "Expected '}'");
        return block;
    }
    advance();

    return block;
}
ast::ModuleDecl Parser::parse_module_decl() {
    ast::ModuleDecl ret;
    std::string_view raw = expect(TokenType::TOKEN_STRING, "Expected module name").text;
    ret.name = raw;
    expect(TOKEN_SEMICOLON, "Expected ';' after module");
    return ret;
}

ast::LoadStmt Parser::parse_load() {
    ast::LoadStmt ret;

    std::string_view raw = expect(TokenType::TOKEN_STRING, "Expected module").text;

    ret.module = raw;

    expect(TOKEN_SEMICOLON, "Expected ';' after load");
    return ret;
}

ast::UsingStmt Parser::parse_using() {
    ast::UsingStmt ret;

    ast::Expr* expr = parse_expr(0);
    auto path = support::flatten_path(expr);
    ret.path = std::move(path);

    expect(TOKEN_SEMICOLON, "Expected ';' after using");
    return ret;
}
// Expressions

ast::Expr* Parser::parse_expr(int min_prec) {
    ast::Expr* left = parse_prefix();
    if (!left) return nullptr;

    left = parse_postfix(left);

    while (true) {
        int prec = get_precedence(current.type);
        if (prec < min_prec) break;

        Token op = advance();

        if (op.type == TOKEN_EQ) {
            if (!std::holds_alternative<ast::VarExpr>(left->kind) &&
                !std::holds_alternative<ast::FieldExpr>(left->kind) &&
                !std::holds_alternative<ast::IndexExpr>(left->kind)) {
                ctx.errors.add(op.loc, op.text.length(), "Invalid assignment target");
                (void)parse_expr(prec);
                return left;
            }

            ast::Expr* right = parse_expr(prec);
            return make_expr(ctx, ast::AssignExpr{ left, right }, left->loc);
        }

        if (op.type == TOKEN_PLUS_EQ || op.type == TOKEN_MINUS_EQ ||
            op.type == TOKEN_STAR_EQ || op.type == TOKEN_SLASH_EQ ||
            op.type == TOKEN_AMP_EQ || op.type == TOKEN_PIPE_EQ) {
            ast::Expr* rhs = parse_expr(prec);
            TokenType base_op;
            switch (op.type) {
                case TOKEN_PLUS_EQ:  base_op = TOKEN_PLUS;  break;
                case TOKEN_MINUS_EQ: base_op = TOKEN_MINUS; break;
                case TOKEN_STAR_EQ:  base_op = TOKEN_STAR;  break;
                case TOKEN_SLASH_EQ: base_op = TOKEN_SLASH; break;
                case TOKEN_AMP_EQ:   base_op = TOKEN_AMP;   break;
                case TOKEN_PIPE_EQ:  base_op = TOKEN_PIPE;  break;
                default:             base_op = TOKEN_PLUS;  break;
            }
            auto* bin = make_binary(left, rhs, base_op);
            return make_expr(ctx, ast::AssignExpr{ left, bin }, left->loc);
        }

        ast::Expr* right = parse_expr(prec + 1);
        left = make_binary(left, right, op.type);
    }

    return left;
}

ast::Expr* Parser::parse_prefix() {
    if (match(TOKEN_NUMBER)) {
        std::string_view text = previous.text;
        bool is_float = text.find('.') != std::string_view::npos ||
                        text.find('e') != std::string_view::npos ||
                        text.find('E') != std::string_view::npos;
        if (is_float) {
            return make_expr(ctx, ast::FloatExpr{ previous.number }, previous.loc);
        }
        return make_expr(ctx, ast::IntExpr{ static_cast<int64_t>(previous.inum) }, previous.loc);
    }

    if (match(TOKEN_STRING)) {
        return make_expr(ctx, ast::StringExpr{ process_escapes(std::string(previous.text), previous.loc) }, previous.loc);
    }

    if (match(TOKEN_CHAR_LITERAL)) {
        return make_expr(ctx, ast::CharExpr{ previous.char_val }, previous.loc);
    }

    if (match(TOKEN_TRUE)) {
        return make_expr(ctx, ast::BoolExpr{ true }, previous.loc);
    }

    if (match(TOKEN_FALSE)) {
        return make_expr(ctx, ast::BoolExpr{ false }, previous.loc);
    }
    if (match(TOKEN_NULLPTR)) {
        return make_expr(ctx, ast::NullPtrExpr{}, previous.loc);
    }
    if (match(TOKEN_HASH)) {
        ast::Expr* e = parse_expr(10);
        if (!e) return nullptr;
        e->is_comptime = true;
        return e;
    }
    if (match(TOKEN_SIZEOF)) {
        expect(TOKEN_LPAREN, "Expected '(' after sizeof");
        const ast::Type* type = parse_type(false, current_type_params);
        expect(TOKEN_RPAREN, "Expected ')' after sizeof type");
        return make_expr(ctx, ast::SizeofExpr{ type }, previous.loc);
    }
    if (match(TOKEN_NOT)){
        auto* operand = parse_expr(10);
        return make_expr(ctx, UnaryExpr{operand, ast::UnaryOp::Not}, previous.loc);
    }
    if (match(TOKEN_MINUS)){
        auto* operand = parse_expr(10);
        return make_expr(ctx, UnaryExpr{operand, ast::UnaryOp::Neg}, previous.loc);
    }
    if (match(TOKEN_AMP)){
        auto* operand = parse_expr(10);
        return make_expr(ctx, UnaryExpr{operand, ast::UnaryOp::AddrOf}, previous.loc);
    }

    if (match(TOKEN_IDENT)) {
        ast::Expr* expr =
            make_expr(ctx,
                ast::VarExpr{
                    std::string(previous.text)
                },
                previous.loc
            );

        while (match(TOKEN_COLON_COLON)) {
            Token name = expect(TOKEN_IDENT, "Expected identifier after ::");

            ast::Expr* rhs = make_expr(ctx, ast::VarExpr{
                        std::string(name.text)
                    },
                    name.loc
                );

            expr = make_expr(ctx,
                ast::NamespaceExpr{
                    expr,
                    rhs
                },
                expr->loc
            );
        }

        return expr;
    }

    if (match(TOKEN_LPAREN)) {
        ast::Expr* e = parse_expr(0);
        expect(TOKEN_RPAREN, "Expected ')'");
        return e;
    }

    if (is_type_token(current.type)) {
        const ast::Type* type = parse_type(false);
        if (type) {
            return make_expr(ctx, ast::TypeExpr{ type }, previous.loc);
        }
    }

    if (match(TOKEN_LBRACE)) {
        std::vector<ast::Expr*> init_args;
        if (!check(TOKEN_RBRACE)) {
            do {
                init_args.push_back(parse_expr(0));
            } while (match(TOKEN_COMMA));
        }
        expect(TOKEN_RBRACE, "Expected '}' after struct init");
        return make_expr(ctx, ast::StructInitExpr{ nullptr, {}, init_args }, previous.loc);
    }

    ctx.errors.add(current.loc, current.text.length(), "Unexpected token");
    if (!check(TOKEN_EOF)) {
        advance();
    }
    return nullptr;
}

ast::Expr* Parser::parse_postfix(ast::Expr* left) {
    while (true) {
        if (check(TOKEN_LT)) {
            auto n1 = peek(0);
            if (n1.type == TOKEN_IDENT || is_type_token(n1.type)) {
                if (looks_like_generic_args()) {
                    advance(); // consume <
                    std::vector<const ast::Type*> type_args;
                    do {
                        type_args.push_back(parse_type(false, current_type_params));
                    } while (match(TOKEN_COMMA));
                    expect(TOKEN_GT, "Expected '>' after generic type arguments");

                    if (check(TOKEN_LBRACE)) {
                        advance(); // consume {
                        std::vector<ast::Expr*> init_args;
                        if (!check(TOKEN_RBRACE)) {
                            do {
                                init_args.push_back(parse_expr(0));
                            } while (match(TOKEN_COMMA));
                        }
                        expect(TOKEN_RBRACE, "Expected '}' after struct init");
                        left = make_expr(ctx, ast::StructInitExpr{ left, type_args, init_args }, left->loc);
                        continue;
                    }

                    expect(TOKEN_LPAREN, "Expected '(' after generic function name");
                    std::vector<ast::Expr*> args;
                    if (!check(TOKEN_RPAREN)) {
                        do {
                            args.push_back(parse_expr(0));
                        } while (match(TOKEN_COMMA));
                    }
                    expect(TOKEN_RPAREN, "Expected ')'");
                    left = make_expr(ctx, ast::CallExpr{ left, args, type_args }, left->loc);
                    continue;
                }
            }
        }

        if (match(TOKEN_LPAREN)) {
            std::vector<ast::Expr*> args;

            if (!check(TOKEN_RPAREN)) {
                do {
                    args.push_back(parse_expr(0));
                } while (match(TOKEN_COMMA));
            }

            expect(TOKEN_RPAREN, "Expected ')'");

            left = make_expr(ctx, ast::CallExpr{ left, args }, left ? left->loc : SourceLocation{});
            continue;
        }

        if (match(TOKEN_LBRACE)) {
            std::vector<ast::Expr*> init_args;
            if (!check(TOKEN_RBRACE)) {
                do {
                    init_args.push_back(parse_expr(0));
                } while (match(TOKEN_COMMA));
            }
            expect(TOKEN_RBRACE, "Expected '}' after struct init");
            left = make_expr(ctx, ast::StructInitExpr{ left, {}, init_args }, left ? left->loc : SourceLocation{});
            continue;
        }

        if (match(TOKEN_LBRACKET)) {
            ast::Expr* index = parse_expr(0);
            expect(TOKEN_RBRACKET, "Expected ']'");

            left = make_expr(ctx, ast::IndexExpr{ left, index }, left ? left->loc : SourceLocation{});
            continue;
        }

        if (match(TOKEN_DOT)) {
            Token field = expect(TOKEN_IDENT, "Expected field name after '.'");

            left = make_expr(ctx, ast::FieldExpr{
                left,
                std::string(field.text)
            }, left ? left->loc : field.loc);

            continue;
        }
        if (check(TOKEN_PLUS_PLUS) || check(TOKEN_MINUS_MINUS)) {
            Token op = advance();

            if (!std::holds_alternative<ast::VarExpr>(left->kind) &&
                !std::holds_alternative<ast::FieldExpr>(left->kind) &&
                !std::holds_alternative<ast::IndexExpr>(left->kind)) {
                ctx.errors.add(op.loc, op.text.length(), "Invalid increment/decrement target");
                return left;
            }

            auto* one = make_expr(ctx, ast::IntExpr{ 1 }, op.loc);
            auto* bin = make_binary(left, one, op.type == TOKEN_PLUS_PLUS ? TOKEN_PLUS : TOKEN_MINUS);
            left = make_expr(ctx, ast::AssignExpr{ left, bin }, left->loc);
            continue;
        }

        if (match(TOKEN_AS)) {
            ast::CastKind kind = ast::CastKind::ValueCast;

            if (match(TOKEN_NOT)) { // !
                kind = ast::CastKind::Bitcast;
            }

            const ast::Type* target = parse_type(false, current_type_params);

            if (!target) {
                ctx.errors.add(current.loc, current.text.length(), "Expected type after cast");
                return left;
            }

            left = make_cast(left, target, kind);
            continue;
        }

        break;
    }

    return left;
}

ast::Expr* Parser::make_binary(ast::Expr* left, ast::Expr* right, TokenType op) {
    SourceLocation loc = left ? left->loc : SourceLocation{};
    return make_expr(ctx, ast::BinaryExpr{
        left,
        right,
        get_op_from_token(op)
    }, loc);
}
ast::Expr* Parser::make_cast(ast::Expr* value, const ast::Type* target, ast::CastKind kind) {
    SourceLocation loc = value ? value->loc : SourceLocation{};

    return make_expr(ctx,
        ast::CastExpr{
            value,
            target,
            kind
        },
        loc
    );
}

const ast::Type* Parser::parse_type(bool allow_implicit_void, const std::vector<std::string>* type_params) {

    if (match(TOKEN_STAR)) {
        const ast::Type* base = parse_type(allow_implicit_void, type_params);
        if (!base) return nullptr;
        return ctx.types.get_pointer(base);
    }

    if (match(TOKEN_AMP)) {
        const ast::Type* base = parse_type(allow_implicit_void, type_params);
        if (!base) return nullptr;
        return ctx.types.get_reference(base);
    }

    if (match(TOKEN_VOID))    return ctx.types.get_builtin(TypeKind::Void);
    if (match(TOKEN_BOOL))    return ctx.types.get_builtin(TypeKind::Bool);

    if (match(TOKEN_I8))      return ctx.types.get_builtin(TypeKind::I8);
    if (match(TOKEN_I16))     return ctx.types.get_builtin(TypeKind::I16);
    if (match(TOKEN_I32))     return ctx.types.get_builtin(TypeKind::I32);
    if (match(TOKEN_I64))     return ctx.types.get_builtin(TypeKind::I64);

    if (match(TOKEN_U8))      return ctx.types.get_builtin(TypeKind::U8);
    if (match(TOKEN_U16))     return ctx.types.get_builtin(TypeKind::U16);
    if (match(TOKEN_U32))     return ctx.types.get_builtin(TypeKind::U32);
    if (match(TOKEN_U64))     return ctx.types.get_builtin(TypeKind::U64);

    if (match(TOKEN_F32))     return ctx.types.get_builtin(TypeKind::F32);
    if (match(TOKEN_F64))     return ctx.types.get_builtin(TypeKind::F64);

    if (match(TOKEN_STR_TYPE)) return ctx.types.get_builtin(TypeKind::String);
    if (match(TOKEN_CHAR_TYPE)) return ctx.types.get_builtin(TypeKind::U8);

    if (match(TOKEN_IDENT)) {
        std::string name(previous.text);

        while (match(TOKEN_COLON_COLON)) {
            name += "::";
            name += expect(TOKEN_IDENT, "Expected identifier after ::").text;
        }

        if (type_params) {
            for (const auto& p : *type_params) {
                if (name == p) {
                    return ctx.types.get_generic_param(std::string(p));
                }
            }
        }

        if (match(TOKEN_LT)) {
            std::vector<const ast::Type*> args;
            do {
                const ast::Type* a = parse_type(false, type_params);
                args.push_back(a);
            } while (match(TOKEN_COMMA));
            expect(TOKEN_GT, "Expected '>' after type arguments");
            return ctx.types.get_deferred_generic(name, args);
        }

        return ctx.types.get_struct(name);
    }

    if (allow_implicit_void) {
        return ctx.types.get_builtin(TypeKind::Void);
    }

    ctx.errors.add(current.loc, current.text.length(), "Expected type");
    return nullptr;
}
} // namespace quant::ps
