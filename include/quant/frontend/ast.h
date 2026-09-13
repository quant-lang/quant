#pragma once

#include <variant>
#include <vector>
#include <unordered_map>
#include <string>
#include <memory>
#include <optional>

#include "utils/logger.h"

namespace quant
{
    struct CompilerContext;
}

namespace quant::ast {

    struct Expr;
    struct Stmt;
    struct Block;
    struct Attribute;
    struct ElseIfStmt;

    // Types

    enum class TypeKind {
        Void,
        Bool,

        I8,
        I16,
        I32,
        I64,

        U8,
        U16,
        U32,
        U64,

        F32,
        F64,

        String,

        Struct,
        Pointer,
        Reference,

        NullPtr,

        Generic,

        Count
    };

    struct Type {
        TypeKind kind;
        std::string struct_name; // struct only
        const Type* pointed; // ptr/reference only
        std::vector<const Type*> type_args;

        std::string to_string(CompilerContext& ctx) const;
    };

    // Expressions

    struct IntExpr {
        int64_t value;
    };

    struct BoolExpr {
        bool value;
    };

    struct NullPtrExpr {};

    struct FloatExpr {
        double value;
    };

    struct StringExpr {
        std::string value;
    };

    struct CharExpr {
        uint8_t value;
    };

    struct VarExpr {
        std::string name;
    };

    enum class BinaryOp {
        Add,
        Sub,
        Mul,
        Div,
        Eq,
        Neq,
        Lt,
        Lte,
        Gt,
        Gte,
        BitAnd,
        BitOr,
        LogicAnd,
        LogicOr
    };

    struct BinaryExpr {
        Expr* lhs;
        Expr* rhs;
        BinaryOp op;
    };
    enum class UnaryOp { Neg, Not, AddrOf };

    struct UnaryExpr{
        Expr* operand;
        UnaryOp op;
    };

    struct AssignExpr {
        Expr* target;
        Expr* value;
    };

    struct CallExpr {
        Expr* callee;
        std::vector<Expr*> args;
        std::vector<const Type*> type_args;
        // Set by the semantic pass when the call resolves to a concrete
        // generic instantiation (explicit or inferred type args), so the
        // IR generator can reference the mangled function name directly.
        std::string resolved_mangled_name;
    };

    struct FieldExpr {
        Expr* base;
        std::string field;
    };

    struct NamespaceExpr {
        Expr* left;
        Expr* right;
    };
    enum class CastKind {
        ValueCast,   // as
        Bitcast      // as!
    };
    struct CastExpr {
        Expr* value;
        const Type* target;
        CastKind kind;
    };

    struct TypeExpr {
        const Type* type;
    };

    struct IndexExpr {
        Expr* base;
        Expr* index;
    };

    struct StructInitExpr {
        Expr* type_ref;                          // VarExpr, NamespaceExpr, TypeExpr, or nullptr
        std::vector<const Type*> type_args;      // Generic type arguments (empty if non-generic)
        std::vector<Expr*> args;                 // Positional field values
    };

    struct SizeofExpr {
        const Type* type;
    };

    using ExprKind = std::variant<
        IntExpr,
        BoolExpr,
        NullPtrExpr,
        FloatExpr,
        StringExpr,
        CharExpr,
        VarExpr,
        BinaryExpr,
        UnaryExpr,
        AssignExpr,
        CallExpr,
        FieldExpr,
        NamespaceExpr,
        CastExpr,
        TypeExpr,
        IndexExpr,
        StructInitExpr,
        SizeofExpr
    >;

    struct Expr {
        ExprKind kind;
        SourceLocation loc;
        const Type* resolved_type = nullptr;
        bool is_comptime = false;
    };

    // Statements

    struct ExprStmt {
        Expr* expr = nullptr;
    };

    struct ReturnStmt {
        Expr* value = nullptr;
    };

    struct BreakStmt {};

    struct ContinueStmt {};

    struct IfStmt {
        Expr* condition = nullptr;
        Block* then_block = nullptr;
        ElseIfStmt* else_if = nullptr;  // first else-if branch in the chain (nullptr if none)
        Block* else_block = nullptr;
    };

    struct ElseIfStmt {
        Expr* condition = nullptr;
        Block* then_block = nullptr;
        Block* else_block = nullptr;  // plain else block (nullptr if the chain continues)
        ElseIfStmt* next = nullptr;   // next else-if branch in the chain (nullptr if none)
    };

    struct WhileStmt {
        Expr* condition = nullptr;
        Block* body = nullptr;
        Expr* for_step = nullptr;  // step of a desugared 'for' loop; 'continue' runs it before re-checking the condition (nullptr for plain while)
    };

    struct BlockStmt {
        Block* body = nullptr;
    };

    struct CaseStmt {
        std::vector<Expr*> values;                            // one or more constant case values sharing one body
        Block* body = nullptr;
        std::vector<std::optional<int64_t>> const_values;     // constant values, filled by semantic analysis
    };

    struct SwitchStmt {
        Expr* condition = nullptr;
        std::vector<CaseStmt> cases;
        Block* default_block = nullptr;   // nullptr if no default
    };

    struct FuncArg {
        std::string name;
        const Type* type;
        bool is_mut;
    };
    struct StructField {
        std::string name;
        const Type* type;
        bool is_mut = false;
        bool is_private = false;
        Expr* default_value = nullptr;
        std::vector<Attribute> attributes;
    };
    struct FuncStmt;
    using StructValue = std::variant<StructField, FuncStmt>;

    struct VarDecl {
        std::string name;
        const Type* type = nullptr;
        Expr* value = nullptr;
        bool is_mut = false;
        std::vector<Attribute> attributes;
    };

    struct StructDecl {
        std::string name;
        std::vector<StructValue> fields;
        std::vector<Attribute> attributes;
	    std::vector<std::string> type_params;
    };

    struct FuncStmt {
        std::string name;
        std::vector<FuncArg> args;
        const Type* return_type = nullptr;
        std::vector<std::string> type_params;

        bool is_extern = false;
        bool is_forward = false;
        bool is_entry = false;
        bool has_body = false;
        bool is_private = false;

        const char* struct_name = nullptr;

        Block* body = nullptr;
        std::vector<Attribute> attributes;
    };
    struct Attribute {
        std::string name;
        std::vector<Expr*> args;
    };
    struct NamespaceStmt {
        std::string name;
        Block* body = nullptr;
    };
    struct ModuleDecl {
        std::string name;
        std::vector<Attribute> attributes;
    };
    struct LoadStmt {
        std::string module;
    };
    struct UsingStmt {
        std::vector<std::string> path;
    };
    struct RegionStmt{
        std::string name;
        Block* body = nullptr;
    };

    struct EnumDecl {
        std::string name;
        std::vector<std::string> variants;
        std::vector<Attribute> attributes;
    };

    using StmtKind = std::variant<
        ExprStmt,
        ReturnStmt,
        BreakStmt,
        ContinueStmt,
        IfStmt,
        WhileStmt,
        BlockStmt,
        SwitchStmt,
        VarDecl,
        StructDecl,
        FuncStmt,
        NamespaceStmt,
        RegionStmt,
        EnumDecl,
        ModuleDecl,
        LoadStmt,
        UsingStmt
    >;

    struct Stmt {
        StmtKind kind;
        SourceLocation loc;
    };

    struct Block {
        std::vector<Stmt*> stmts;
    };

}
