#include "quant/attributes/attr_lowering.h"

#include <algorithm>
#include <utility>
#include <variant>

#include "quant/support/alloc.h"

namespace quant::attrs {

namespace {

template<class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

template<class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

ast::Expr* make_expr(CompilerContext& ctx, ast::ExprKind kind, const SourceLocation& loc) {
    auto* expr = memory::make<ast::Expr>(ctx.ast_arena);
    expr->kind = std::move(kind);
    expr->loc = loc;
    return expr;
}

// Types we can initialize with `0 as/as! T` using the current cast rules.
// Aggregates and generic types are excluded because a scalar cast can't
// produce a properly zero-initialized value of those types.
bool can_lower_to_zero_cast(const ast::Type* type) {
    if (!type) return false;
    switch (type->kind) {
        case ast::TypeKind::Bool:
        case ast::TypeKind::I8:
        case ast::TypeKind::I16:
        case ast::TypeKind::I32:
        case ast::TypeKind::I64:
        case ast::TypeKind::U8:
        case ast::TypeKind::U16:
        case ast::TypeKind::U32:
        case ast::TypeKind::U64:
        case ast::TypeKind::F32:
        case ast::TypeKind::F64:
        case ast::TypeKind::Enum:
        case ast::TypeKind::Pointer:
        case ast::TypeKind::Reference:
        case ast::TypeKind::NullPtr:
            return true;
        case ast::TypeKind::Struct:
        case ast::TypeKind::Generic:
        case ast::TypeKind::Void:
        default:
            return false;
    }
}

} // namespace

AttrEvaluator::AttrEvaluator(CompilerContext& ctx, std::vector<std::string> ns)
    : ctx(ctx), module_namespace(std::move(ns)) {}

void AttrEvaluator::evaluate(const std::vector<ast::Stmt*>& stmts) {
    for (auto* stmt : stmts) {
        visit_stmt(stmt);
    }
}

void AttrEvaluator::visit_block(ast::Block* block) {
    if (!block)
        return;
    for (auto* stmt : block->stmts) {
        visit_stmt(stmt);
    }
}

void AttrEvaluator::visit_stmt(ast::Stmt* stmt) {
    if (!stmt) return;

    std::visit(overloaded{
        [&](ast::VarDecl& node) {
            lower_var_decl(node, stmt->loc);
        },
        [&](ast::StructDecl& node) {
            lower_struct_decl(node);
        },
        [&](ast::FuncStmt& node) {
            visit_block(node.body);
        },
        [&](ast::NamespaceStmt& node) {
            visit_block(node.body);
        },
        [&](ast::IfStmt& node) {
            visit_block(node.then_block);
            for (auto* ei = node.else_if; ei; ei = ei->next) {
                visit_block(ei->then_block);
                if (ei->else_block) {
                    visit_block(ei->else_block);
                }
            }
            visit_block(node.else_block);
        },
        [&](ast::WhileStmt& node) {
            visit_block(node.body);
        },
        [&](ast::BlockStmt& node) {
            visit_block(node.body);
        },
        [&](ast::SwitchStmt& node) {
            for (auto& cs : node.cases) {
                visit_block(cs.body);
            }
            visit_block(node.default_block);
        },
        [&](ast::RegionStmt& node) {
            visit_block(node.body);
        },
        [](auto&) {
            // No runtime-attribute lowering needed for other statements.
        }
    }, stmt->kind);
}

void AttrEvaluator::lower_struct_decl(ast::StructDecl& str) {
    for (auto& value : str.fields) {
        if (auto* field = std::get_if<ast::StructField>(&value)) {
            if (has_init_attr(field->attributes)) {
                if (!field->default_value && can_lower_to_zero_cast(field->type)) {
                    if (auto* zero = make_zero_expr(field->type, {})) {
                        field->default_value = zero;
                        // Keep @init as a marker that this field is initialized by
                        // default (useful for semantic field-init checks).
                    }
                    // If we couldn't create a zero expr, keep @init as fallback.
                } else if (field->default_value) {
                    // Explicit default provided; @init marker not needed.
                    remove_init_attr(field->attributes);
                }
                // For non-lowerable types without a default, keep @init so
                // semantic analysis can continue to treat the field as
                // intentionally uninitialized/guarded.
            }
        } else if (auto* fn = std::get_if<ast::FuncStmt>(&value)) {
            visit_block(fn->body);
        }
    }
}

void AttrEvaluator::lower_var_decl(ast::VarDecl& var, const SourceLocation& loc) {
    if (!has_init_attr(var.attributes)) {
        return;
    }

    if (!var.value) {
        // @init T var;  =>  T var = 0 as/as! T;
        if (can_lower_to_zero_cast(var.type)) {
            if (auto* zero = make_zero_expr(var.type, loc)) {
                var.value = zero;
                remove_init_attr(var.attributes);
            }
            // Keep @init if we couldn't create the zero initializer.
            return;
        }

        // Leave types we can't lower here (such as aggregates and generics)
        // to the existing semantic/IR handling.
        return;
    }

    // An explicit initializer makes @init unnecessary.
    remove_init_attr(var.attributes);
}

ast::Expr* AttrEvaluator::make_zero_expr(const ast::Type* type, const SourceLocation& loc) {
    if (!type) return nullptr;

    // Bool zero-initializes to false directly
    if (type->kind == ast::TypeKind::Bool) {
        return make_expr(ctx, ast::BoolExpr{false}, loc);
    }

    // Base zero literal
    ast::Expr* zero = make_expr(ctx, ast::IntExpr{0, false, ast::TypeKind::Count}, loc);

    // Choose cast kind based on type/size:
    //  - pointers/references: 0 as! T
    //  - 4-byte kinds:        0 as! T  (bitcast), by requested  rule
    //  - 1/2/8-byte kinds:    0 as  T  (value cast)
    //  - fallback:            0 as! T
    ast::CastKind cast_kind = ast::CastKind::ValueCast;
    if (type->kind == ast::TypeKind::Pointer || type->kind == ast::TypeKind::Reference) {
        cast_kind = ast::CastKind::Bitcast;
    } else {
        const int sz = ctx.types.type_size(type);
        if (sz == 4) {
            cast_kind = ast::CastKind::Bitcast;
        } else if (sz == 1 || sz == 2 || sz == 8) {
            cast_kind = ast::CastKind::ValueCast;
        } else {
            cast_kind = ast::CastKind::Bitcast;
        }
    }

    return make_expr(ctx, ast::CastExpr{zero, type, cast_kind}, loc);
}

bool AttrEvaluator::has_init_attr(const std::vector<ast::Attribute>& attrs) {
    return std::any_of(attrs.begin(), attrs.end(), [](const ast::Attribute& a) {
        return a.name == "init";
    });
}

void AttrEvaluator::remove_init_attr(std::vector<ast::Attribute>& attrs) {
    attrs.erase(std::remove_if(attrs.begin(), attrs.end(), [](const ast::Attribute& a) {
        return a.name == "init";
    }), attrs.end());
}

} // namespace quant::attrs
