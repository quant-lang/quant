#include "quant/frontend/ast_deserialize.h"

#include <cstring>
#include <stdexcept>

#include "quant/support/alloc.h"

namespace quant::ast {

AstDeserializer::AstDeserializer(CompilerContext& ctx_)
    : ctx(ctx_) {}

// Primitive readers

uint8_t AstDeserializer::read_u8() {
    uint8_t v = 0;
    stream->read(reinterpret_cast<char*>(&v), 1);
    return v;
}

uint32_t AstDeserializer::read_u32() {
    uint8_t buf[4];
    stream->read(reinterpret_cast<char*>(buf), 4);
    return static_cast<uint32_t>(buf[0])
         | (static_cast<uint32_t>(buf[1]) << 8)
         | (static_cast<uint32_t>(buf[2]) << 16)
         | (static_cast<uint32_t>(buf[3]) << 24);
}

int32_t AstDeserializer::read_i32() {
    return static_cast<int32_t>(read_u32());
}

uint64_t AstDeserializer::read_u64() {
    uint64_t lo = read_u32();
    uint64_t hi = read_u32();
    return lo | (hi << 32);
}

int64_t AstDeserializer::read_i64() {
    return static_cast<int64_t>(read_u64());
}

double AstDeserializer::read_f64() {
    int64_t raw = read_i64();
    double v;
    std::memcpy(&v, &raw, sizeof(double));
    return v;
}

std::string AstDeserializer::read_string() {
    uint32_t len = read_u32();
    if (len == 0) return {};
    std::string s(len, '\0');
    stream->read(&s[0], len);
    return s;
}

SourceLocation AstDeserializer::read_loc() {
    SourceLocation loc;
    loc.file = source_file;
    loc.line = read_i32();
    loc.column = read_i32();
    loc.length = read_i32();
    return loc;
}

// Helpers to emplace the correct variant alternative by tag byte

static void set_expr_kind(Expr* e, uint8_t tag) {
    switch (tag) {
        case 0:  e->kind.emplace<IntExpr>(); break;
        case 1:  e->kind.emplace<BoolExpr>(); break;
        case 2:  e->kind.emplace<NullPtrExpr>(); break;
        case 3:  e->kind.emplace<FloatExpr>(); break;
        case 4:  e->kind.emplace<StringExpr>(); break;
        case 5:  e->kind.emplace<CharExpr>(); break;
        case 6:  e->kind.emplace<VarExpr>(); break;
        case 7:  e->kind.emplace<BinaryExpr>(); break;
        case 8:  e->kind.emplace<UnaryExpr>(); break;
        case 9:  e->kind.emplace<AssignExpr>(); break;
        case 10: e->kind.emplace<CallExpr>(); break;
        case 11: e->kind.emplace<FieldExpr>(); break;
        case 12: e->kind.emplace<NamespaceExpr>(); break;
        case 13: e->kind.emplace<CastExpr>(); break;
        case 14: e->kind.emplace<TypeExpr>(); break;
        case 15: e->kind.emplace<IndexExpr>(); break;
        case 16: e->kind.emplace<StructInitExpr>(); break;
        case 17: e->kind.emplace<SizeofExpr>(); break;
        default: e->kind.emplace<IntExpr>(); break;
    }
}

static void set_stmt_kind(Stmt* s, uint8_t tag) {
    switch (tag) {
        case 0:  s->kind.emplace<ExprStmt>(); break;
        case 1:  s->kind.emplace<ReturnStmt>(); break;
        case 2:  s->kind.emplace<BreakStmt>(); break;
        case 3:  s->kind.emplace<ContinueStmt>(); break;
        case 4:  s->kind.emplace<IfStmt>(); break;
        case 5:  s->kind.emplace<WhileStmt>(); break;
        case 6:  s->kind.emplace<BlockStmt>(); break;
        case 7:  s->kind.emplace<SwitchStmt>(); break;
        case 8:  s->kind.emplace<VarDecl>(); break;
        case 9:  s->kind.emplace<StructDecl>(); break;
        case 10: s->kind.emplace<FuncStmt>(); break;
        case 11: s->kind.emplace<NamespaceStmt>(); break;
        case 12: s->kind.emplace<RegionStmt>(); break;
        case 13: s->kind.emplace<EnumDecl>(); break;
        case 14: s->kind.emplace<ModuleDecl>(); break;
        case 15: s->kind.emplace<LoadStmt>(); break;
        case 16: s->kind.emplace<UsingStmt>(); break;
        default: s->kind.emplace<ExprStmt>(); break;
    }
}

// Type deserialization

const Type* AstDeserializer::deserialize_type() {
    uint8_t flag = read_u8();
    if (flag == 0) return nullptr;

    auto* t = alloc_default<Type>();

    uint8_t kind_byte = read_u8();
    t->kind = static_cast<TypeKind>(kind_byte);

    // struct_name (opt_str: u8 flag + u32 len + bytes)
    uint8_t has_name = read_u8();
    if (has_name) {
        t->struct_name = read_string();
    }

    // pointed type (recursive nullable)
    t->pointed = deserialize_type();

    // type args: u32 count, then each arg is a recursive type
    uint32_t arg_count = read_u32();
    for (uint32_t i = 0; i < arg_count; i++) {
        t->type_args.push_back(deserialize_type());
    }

    return t;
}

// EXPRS deserialization

Expr* AstDeserializer::deserialize_expr() {
    uint8_t flag = read_u8();
    if (flag == 0) return nullptr;

    auto* e = alloc_default<Expr>();

    uint8_t kind_byte = read_u8();
    set_expr_kind(e, kind_byte);
    e->loc = read_loc();
    e->is_comptime = read_u8() != 0;

    switch (e->kind.index()) {
        case 0: { // IntExpr
            auto& v = std::get<IntExpr>(e->kind);
            v.value = read_i64();
            break;
        }
        case 1: { // BoolExpr
            auto& v = std::get<BoolExpr>(e->kind);
            v.value = read_u8() != 0;
            break;
        }
        case 2: { // NullPtrExpr
            break;
        }
        case 3: { // FloatExpr
            auto& v = std::get<FloatExpr>(e->kind);
            v.value = read_f64();
            break;
        }
        case 4: { // StringExpr
            auto& v = std::get<StringExpr>(e->kind);
            v.value = read_string();
            break;
        }
        case 5: { // CharExpr
            auto& v = std::get<CharExpr>(e->kind);
            v.value = read_u8();
            break;
        }
        case 6: { // VarExpr
            auto& v = std::get<VarExpr>(e->kind);
            v.name = read_string();
            break;
        }
        case 7: { // BinaryExpr
            auto& v = std::get<BinaryExpr>(e->kind);
            v.op = static_cast<BinaryOp>(read_u8());
            v.lhs = deserialize_expr();
            v.rhs = deserialize_expr();
            break;
        }
        case 8: { // UnaryExpr
            auto& v = std::get<UnaryExpr>(e->kind);
            v.op = static_cast<UnaryOp>(read_u8());
            v.operand = deserialize_expr();
            break;
        }
        case 9: { // AssignExpr
            auto& v = std::get<AssignExpr>(e->kind);
            v.target = deserialize_expr();
            v.value = deserialize_expr();
            break;
        }
        case 10: { // CallExpr
            auto& v = std::get<CallExpr>(e->kind);
            v.callee = deserialize_expr();
            uint32_t argc = read_u32();
            v.args.reserve(argc);
            for (uint32_t i = 0; i < argc; i++) {
                v.args.push_back(deserialize_expr());
            }
            uint32_t tac = read_u32();
            v.type_args.reserve(tac);
            for (uint32_t i = 0; i < tac; i++) {
                v.type_args.push_back(deserialize_type());
            }
            break;
        }
        case 11: { // FieldExpr
            auto& v = std::get<FieldExpr>(e->kind);
            v.base = deserialize_expr();
            v.field = read_string();
            break;
        }
        case 12: { // NamespaceExpr
            auto& v = std::get<NamespaceExpr>(e->kind);
            v.left = deserialize_expr();
            v.right = deserialize_expr();
            break;
        }
        case 13: { // CastExpr
            auto& v = std::get<CastExpr>(e->kind);
            v.value = deserialize_expr();
            v.target = deserialize_type();
            v.kind = static_cast<CastKind>(read_u8());
            break;
        }
        case 14: { // TypeExpr
            auto& v = std::get<TypeExpr>(e->kind);
            v.type = deserialize_type();
            break;
        }
        case 15: { // IndexExpr
            auto& v = std::get<IndexExpr>(e->kind);
            v.base = deserialize_expr();
            v.index = deserialize_expr();
            break;
        }
        case 16: { // StructInitExpr
            auto& v = std::get<StructInitExpr>(e->kind);
            std::string name = read_string();
            v.type_ref = deserialize_expr();
            uint32_t argc = read_u32();
            v.args.reserve(argc);
            for (uint32_t i = 0; i < argc; i++) {
                v.args.push_back(deserialize_expr());
            }
            uint32_t tac = read_u32();
            v.type_args.reserve(tac);
            for (uint32_t i = 0; i < tac; i++) {
                v.type_args.push_back(deserialize_type());
            }
            break;
        }
        case 17: { // SizeofExpr
            auto& v = std::get<SizeofExpr>(e->kind);
            v.type = deserialize_type();
            break;
        }
    }

    return e;
}

// Block deserialization

Block* AstDeserializer::deserialize_block() {
    uint8_t flag = read_u8();
    if (flag == 0) return nullptr;

    auto* block = alloc_default<Block>();

    uint64_t count = read_u64();
    block->stmts.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; i++) {
        block->stmts.push_back(deserialize_stmt());
    }

    return block;
}

// FuncArg deserialization

FuncArg AstDeserializer::read_func_arg() {
    FuncArg arg;
    arg.name = read_string();
    arg.type = deserialize_type();
    arg.is_mut = read_u8() != 0;
    return arg;
}

// Function deserialization

void AstDeserializer::deserialize_func_inner(FuncStmt* fs) {
    fs->name = read_string();
    // Read and discard func-level location (loc is stored on Stmt, not FuncStmt)
    read_i32(); read_i32(); read_i32();
    fs->return_type = deserialize_type();

    uint32_t argc = read_u32();
    fs->args.reserve(argc);
    for (uint32_t i = 0; i < argc; i++) {
        fs->args.push_back(read_func_arg());
    }

    fs->is_extern = read_u8() != 0;
    fs->has_body = read_u8() != 0;
    fs->is_entry = read_u8() != 0;
    fs->is_forward = read_u8() != 0;
    fs->is_private = read_u8() != 0;

    // struct_name (opt_str)
    uint8_t has_sn = read_u8();
    if (has_sn) {
        auto sn = read_string();
        auto* s = memory::make<std::string>(ctx.ast_arena, std::move(sn));
        fs->struct_name = s->c_str();
    }

    fs->body = deserialize_block();

    // type_params: u32 count, then each is a Type serialized as TYPE_GENERIC
    uint32_t tpc = read_u32();
    for (uint32_t i = 0; i < tpc; i++) {
        const Type* tp = deserialize_type();
        if (tp) {
            fs->type_params.push_back(tp->struct_name);
        }
    }
}

// Struct member deserialization

StructValue AstDeserializer::deserialize_struct_member() {
    uint8_t is_method = read_u8();

    if (is_method) {
        FuncStmt fs;
        deserialize_func_inner(&fs);
        return StructValue(fs);
    } else {
        StructField sf;
        sf.name = read_string();
        sf.type = deserialize_type();
        sf.is_mut = read_u8() != 0;
        sf.is_private = read_u8() != 0;
        sf.default_value = deserialize_expr();
        return StructValue(sf);
    }
}

// STMTS deserialization

Stmt* AstDeserializer::deserialize_stmt() {
    auto* s = alloc_default<Stmt>();

    uint8_t kind_byte = read_u8();
    set_stmt_kind(s, kind_byte);
    s->loc = read_loc();

    switch (s->kind.index()) {
        case 0: { // ExprStmt
            auto& v = std::get<ExprStmt>(s->kind);
            v.expr = deserialize_expr();
            break;
        }
        case 1: { // ReturnStmt
            auto& v = std::get<ReturnStmt>(s->kind);
            v.value = deserialize_expr();
            break;
        }
        case 2: { // BreakStmt
            break;
        }
        case 3: { // ContinueStmt
            break;
        }
        case 4: { // IfStmt
            auto& v = std::get<IfStmt>(s->kind);
            v.condition = deserialize_expr();
            v.then_block = deserialize_block();

            // else-if chain
            uint8_t has_elif = read_u8();
            if (has_elif) {
                ElseIfStmt* prev = nullptr;
                while (true) {
                    auto* ef = alloc_default<ElseIfStmt>();
                    ef->condition = deserialize_expr();
                    ef->then_block = deserialize_block();
                    ef->else_block = deserialize_block();

                    if (prev) {
                        prev->next = ef;
                    } else {
                        v.else_if = ef;
                    }
                    prev = ef;

                    uint8_t more = read_u8();
                    if (more == 0) break;
                }
            }

            v.else_block = deserialize_block();
            break;
        }
        case 5: { // WhileStmt
            auto& v = std::get<WhileStmt>(s->kind);
            v.condition = deserialize_expr();
            v.body = deserialize_block();
            break;
        }
        case 6: { // BlockStmt
            auto& v = std::get<BlockStmt>(s->kind);
            v.body = deserialize_block();
            break;
        }
        case 7: { // SwitchStmt
            auto& v = std::get<SwitchStmt>(s->kind);
            v.condition = deserialize_expr();

            uint32_t case_count = read_u32();
            v.cases.reserve(case_count);
            for (uint32_t i = 0; i < case_count; i++) {
                CaseStmt cs;
                uint32_t vc = read_u32();
                cs.values.reserve(vc);
                for (uint32_t j = 0; j < vc; j++) {
                    cs.values.push_back(deserialize_expr());
                }
                cs.body = deserialize_block();
                v.cases.push_back(std::move(cs));
            }

            v.default_block = deserialize_block();
            break;
        }
        case 8: { // VarDecl
            auto& v = std::get<VarDecl>(s->kind);
            v.name = read_string();
            v.type = deserialize_type();
            v.value = deserialize_expr();
            v.is_mut = read_u8() != 0;
            break;
        }
        case 9: { // StructDecl
            auto& v = std::get<StructDecl>(s->kind);

            // ser_struct_node: nullable flag + struct data
            uint8_t has_sn = read_u8();
            if (has_sn) {
                v.name = read_string();
                // Skip inner loc (already read from ser_stmt's loc)
                read_i32(); read_i32(); read_i32();

                uint32_t member_count = read_u32();
                v.fields.reserve(member_count);
                for (uint32_t i = 0; i < member_count; i++) {
                    v.fields.push_back(deserialize_struct_member());
                }

                uint32_t tpc = read_u32();
                for (uint32_t i = 0; i < tpc; i++) {
                    const Type* tp = deserialize_type();
                    if (tp) v.type_params.push_back(tp->struct_name);
                }
            }
            break;
        }
        case 10: { // FuncStmt
            auto& v = std::get<FuncStmt>(s->kind);
            deserialize_func_inner(&v);
            break;
        }
        case 11: { // NamespaceStmt
            auto& v = std::get<NamespaceStmt>(s->kind);
            v.name = read_string();
            v.body = deserialize_block();
            break;
        }
        case 12: { // RegionStmt
            auto& v = std::get<RegionStmt>(s->kind);
            v.body = deserialize_block();
            break;
        }
        case 13: { // EnumDecl
            auto& v = std::get<EnumDecl>(s->kind);

            v.name = read_string();
            // Skip inner loc (already read from ser_stmt's loc)
            read_i32(); read_i32(); read_i32();

            uint64_t variant_count = read_u64();
            v.variants.reserve(static_cast<size_t>(variant_count));
            for (uint64_t i = 0; i < variant_count; i++) {
                v.variants.push_back(read_string());
            }
            break;
        }
        case 14: { // ModuleDecl
            auto& v = std::get<ModuleDecl>(s->kind);
            v.name = read_string();
            break;
        }
        case 15: { // LoadStmt
            auto& v = std::get<LoadStmt>(s->kind);
            v.module = read_string();
            break;
        }
        case 16: { // UsingStmt
            auto& v = std::get<UsingStmt>(s->kind);
            std::string full = read_string();
            size_t start = 0;
            while (true) {
                size_t sep = full.find("::", start);
                if (sep == std::string::npos) {
                    v.path.push_back(full.substr(start));
                    break;
                }
                v.path.push_back(full.substr(start, sep - start));
                start = sep + 2;
            }
            break;
        }
    }

    return s;
}

// Module deserialization (from file)

std::vector<Stmt*> AstDeserializer::deserialize_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("cannot open QAST file: " + path);
    }
    source_file = path;
    return deserialize(file);
}

std::vector<Stmt*> AstDeserializer::deserialize(std::istream& input_stream) {
    this->stream = &input_stream;

    // Read and verify magic: "QAST"
    uint8_t magic[4];
    input_stream.read(reinterpret_cast<char*>(magic), 4);
    if (magic[0] != 'Q' || magic[1] != 'A' || magic[2] != 'S' || magic[3] != 'T') {
        throw std::runtime_error("invalid QAST magic");
    }

    // Version
    uint32_t version = read_u32();
    if (version != 1) {
        throw std::runtime_error("unsupported QAST version: " + std::to_string(version));
    }

    // Statement count
    uint64_t count = read_u64();

    std::vector<Stmt*> stmts;
    stmts.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; i++) {
        stmts.push_back(deserialize_stmt());
    }

    return stmts;
}

} // namespace quant::ast
