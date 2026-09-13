#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "quant/frontend/ast.h"
#include "quant/support/compiler_context.h"

namespace quant::comptime {

// Scalar value; monostate represents void or an uninitialized local.
struct Value {
    const ast::Type* type = nullptr;
    std::variant<std::monostate, int64_t, double> data;
};

// Interprets '#' expressions during semantic analysis and replaces them with literals.
class Evaluator {
public:
    explicit Evaluator(CompilerContext& ctx) : ctx(ctx) {}

    // Evaluate a '#'-marked expression and replace `expr->kind` with the
    // resulting literal (preserving `resolved_type`). On failure adds an
    // error at `expr->loc` and returns false.
    bool evaluate_and_substitute(ast::Expr* expr);

private:
    CompilerContext& ctx;

    std::vector<std::unordered_map<std::string, Value>> scopes_;
    size_t scope_base_ = 0; // The current call cannot access its caller's locals.

    // Safety budgets against non-terminating compile-time programs.
    static constexpr int64_t kMaxSteps = 50'000'000;
    static constexpr int32_t kMaxCallDepth = 128;
    int64_t steps_used_ = 0;
    int32_t call_depth_ = 0;

    // First failure reason, reported once by evaluate_and_substitute.
    std::string fail_reason_;

    void fail(const std::string& reason);
    bool has_failed() const { return !fail_reason_.empty(); }

    void push_scope();
    void pop_scope();
    void declare(const std::string& name, const Value& value);
    Value* lookup_env(const std::string& name);
    symb_t::Symbol* resolve_symbol_path(const std::vector<std::string>& path,
                                          symb_t::Namespace** owner = nullptr);

    std::optional<Value> eval_expr(const ast::Expr* expr);
    std::optional<Value> eval_binary(const ast::BinaryExpr& node);
    std::optional<Value> eval_unary(const ast::UnaryExpr& node);
    std::optional<Value> eval_cast(const ast::CastExpr& node);
    std::optional<Value> eval_call(const ast::CallExpr& node);
    std::optional<Value> call_function(const ast::FuncStmt& fn,
                                       std::vector<Value> args);
    std::optional<Value> eval_var(const ast::VarExpr& node);
    std::optional<Value> eval_namespace(const ast::NamespaceExpr& node);

    // Statement-level interpretation of a called function's body.
    enum class Flow { Normal, Returned, Broke, Continued };
    struct Frame {
        Flow flow = Flow::Normal;
        std::optional<Value> return_value;
    };
    bool run_block(const ast::Block* block, Frame& frame);
    bool run_stmt(const ast::Stmt* stmt, Frame& frame);
    bool run_if(const ast::IfStmt& node, Frame& frame);
    bool run_while(const ast::WhileStmt& node, Frame& frame);
    bool run_switch(const ast::SwitchStmt& node, Frame& frame);

    static std::optional<int64_t> value_int(const Value& v);
    static std::optional<double> value_float(const Value& v);
    static std::optional<bool> value_bool(const Value& v);

    // Substitutes a completed evaluation result into the AST node.
    bool substitute_literal(ast::Expr* expr, const Value& value);
};

} // namespace quant::comptime
