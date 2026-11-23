#pragma once

#include <string>
#include <vector>

#include "quant/frontend/ast.h"
#include "quant/support/compiler_context.h"
#include "quant/modules/module.h"

namespace quant::attrs {

class AttrEvaluator {
public:
    explicit AttrEvaluator(CompilerContext& ctx, std::vector<std::string> ns = {});

    // Lower runtime attributes in the given module AST.
    // Currently handles @init by transforming declarations/fields without
    // an initializer into an explicit zero initialization:
    //   @init T var;  <=>  T var = 0 as/as! T;
    // The cast kind is chosen based on a size check.
    void evaluate(const std::vector<ast::Stmt*>& stmts);

private:
    CompilerContext& ctx;
    std::vector<std::string> module_namespace;

    void visit_stmt(ast::Stmt* stmt);
    void visit_block(ast::Block* block);

    void lower_var_decl(ast::VarDecl& var, const SourceLocation& loc);
    void lower_struct_decl(ast::StructDecl& str);

    ast::Expr* make_zero_expr(const ast::Type* type, const SourceLocation& loc);
    static bool has_init_attr(const std::vector<ast::Attribute>& attrs);
    static void remove_init_attr(std::vector<ast::Attribute>& attrs);
};

// Insert @onbegin calls at the start of main and @onend calls in reverse
// order before the final return or at the end. Runs after semantic analysis.
void lower_dynamics(CompilerContext& ctx, modules::Module& mod);

} // namespace quant::attrs
