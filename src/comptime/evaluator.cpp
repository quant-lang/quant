#include "quant/comptime/evaluator.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <variant>

#include "quant/support/symbol_path.h"

namespace quant::comptime {

namespace {

template<class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

template<class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

bool is_float_kind(ast::TypeKind kind) {
    return kind == ast::TypeKind::F32 || kind == ast::TypeKind::F64;
}

bool is_int_kind(ast::TypeKind kind) {
    return kind >= ast::TypeKind::Bool && kind <= ast::TypeKind::U64;
}

} // namespace

std::optional<int64_t> Evaluator::value_int(const Value& v) {
    if (const auto* i = std::get_if<int64_t>(&v.data)) {
        return *i;
    }
    return std::nullopt;
}

std::optional<double> Evaluator::value_float(const Value& v) {
    if (const auto* d = std::get_if<double>(&v.data)) {
        return *d;
    }
    return std::nullopt;
}

std::optional<bool> Evaluator::value_bool(const Value& v) {
    if (const auto* i = std::get_if<int64_t>(&v.data)) {
        return *i != 0;
    }
    if (const auto* d = std::get_if<double>(&v.data)) {
        return *d != 0.0;
    }
    return std::nullopt;
}

void Evaluator::fail(const std::string& reason) {
    if (!fail_reason_.empty()) return;
    fail_reason_ = reason;
}

void Evaluator::push_scope() {
    scopes_.emplace_back();
}

void Evaluator::pop_scope() {
    if (!scopes_.empty()) scopes_.pop_back();
}

void Evaluator::declare(const std::string& name, const Value& value) {
    if (!scopes_.empty()) {
        scopes_.back()[name] = value;
    }
}

Value* Evaluator::lookup_env(const std::string& name) {
    for (size_t i = scopes_.size(); i > scope_base_; --i) {
        auto it = scopes_[i - 1].find(name);
        if (it != scopes_[i - 1].end()) return &it->second;
    }
    return nullptr;
}

symb_t::Symbol* Evaluator::resolve_symbol_path(const std::vector<std::string>& path,
                                                     symb_t::Namespace** owner) {
    if (path.empty()) return nullptr;

    if (path.size() == 1 && call_depth_ == 0 && !owner) {
        return ctx.symbols.lookup(path[0]);
    }

    // Walk from the current namespace toward the root, mirroring the
    // resolution semantic analysis performs for qualified names.
    auto* ns = ctx.symbols.get_current_namespace();
    while (ns) {
        auto* target = ns;
        for (size_t i = 0; i + 1 < path.size() && target; ++i) {
            auto it = target->children.find(path[i]);
            if (it == target->children.end()) {
                target = nullptr;
                break;
            }
            target = it->second;
        }
        if (target) {
            auto sym_it = target->symbols.find(path.back());
            if (sym_it != target->symbols.end()) {
                if (owner) *owner = target;
                return sym_it->second;
            }
        }
        ns = ns->parent;
    }

    if (auto* first = ctx.symbols.lookup(path[0])) {
        if (std::holds_alternative<symb_t::EnumSymbol>(first->data)) {
            return ctx.symbols.lookup(path.back());
        }
    }

    return ctx.symbols.lookup_qualified(path);
}

std::optional<Value> Evaluator::eval_expr(const ast::Expr* expr) {
    if (!expr || has_failed()) return std::nullopt;

    if (++steps_used_ > kMaxSteps) {
        fail("compile-time expression exceeded the step budget (non-terminating loop?)");
        return std::nullopt;
    }

    return std::visit(overloaded{
        [&](const ast::IntExpr& n) -> std::optional<Value> {
            return Value{ expr->resolved_type ? expr->resolved_type :
                          ctx.types.get_builtin(ast::TypeKind::I32), n.value };
        },
        [&](const ast::BoolExpr& n) -> std::optional<Value> {
            return Value{ ctx.types.get_builtin(ast::TypeKind::Bool), n.value ? 1 : 0 };
        },
        [&](const ast::CharExpr& n) -> std::optional<Value> {
            return Value{ ctx.types.get_builtin(ast::TypeKind::U8),
                          static_cast<int64_t>(n.value) };
        },
        [&](const ast::FloatExpr& n) -> std::optional<Value> {
            ast::TypeKind kind = ast::TypeKind::F64;
            if (expr->resolved_type && expr->resolved_type->kind == ast::TypeKind::F32) {
                kind = ast::TypeKind::F32;
            }
            return Value{ ctx.types.get_builtin(kind), n.value };
        },
        [&](const ast::VarExpr& n) -> std::optional<Value> {
            return eval_var(n);
        },
        [&](const ast::NamespaceExpr& n) -> std::optional<Value> {
            return eval_namespace(n);
        },
        [&](const ast::BinaryExpr& n) -> std::optional<Value> {
            return eval_binary(n);
        },
        [&](const ast::UnaryExpr& n) -> std::optional<Value> {
            return eval_unary(n);
        },
        [&](const ast::AssignExpr& n) -> std::optional<Value> {
            if (const auto* var = std::get_if<ast::VarExpr>(&n.target->kind)) {
                auto value = eval_expr(n.value);
                if (!value) return std::nullopt;
                auto* local = lookup_env(var->name);
                if (!local) {
                    fail("assignment to a non-local variable inside a compile-time expression");
                    return std::nullopt;
                }
                *local = *value;
                return *value;
            }
            fail("unsupported assignment target in a compile-time expression");
            return std::nullopt;
        },
        [&](const ast::CallExpr& n) -> std::optional<Value> {
            return eval_call(n);
        },
        [&](const ast::CastExpr& n) -> std::optional<Value> {
            return eval_cast(n);
        },
        [&](const ast::SizeofExpr& n) -> std::optional<Value> {
            int size = ctx.types.type_size(n.type);
            if (size <= 0) {
                fail("sizeof: unsupported type or zero size");
                return std::nullopt;
            }
            return Value{ ctx.types.get_builtin(ast::TypeKind::U64),
                          static_cast<int64_t>(size) };
        },
        [&](const ast::NullPtrExpr&) -> std::optional<Value> {
            fail("null pointer is not supported in compile-time expressions");
            return std::nullopt;
        },
        [&](const ast::StringExpr&) -> std::optional<Value> {
            fail("strings are not supported in compile-time expressions");
            return std::nullopt;
        },
        [&](const ast::FieldExpr&) -> std::optional<Value> {
            fail("field access is not supported in compile-time expressions");
            return std::nullopt;
        },
        [&](const ast::IndexExpr&) -> std::optional<Value> {
            fail("indexing is not supported in compile-time expressions");
            return std::nullopt;
        },
        [&](const ast::StructInitExpr&) -> std::optional<Value> {
            fail("struct literals are not supported in compile-time expressions");
            return std::nullopt;
        },
        [&](const ast::TypeExpr&) -> std::optional<Value> {
            fail("a type cannot be used as a value in a compile-time expression");
            return std::nullopt;
        },
    }, expr->kind);
}

std::optional<Value> Evaluator::eval_var(const ast::VarExpr& node) {
    if (auto env = lookup_env(node.name)) {
        if (std::get_if<std::monostate>(&env->data)) {
            fail("use of an uninitialized local variable '" + node.name + "'");
            return std::nullopt;
        }
        return *env;
    }

    auto* sym = resolve_symbol_path({ node.name });
    if (sym && std::holds_alternative<symb_t::VarSymbol>(sym->data)) {
        const auto& vs = std::get<symb_t::VarSymbol>(sym->data);
        if (!vs.is_mut && vs.const_value.has_value()) {
            return Value{ vs.type, *vs.const_value };
        }
        fail("variable '" + node.name + "' is not a compile-time constant");
        return std::nullopt;
    }

    fail("cannot resolve variable '" + node.name + "' at compile time");
    return std::nullopt;
}

std::optional<Value> Evaluator::eval_namespace(const ast::NamespaceExpr& node) {
    ast::Expr tmp{ node };
    auto path = support::flatten_path(&tmp);
    auto* sym = resolve_symbol_path(path);
    if (sym && std::holds_alternative<symb_t::VarSymbol>(sym->data)) {
        const auto& vs = std::get<symb_t::VarSymbol>(sym->data);
        if (!vs.is_mut && vs.const_value.has_value()) {
            return Value{ vs.type, *vs.const_value };
        }
        fail("variable '" + support::join_namespace(path) + "' is not a compile-time constant");
        return std::nullopt;
    }
    fail("cannot resolve '" + support::join_namespace(path) + "' at compile time");
    return std::nullopt;
}

std::optional<Value> Evaluator::eval_unary(const ast::UnaryExpr& node) {
    auto operand = eval_expr(node.operand);
    if (!operand) return std::nullopt;

    switch (node.op) {
        case ast::UnaryOp::Not: {
            auto b = value_bool(*operand);
            if (!b) return std::nullopt;
            return Value{ ctx.types.get_builtin(ast::TypeKind::Bool), *b ? 0 : 1 };
        }
        case ast::UnaryOp::Neg: {
            if (auto i = value_int(*operand)) {
                return Value{ operand->type, static_cast<int64_t>(uint64_t{0} - static_cast<uint64_t>(*i)) };
            }
            if (auto d = value_float(*operand)) {
                return Value{ operand->type, -*d };
            }
            fail("cannot negate this value at compile time");
            return std::nullopt;
        }
        case ast::UnaryOp::AddrOf:
            fail("taking an address is not allowed in compile-time expressions");
            return std::nullopt;
    }
    fail("unsupported unary operation");
    return std::nullopt;
}

std::optional<Value> Evaluator::eval_binary(const ast::BinaryExpr& node) {
    // Short-circuit logical operators.
    if (node.op == ast::BinaryOp::LogicAnd || node.op == ast::BinaryOp::LogicOr) {
        auto lhs = eval_expr(node.lhs);
        if (!lhs) return std::nullopt;
        auto lb = value_bool(*lhs);
        if (!lb) return std::nullopt;
        if (node.op == ast::BinaryOp::LogicAnd && !*lb) {
            return Value{ ctx.types.get_builtin(ast::TypeKind::Bool), 0 };
        }
        if (node.op == ast::BinaryOp::LogicOr && *lb) {
            return Value{ ctx.types.get_builtin(ast::TypeKind::Bool), 1 };
        }
        auto rhs = eval_expr(node.rhs);
        if (!rhs) return std::nullopt;
        auto rb = value_bool(*rhs);
        if (!rb) return std::nullopt;
        return Value{ ctx.types.get_builtin(ast::TypeKind::Bool), *rb ? 1 : 0 };
    }

    auto lhs = eval_expr(node.lhs);
    if (!lhs) return std::nullopt;
    auto rhs = eval_expr(node.rhs);
    if (!rhs) return std::nullopt;

    const bool floaty = std::holds_alternative<double>(lhs->data) || std::holds_alternative<double>(rhs->data);
    const auto* bool_t = ctx.types.get_builtin(ast::TypeKind::Bool);

    if (!floaty) {
        auto li = value_int(*lhs);
        auto ri = value_int(*rhs);
        if (!li || !ri) {
            fail("non-numeric operands in a compile-time expression");
            return std::nullopt;
        }
        const auto* type = lhs->type;
        const int left_size = ctx.types.type_size(lhs->type);
        const int right_size = ctx.types.type_size(rhs->type);
        if (right_size > left_size || (right_size == left_size && rhs->type &&
            rhs->type->kind >= ast::TypeKind::U8 && rhs->type->kind <= ast::TypeKind::U64)) {
            type = rhs->type;
        }
        auto widen = [&](int64_t value, const ast::Type* source, int size) {
            if (!source || source == type || size >= ctx.types.type_size(type)) return value;
            const bool signed_source = source->kind >= ast::TypeKind::I8 &&
                                       source->kind <= ast::TypeKind::I64;
            switch (size) {
                case 1: return signed_source ? static_cast<int64_t>(static_cast<int8_t>(value))
                                             : static_cast<int64_t>(static_cast<uint8_t>(value));
                case 2: return signed_source ? static_cast<int64_t>(static_cast<int16_t>(value))
                                             : static_cast<int64_t>(static_cast<uint16_t>(value));
                case 4: return signed_source ? static_cast<int64_t>(static_cast<int32_t>(value))
                                             : static_cast<int64_t>(static_cast<uint32_t>(value));
                default: return value;
            }
        };
        const int64_t l = widen(*li, lhs->type, left_size);
        const int64_t r = widen(*ri, rhs->type, right_size);
        const uint64_t a = static_cast<uint64_t>(l);
        const uint64_t b = static_cast<uint64_t>(r);
        const bool unsigned_op = type && type->kind >= ast::TypeKind::U8 &&
                                 type->kind <= ast::TypeKind::U64;
        // Quant integer slots wrap at 64 bits, including narrow declared types.
        switch (node.op) {
            case ast::BinaryOp::Add: return Value{ type, static_cast<int64_t>(a + b) };
            case ast::BinaryOp::Sub: return Value{ type, static_cast<int64_t>(a - b) };
            case ast::BinaryOp::Mul: return Value{ type, static_cast<int64_t>(a * b) };
            case ast::BinaryOp::Div: {
                if (r == 0) {
                    fail("division by zero in a compile-time expression");
                    return std::nullopt;
                }
                if (unsigned_op) return Value{ type, static_cast<int64_t>(a / b) };
                if (l == (std::numeric_limits<int64_t>::min)() && r == -1) {
                    fail("integer division overflow in a compile-time expression");
                    return std::nullopt;
                }
                return Value{ type, l / r };
            }
            case ast::BinaryOp::Eq:  return Value{ bool_t, l == r ? 1 : 0 };
            case ast::BinaryOp::Neq: return Value{ bool_t, l != r ? 1 : 0 };
            case ast::BinaryOp::Lt:  return Value{ bool_t, (unsigned_op ? a < b : l < r) ? 1 : 0 };
            case ast::BinaryOp::Lte: return Value{ bool_t, (unsigned_op ? a <= b : l <= r) ? 1 : 0 };
            case ast::BinaryOp::Gt:  return Value{ bool_t, (unsigned_op ? a > b : l > r) ? 1 : 0 };
            case ast::BinaryOp::Gte: return Value{ bool_t, (unsigned_op ? a >= b : l >= r) ? 1 : 0 };
            case ast::BinaryOp::BitAnd: return Value{ type, l & r };
            case ast::BinaryOp::BitOr:  return Value{ type, l | r };
            default:
                fail("unsupported binary operation in a compile-time expression");
                return std::nullopt;
        }
    }

    // Floating path: widen ints to double.
    auto ld = value_float(*lhs);
    auto rd = value_float(*rhs);
    auto li = value_int(*lhs);
    auto ri = value_int(*rhs);
    if ((!ld && !li) || (!rd && !ri)) {
        fail("non-numeric operands in a compile-time expression");
        return std::nullopt;
    }
    const double dl = ld ? *ld : static_cast<double>(*li);
    const double dr = rd ? *rd : static_cast<double>(*ri);
    ast::TypeKind float_kind = ast::TypeKind::F64;
    if (auto t = ld ? lhs->type : rhs->type) {
        float_kind = t->kind;
    }
    const auto* ft = ctx.types.get_builtin(float_kind);

    switch (node.op) {
        case ast::BinaryOp::Add: return Value{ ft, dl + dr };
        case ast::BinaryOp::Sub: return Value{ ft, dl - dr };
        case ast::BinaryOp::Mul: return Value{ ft, dl * dr };
        case ast::BinaryOp::Div: {
            if (dr == 0.0) {
                fail("division by zero in a compile-time expression");
                return std::nullopt;
            }
            return Value{ ft, dl / dr };
        }
        case ast::BinaryOp::Eq:  return Value{ bool_t, dl == dr ? 1 : 0 };
        case ast::BinaryOp::Neq: return Value{ bool_t, dl != dr ? 1 : 0 };
        case ast::BinaryOp::Lt:  return Value{ bool_t, dl <  dr ? 1 : 0 };
        case ast::BinaryOp::Lte: return Value{ bool_t, dl <= dr ? 1 : 0 };
        case ast::BinaryOp::Gt:  return Value{ bool_t, dl >  dr ? 1 : 0 };
        case ast::BinaryOp::Gte: return Value{ bool_t, dl >= dr ? 1 : 0 };
        default:
            fail("unsupported operation on floating-point values in a compile-time expression");
            return std::nullopt;
    }
}

std::optional<Value> Evaluator::eval_cast(const ast::CastExpr& node) {
    auto value = eval_expr(node.value);
    if (!value) return std::nullopt;

    const ast::Type* target = node.target;
    if (!target) {
        fail("cast target type is missing");
        return std::nullopt;
    }

    if (node.kind == ast::CastKind::Bitcast) {
        if (is_int_kind(target->kind)) {
            if (auto i = value_int(*value)) return Value{ target, *i };
            if (auto d = value_float(*value)) {
                return Value{ target, value->type->kind == ast::TypeKind::F32
                    ? static_cast<int64_t>(std::bit_cast<uint32_t>(static_cast<float>(*d)))
                    : std::bit_cast<int64_t>(*d) };
            }
        } else if (is_float_kind(target->kind)) {
            if (auto d = value_float(*value)) return Value{ target, *d };
            if (auto i = value_int(*value)) {
                return Value{ target, target->kind == ast::TypeKind::F32
                    ? static_cast<double>(std::bit_cast<float>(static_cast<uint32_t>(*i)))
                    : std::bit_cast<double>(*i) };
            }
        }
        fail("unsupported bitcast in a compile-time expression");
        return std::nullopt;
    }

    if (is_float_kind(target->kind)) {
        if (auto d = value_float(*value)) return Value{ target, *d };
        if (auto i = value_int(*value)) return Value{ target, static_cast<double>(*i) };
        fail("invalid operand for numeric cast at compile time");
        return std::nullopt;
    }

    if (is_int_kind(target->kind)) {
        if (auto i = value_int(*value)) return Value{ target, *i };
        if (auto d = value_float(*value)) {
            if (!std::isfinite(*d) || *d < -0x1p63 || *d >= 0x1p63) {
                fail("float-to-integer cast is out of range at compile time");
                return std::nullopt;
            }
            return Value{ target, static_cast<int64_t>(*d) };
        }
        fail("invalid operand for numeric cast at compile time");
        return std::nullopt;
    }

    fail("cast to '" + target->to_string(ctx) + "' is not supported in compile-time expressions");
    return std::nullopt;
}

std::optional<Value> Evaluator::eval_call(const ast::CallExpr& node) {
    if (std::get_if<ast::FieldExpr>(&node.callee->kind)) {
        fail("method calls are not supported in compile-time expressions yet");
        return std::nullopt;
    }

    std::vector<Value> args;
    args.reserve(node.args.size());
    for (const auto* arg : node.args) {
        auto v = eval_expr(arg);
        if (!v) return std::nullopt;
        args.push_back(std::move(*v));
    }

    const ast::FuncStmt* fn = nullptr;
    auto* function_namespace = ctx.symbols.get_current_namespace();

    if (!node.resolved_mangled_name.empty()) {
        // Generic instantiation: the concrete body lives in generic_instantiations.
        const std::string& mangled = node.resolved_mangled_name;
        for (const auto& inst : ctx.generic_instantiations) {
            if (inst.stmt.name == mangled) {
                fn = &inst.stmt;
                function_namespace = ctx.symbols.resolve_namespace(inst.module_namespace);
                break;
            }
        }
        if (!fn) {
            fail("cannot find the concrete generic function '" + mangled +
                 "' for compile-time evaluation");
            return std::nullopt;
        }
    } else {
        if (!std::get_if<ast::VarExpr>(&node.callee->kind) &&
            !std::get_if<ast::NamespaceExpr>(&node.callee->kind)) {
            fail("unsupported callee syntax in a compile-time call");
            return std::nullopt;
        }
        auto path = support::flatten_path(node.callee);
        const std::string callee_name = support::join_namespace(path);
        auto* sym = resolve_symbol_path(path, &function_namespace);
        if (!sym) {
            fail("undefined function '" + callee_name + "'");
            return std::nullopt;
        }
        auto* fn_sym = std::get_if<symb_t::FuncSymbol>(&sym->data);
        if (!fn_sym) {
            fail("'" + callee_name + "' is not a function");
            return std::nullopt;
        }
        if (fn_sym->is_extern || !fn_sym->is_defined) {
            fail("cannot call extern/undefined function '" + callee_name + "' at compile time");
            return std::nullopt;
        }
        fn = fn_sym->func_decl;
        if (!fn) {
            fail("no body found for function '" + callee_name + "'");
            return std::nullopt;
        }
    }

    if (!fn->body) {
        fail("function '" + fn->name + "' has no body to evaluate at compile time");
        return std::nullopt;
    }

    auto* saved_namespace = ctx.symbols.get_current_namespace();
    ctx.symbols.set_current_namespace(function_namespace);
    auto result = call_function(*fn, std::move(args));
    ctx.symbols.set_current_namespace(saved_namespace);
    return result;
}

std::optional<Value> Evaluator::call_function(const ast::FuncStmt& fn,
                                              std::vector<Value> args) {
    if (++call_depth_ > kMaxCallDepth) {
        --call_depth_;
        fail("compile-time recursion too deep");
        return std::nullopt;
    }

    if (fn.args.size() != args.size()) {
        fail("argument count mismatch in compile-time call '" + fn.name + "'");
        --call_depth_;
        return std::nullopt;
    }
    const size_t saved_base = scope_base_;
    scope_base_ = scopes_.size();
    push_scope();
    for (size_t i = 0; i < fn.args.size(); ++i) {
        declare(fn.args[i].name, args[i]);
    }

    Frame frame;
    const bool ok = run_block(fn.body, frame);
    pop_scope();
    scope_base_ = saved_base;
    --call_depth_;

    if (!ok) return std::nullopt;

    if (frame.flow == Flow::Normal && fn.return_type &&
        fn.return_type->kind == ast::TypeKind::Void) return Value{};
    if (frame.flow == Flow::Returned) {
        return frame.return_value ? *frame.return_value : Value{};
    }
    fail("function '" + fn.name + "' did not return a value");
    return std::nullopt;
}

bool Evaluator::run_block(const ast::Block* block, Frame& frame) {
    if (!block) return true;
    push_scope();
    for (const auto* stmt : block->stmts) {
        if (!stmt || has_failed()) break;
        if (++steps_used_ > kMaxSteps) {
            fail("compile-time expression exceeded the step budget (non-terminating loop?)");
            break;
        }
        if (!run_stmt(stmt, frame)) {
            pop_scope();
            return false;
        }
        if (frame.flow != Flow::Normal) break;
    }
    pop_scope();
    return !has_failed();
}

bool Evaluator::run_stmt(const ast::Stmt* stmt, Frame& frame) {
    if (!stmt) return true;

    return std::visit(overloaded{
        [&](const ast::VarDecl& n) -> bool {
            if (!n.value) {
                declare(n.name, Value{});
                return true;
            }
            auto v = eval_expr(n.value);
            if (!v) return false;
            declare(n.name, *v);
            return true;
        },
        [&](const ast::ExprStmt& n) -> bool {
            return eval_expr(n.expr).has_value();
        },
        [&](const ast::ReturnStmt& n) -> bool {
            if (n.value) {
                auto v = eval_expr(n.value);
                if (!v) return false;
                frame.return_value = *v;
            }
            frame.flow = Flow::Returned;
            return true;
        },
        [&](const ast::BlockStmt& n) -> bool {
            return run_block(n.body, frame);
        },
        [&](const ast::IfStmt& n) -> bool {
            return run_if(n, frame);
        },
        [&](const ast::WhileStmt& n) -> bool {
            return run_while(n, frame);
        },
        [&](const ast::SwitchStmt& n) -> bool {
            return run_switch(n, frame);
        },
        [&](const ast::BreakStmt&) -> bool {
            frame.flow = Flow::Broke;
            return true;
        },
        [&](const ast::ContinueStmt&) -> bool {
            frame.flow = Flow::Continued;
            return true;
        },
        [&](const auto&) -> bool {
            fail("unsupported statement inside a compile-time function");
            return false;
        },
    }, stmt->kind);
}

bool Evaluator::run_if(const ast::IfStmt& node, Frame& frame) {
    auto cond = eval_expr(node.condition);
    if (!cond) return false;
    auto b = value_bool(*cond);
    if (!b) {
        fail("'if' condition is not a boolean in a compile-time expression");
        return false;
    }
    if (*b) return run_block(node.then_block, frame);

    const ast::ElseIfStmt* ei = node.else_if;
    while (ei) {
        auto c = eval_expr(ei->condition);
        if (!c) return false;
        auto cb = value_bool(*c);
        if (!cb) {
            fail("'else if' condition is not a boolean in a compile-time expression");
            return false;
        }
        if (*cb) return run_block(ei->then_block, frame);
        ei = ei->next;
    }

    return run_block(node.else_block, frame);
}

bool Evaluator::run_while(const ast::WhileStmt& node, Frame& frame) {
    while (true) {
        if (has_failed()) return false;
        if (++steps_used_ > kMaxSteps) {
            fail("compile-time loop exceeded the step budget (non-terminating loop?)");
            return false;
        }
        auto cond = eval_expr(node.condition);
        if (!cond) return false;
        auto b = value_bool(*cond);
        if (!b) {
            fail("'while' condition is not a boolean in a compile-time expression");
            return false;
        }
        if (!*b) break;

        if (!run_block(node.body, frame)) return false;

        if (frame.flow == Flow::Broke) {
            frame.flow = Flow::Normal;
            break;
        }
        if (frame.flow == Flow::Continued) {
            frame.flow = Flow::Normal;
        }
        if (frame.flow == Flow::Returned) return true;

        if (node.for_step && !eval_expr(node.for_step)) return false;
    }
    return true;
}

bool Evaluator::run_switch(const ast::SwitchStmt& node, Frame& frame) {
    auto cond = eval_expr(node.condition);
    if (!cond) return false;
    auto ci = value_int(*cond);
    if (!ci) {
        fail("'switch' condition must be an integer in a compile-time expression");
        return false;
    }

    for (const auto& cs : node.cases) {
        bool matched = false;
        for (const auto& cv : cs.const_values) {
            if (cv && *cv == *ci) {
                matched = true;
                break;
            }
        }
        if (!matched) continue;

        if (!run_block(cs.body, frame)) return false;
        if (frame.flow == Flow::Broke) frame.flow = Flow::Normal;
        return true;
    }

    if (node.default_block) {
        if (!run_block(node.default_block, frame)) return false;
        if (frame.flow == Flow::Broke) frame.flow = Flow::Normal;
    }
    return true;
}

bool Evaluator::evaluate_and_substitute(ast::Expr* expr) {
    if (!expr) return false;

    fail_reason_.clear();
    steps_used_ = 0;
    call_depth_ = 0;

    auto value = eval_expr(expr);
    if (!value || has_failed()) {
        ctx.errors.add(expr->loc,
            "cannot evaluate expression at compile time: " +
            (fail_reason_.empty() ? "invalid expression" : fail_reason_));
        return false;
    }
    return substitute_literal(expr, *value);
}

bool Evaluator::substitute_literal(ast::Expr* expr, const Value& value) {
    if (std::get_if<std::monostate>(&value.data)) {
        ctx.errors.add(expr->loc, "compile-time expression does not produce a value");
        return false;
    }

    const ast::Type* t = expr->resolved_type ? expr->resolved_type : value.type;
    if (!t) {
        ctx.errors.add(expr->loc, "compile-time expression has no resolved type");
        return false;
    }

    if (is_float_kind(t->kind)) {
        auto f = value_float(value);
        if (!f) {
            ctx.errors.add(expr->loc, "compile-time expression produced a non-float value");
            return false;
        }
        expr->kind = ast::FloatExpr{ *f };
    } else if (t->kind == ast::TypeKind::Bool) {
        auto b = value_bool(value);
        if (!b) {
            ctx.errors.add(expr->loc, "compile-time expression produced a non-boolean value");
            return false;
        }
        expr->kind = ast::BoolExpr{ *b };
    } else if (is_int_kind(t->kind)) {
        auto i = value_int(value);
        if (!i) {
            ctx.errors.add(expr->loc, "compile-time expression produced a non-integer value");
            return false;
        }
        expr->kind = ast::IntExpr{ *i };
    } else {
        ctx.errors.add(expr->loc,
            "compile-time expression has an unsupported result type: " + t->to_string(ctx));
        return false;
    }

    expr->resolved_type = t;
    expr->is_comptime = false;
    return true;
}

} // namespace quant::comptime
