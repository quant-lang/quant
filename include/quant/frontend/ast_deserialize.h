#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "quant/frontend/ast.h"
#include "quant/support/compiler_context.h"

namespace quant::ast {

// Deserializer for the QAST binary format by the self-hosted frontend
class AstDeserializer {
public:
    explicit AstDeserializer(CompilerContext& ctx);

    // Deserialize an entire module body from a QAST file
    // Returns the list of top-level statements
    std::vector<Stmt*> deserialize_file(const std::string& path);

    // Deserialize from an open binary stream.
    std::vector<Stmt*> deserialize(std::istream& stream);

private:
    CompilerContext& ctx;
    std::istream* stream = nullptr;
public:
    std::string source_file;  // for SourceLocation

    // Read primitives
    uint8_t read_u8();
    uint32_t read_u32();
    int32_t read_i32();
    int64_t read_i64();
    uint64_t read_u64();
    double read_f64();
    std::string read_string();

    // Arena allocation helpers (using ctx.ast_arena)
    template<typename T, typename... Args>
    T* alloc(Args&&... args) { return memory::make<T>(ctx.ast_arena, std::forward<Args>(args)...); }

    template<typename T>
    T* alloc_default() { return memory::make_default<T>(ctx.ast_arena); }

    // Read a SourceLocation (line, column, length from stream, file from source_file)
    SourceLocation read_loc();

    // Deserialize AST node types
    const Type* deserialize_type();
    Expr* deserialize_expr();
    Stmt* deserialize_stmt();
    Block* deserialize_block();
    FuncArg read_func_arg();
    void deserialize_func_inner(FuncStmt* fs);
    StructValue deserialize_struct_member();
};

} // namespace quant::ast
