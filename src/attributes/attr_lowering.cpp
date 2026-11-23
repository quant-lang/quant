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

// Fully qualified path of a hook function. Library/std hooks live in their
// own module namespace, so a bare name would not resolve in the entry module.
struct HookCall {
    std::vector<std::string> path;
};

void collect_dyn_hooks(const std::vector<ast::Stmt*>& stmts,
                       const std::vector<std::string>& module_ns,
                       std::vector<std::string>& ns_stack,
                       std::vector<HookCall>& onbegin,
                       std::vector<HookCall>& onend) {
    auto add_hook = [&](ast::FuncStmt* fn, std::vector<HookCall>& out) {
        if (!fn) return;
        auto path = module_ns;
        path.insert(path.end(), ns_stack.begin(), ns_stack.end());
        path.push_back(fn->name);
        out.push_back(HookCall{std::move(path)});
    };

    for (auto* s : stmts) {
        if (!s) continue;
        if (auto* d = std::get_if<ast::DynamicStmt>(&s->kind)) {
            add_hook(d->on_begin_func, onbegin);
            add_hook(d->on_end_func, onend);
        } else if (auto* ns = std::get_if<ast::NamespaceStmt>(&s->kind)) {
            if (!ns->body) continue;
            ns_stack.push_back(ns->name);
            collect_dyn_hooks(ns->body->stmts, module_ns, ns_stack, onbegin, onend);
            ns_stack.pop_back();
        }
    }
}

// Mirror Linker::find_entry(): an @entry function wins globally over any
// function simply named "main".
template<class Pred>
ast::Stmt* find_entry_stmt(const std::vector<ast::Stmt*>& stmts, Pred pred) {
    for (auto* s : stmts) {
        if (!s) continue;
        if (auto* fn = std::get_if<ast::FuncStmt>(&s->kind)) {
            if (pred(*fn)) return s;
        } else if (auto* ns = std::get_if<ast::NamespaceStmt>(&s->kind)) {
            if (ns->body) {
                if (auto* found = find_entry_stmt(ns->body->stmts, pred)) return found;
            }
        }
    }
    return nullptr;
}

ast::Stmt* find_entry_main(const std::vector<modules::Module*>& modules) {
    auto has_entry_attr = [](const ast::FuncStmt& fn) {
        return std::any_of(fn.attributes.begin(), fn.attributes.end(),
                           [](const ast::Attribute& a) { return a.name == "entry"; });
    };
    auto is_named_main = [](const ast::FuncStmt& fn) { return fn.name == "main"; };

    for (auto* mod : modules) {
        if (mod) {
            if (auto* s = find_entry_stmt(mod->ast, has_entry_attr)) return s;
        }
    }
    for (auto* mod : modules) {
        if (mod) {
            if (auto* s = find_entry_stmt(mod->ast, is_named_main)) return s;
        }
    }
    return nullptr;
}

// A call statement to the hook at `path` (e.g. std::io::io_dyn_on_begin),
// inserted into the entry function.
ast::Stmt* make_func_call_stmt(CompilerContext& ctx,
                               const std::vector<std::string>& path,
                               const SourceLocation& loc) {
    ast::Expr* callee_expr = nullptr;
    for (const auto& part : path) {
        auto* part_expr = make_expr(ctx, ast::VarExpr{part}, loc);
        callee_expr = callee_expr
            ? make_expr(ctx, ast::NamespaceExpr{callee_expr, part_expr}, loc)
            : part_expr;
    }

    auto* expr = memory::make<ast::Expr>(ctx.ast_arena);
    expr->kind = ast::CallExpr{ callee_expr, {}, {}, {} };
    expr->loc = loc;

    auto* stmt = memory::make<ast::Stmt>(ctx.ast_arena);
    stmt->kind = ast::ExprStmt{ expr };
    stmt->loc = loc;
    return stmt;
}

} // namespace

void lower_dynamics(CompilerContext& ctx, const std::vector<modules::Module*>& modules) {
    std::vector<HookCall> onbegin_hooks, onend_hooks;
    for (auto* mod : modules) {
        if (!mod) continue;
        std::vector<std::string> ns_stack;
        collect_dyn_hooks(mod->ast, mod->namespace_path, ns_stack, onbegin_hooks, onend_hooks);
    }
    if (onbegin_hooks.empty() && onend_hooks.empty()) {
        return;
    }

    ast::Stmt* main_stmt = find_entry_main(modules);
    auto* main_fn = main_stmt ? std::get_if<ast::FuncStmt>(&main_stmt->kind) : nullptr;
    if (!main_fn || !main_fn->body) {
        return;
    }

    // @onbegin calls: at the start of the body, in dynamic declaration order.
    // @onend calls: right before the trailing return (if any), otherwise at
    // the end of the body; reverse dynamic order (destructors).
    auto* body = main_fn->body;
    auto& stmts = body->stmts;

    std::vector<ast::Stmt*> onend_calls;
    for (auto it = onend_hooks.rbegin(); it != onend_hooks.rend(); ++it) {
        onend_calls.push_back(make_func_call_stmt(ctx, it->path, main_stmt->loc));
    }

    std::vector<ast::Stmt*> new_stmts;
    for (const auto& hook : onbegin_hooks) {
        new_stmts.push_back(make_func_call_stmt(ctx, hook.path, main_stmt->loc));
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
