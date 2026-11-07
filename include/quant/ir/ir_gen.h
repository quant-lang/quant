#pragma once

#include "qu-ir/ir.h"
#include "quant/frontend/ast.h"
#include "quant/support/compiler_context.h"
#include "quant/modules/module.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <string>
#include <span>

namespace quant::codegen {

struct IRGenerator {
    CompilerContext& ctx;

    IRProgram program;

    IRFunction* current_func = nullptr;
    const ast::Type* current_func_return_type = nullptr;
    const modules::Module* current_module = nullptr;

    // Struct name of the method currently being generated (empty for free
    // functions). Used to resolve bare field names to the receiver.
    std::string current_method_struct_name;

    Reg next_reg = 0;
    Local next_local = 0;
    Label next_label = 0;

    bool current_terminated = false;

    // break/continue jump targets.
    // break_labels is pushed by while and switch;
    // continue_labels is pushed only by while.
    std::vector<Label> break_labels;
    std::vector<Label> continue_labels;

    // function name -> id
    std::unordered_map<std::string, uint32_t> function_ids;

    // global variable name -> id
    std::unordered_map<std::string, uint32_t> global_ids;

    // variable scopes
    std::vector<std::unordered_map<std::string, Local>> local_scopes;

    // variable type scopes
    std::vector<std::unordered_map<std::string, const ast::Type*>> type_scopes;

    // local variable attributes (for runtime attrs)
    std::unordered_map<std::string, std::vector<ast::Attribute>> local_var_attrs;

    // namespace nesting
    std::vector<std::string> namespace_stack;

    // module namespace of the generic instantiation being generated (if any)
    std::vector<std::string> generic_module_ns;

    // substitution map of the generic instantiation being generated (if any),
    // used to resolve nested generic calls whose type args are still the
    // generic params (e.g. opt_none<T> inside slice_at<i32>).
    const std::unordered_map<std::string, const ast::Type*>* current_type_subst = nullptr;

    struct RegionInfo {
        Local region_local;  // local slot holding pointer to Region struct
    };
    std::vector<RegionInfo> region_stack;

    explicit IRGenerator(CompilerContext& c);

    // Entry

    void gen_program(std::span<quant::modules::Module* const> modules);
    void gen_module(const quant::modules::Module& mod);
    // Functions

    void gen_function(const ast::FuncStmt& fn);

    // For runtime attributes. (Now there is no runtime attributes)
    void emit_attr_lowering(const std::string& var_name);
    void emit_attr_lowering(const std::string& var_name, const std::vector<ast::Attribute>& attrs);

    // Statements

    void gen_stmt(const ast::Stmt& stmt);
    void gen_block(const ast::Block& block);
    void gen_region(const ast::RegionStmt& reg);

    // Expressions

    Reg gen_expr(const ast::Expr& expr);

    // Generate an expression and emit an implicit widening cast to `target`
    // when the value's type differs numerically from the destination type.
    Reg gen_expr_as(const ast::Expr& expr, const ast::Type* target);

private:

    // Helpers

    Reg new_reg();
    Local new_local();
    Label new_label();

    void emit(const IRInst& inst);

    IRBinaryOp map_op(ast::BinaryOp op);
};

} // namespace quant::codegen
