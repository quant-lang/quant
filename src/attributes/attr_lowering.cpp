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
        [&](ast::DynamicStmt& node) {
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

namespace {

void collect_dynamics(const std::vector<ast::Stmt*>& stmts, std::vector<ast::DynamicStmt*>& out) {
    for (auto* s : stmts) {
        if (!s) 
            continue;
        if (auto* d = std::get_if<ast::DynamicStmt>(&s->kind)) {
            out.push_back(d);
        } else if (auto* ns = std::get_if<ast::NamespaceStmt>(&s->kind)) {
            if (ns->body) 
                collect_dynamics(ns->body->stmts, out);
        }
    }
}

// Find the program entry statement: the one whose function is marked with
// @entry, or the function named "main".
ast::Stmt* find_entry_main_stmt(std::vector<ast::Stmt*>& stmts) {
    ast::Stmt* by_name = nullptr;
    for (auto* s : stmts) {
        if (!s) continue;
        if (auto* fn = std::get_if<ast::FuncStmt>(&s->kind)) {
            if (std::any_of(fn->attributes.begin(), fn->attributes.end(),
                            [](const ast::Attribute& a) { return a.name == "entry"; })) {
                return s;
            }
            if (!by_name && fn->name == "main") {
                by_name = s;
            }
        } else if (auto* ns = std::get_if<ast::NamespaceStmt>(&s->kind)) {
            if (ns->body) {
                if (auto* found = find_entry_main_stmt(ns->body->stmts)) return found;
            }
        }
    }
    return by_name;
}

// A bare call statement to the given function, e.g. for the @onbegin calls
// inserted into the entry main function.
ast::Stmt* make_func_call_stmt(CompilerContext& ctx, const ast::FuncStmt* callee, const SourceLocation& loc) {
    auto* callee_expr = memory::make<ast::Expr>(ctx.ast_arena);
    callee_expr->kind = ast::VarExpr{ callee->name };

    auto* expr = memory::make<ast::Expr>(ctx.ast_arena);
    expr->kind = ast::CallExpr{ callee_expr, {}, {}, {} };
    expr->loc = loc;

    auto* stmt = memory::make<ast::Stmt>(ctx.ast_arena);
    stmt->kind = ast::ExprStmt{ expr };
    stmt->loc = loc;
    return stmt;
}

} // namespace

void lower_dynamics(CompilerContext& ctx, modules::Module& mod) {
    std::vector<ast::DynamicStmt*> dynamics;
    collect_dynamics(mod.ast, dynamics);

    std::vector<ast::DynamicStmt*> onbegin_dyns, onend_dyns;
    for (auto* d : dynamics) {
        if (d->on_begin_func) onbegin_dyns.push_back(d);
        if (d->on_end_func) onend_dyns.push_back(d);
    }
    if (onbegin_dyns.empty() && onend_dyns.empty()) {
        return;
    }

    ast::Stmt* main_stmt = find_entry_main_stmt(mod.ast);
    auto* main_fn = main_stmt ? std::get_if<ast::FuncStmt>(&main_stmt->kind) : nullptr;
    if (!main_fn || !main_fn->body) {
        // No entry main (e.g. library modules): dynamics are just metadata,
        // there is nothing to substitute calls into.
        return;
    }

    // @onbegin calls: at the start of the body, in dynamic declaration order.
    // @onend calls: right before the trailing return (if any), otherwise at
    // the end of the body; reverse dynamic order (destructors).
    auto* body = main_fn->body;
    auto& stmts = body->stmts;

    std::vector<ast::Stmt*> onend_calls;
    for (auto it = onend_dyns.rbegin(); it != onend_dyns.rend(); ++it) {
        onend_calls.push_back(make_func_call_stmt(ctx, (*it)->on_end_func, main_stmt->loc));
    }

    std::vector<ast::Stmt*> new_stmts;
    for (auto* d : onbegin_dyns) {
        new_stmts.push_back(make_func_call_stmt(ctx, d->on_begin_func, main_stmt->loc));
    }
    new_stmts.insert(new_stmts.end(), stmts.begin(), stmts.end());

    if (!stmts.empty() && std::holds_alternative<ast::ReturnStmt>(stmts.back()->kind)) {
        new_stmts.insert(new_stmts.end() - 1, onend_calls.begin(), onend_calls.end());
    } else {
        new_stmts.insert(new_stmts.end(), onend_calls.begin(), onend_calls.end());
    }
    body->stmts = std::move(new_stmts);
}

} // namespace quant::attrs
