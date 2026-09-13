#pragma once

#include <unordered_map>
#include <variant>
#include <vector>
#include <string>

#include "quant/frontend/ast.h"
#include "quant/support/alloc.h"
#include "utils/errors.h"

namespace quant::symb_t {

    struct VarSymbol {
        const ast::Type* type;
        bool is_mut;
        bool is_initialized;
        std::optional<int64_t> const_value;
        ast::Expr* guard_cond = nullptr;
        // True if this pointer may be null at runtime.
        bool may_be_null = false;
    };

    struct FuncArgSymbol {
        const ast::Type* type;
        bool is_mut;
    };
    struct FuncSymbol {
        std::vector<const ast::Type*> arg_types;
        const ast::Type* return_type;

        bool is_extern;
        bool is_defined;
        bool is_entry;

        // AST declaration of the function (used by the compile-time
        // evaluator to interpret its body). nullptr for extern/forward
        // declarations without a body. Forward declarations are upgraded to
        // point at the definition when one is seen.
        const ast::FuncStmt* func_decl = nullptr;
    };
    struct StructSymbol {
        std::vector<std::string> field_names;
        std::vector<const ast::Type*> field_types;
        std::vector<std::vector<ast::Attribute>> field_attributes;
        std::vector<std::string> method_names;
    };
    struct EnumSymbol {
        std::vector<std::string> variant_names;
    };
    using SymbolData = std::variant<
        VarSymbol,
        FuncArgSymbol,
        FuncSymbol,
        StructSymbol,
        EnumSymbol
    >;

    struct Symbol {
        std::string name;
        SymbolData data;
        std::vector<ast::Attribute> attributes;
        std::vector<std::string> owning_module;
    };

    struct Namespace {
        std::string name;
        Namespace* parent = nullptr;

        std::unordered_map<std::string, Symbol*> symbols;
        std::unordered_map<std::string, Namespace*> children;
    };
    class SymbolTable {
    public:
        SymbolTable(memory::Arena& a);

        void enter_scope();
        void exit_scope();

        bool enter_namespace(const std::string& name);
        void exit_namespace();

        Namespace* resolve_namespace(const std::vector<std::string>& path);

        bool declare(const ast::VarDecl& decl);
        bool declare(const ast::FuncArg& arg);
        bool declare(const ast::FuncStmt& fn);
        bool declare(const ast::StructDecl& str);
        bool declare_method(const std::string& struct_name, const ast::FuncStmt& fn);
        bool declare_struct(const std::string& name,
            const std::vector<std::pair<std::string, const ast::Type*>>& fields,
            const std::vector<ast::Attribute>& attrs = {},
            const std::vector<std::vector<ast::Attribute>>& field_attrs = {});
        bool declare_struct_global(const std::string& name,
            const std::vector<std::pair<std::string, const ast::Type*>>& fields,
            const std::vector<ast::Attribute>& attrs = {},
            const std::vector<std::vector<ast::Attribute>>& field_attrs = {});
        bool declare_struct_in_ns(const std::vector<std::string>& ns_path,
            const std::string& name,
            const std::vector<std::pair<std::string, const ast::Type*>>& fields,
            const std::vector<ast::Attribute>& attrs = {},
            const std::vector<std::vector<ast::Attribute>>& field_attrs = {});
        bool declare(const ast::RegionStmt& reg);

        bool declare_symbol(const std::string& name, Symbol symbol, bool preserve_owning_module = false);
        // Declare a symbol directly in the given namespace (created if needed),
        // bypassing any active lexical scopes. Used for lazily instantiated
        // generic struct methods so they stay resolvable from any call site.
        bool declare_symbol_in_namespace(const std::vector<std::string>& ns_path,
                                         const std::string& name, Symbol symbol);

        Symbol* lookup(const std::string& name);
        Symbol* lookup_qualified(const std::vector<std::string>& path);
        Symbol* lookup_current_namespace(const std::string& name);
        Namespace* get_current_namespace() const { return current_namespace; }
        void set_current_namespace(Namespace* ns) { current_namespace = ns; }

        void mark_initialized(const std::string& name);
        Namespace* create_namespace_path(const std::vector<std::string>& path);

        void set_current_module_ns(const std::vector<std::string>& ns);
        const std::vector<std::string>& get_current_module_ns() const;

        // Error sink used to report declaration-level errors (e.g. duplicate
        // methods); the compiler's ErrorBag counts them and aborts compilation.
        void set_error_bag(ErrorBag* bag) { error_bag = bag; }

    private:
        memory::Arena& arena;
        ErrorBag* error_bag = nullptr;
        Namespace* global_namespace;
        Namespace* current_namespace = nullptr;

        std::vector<std::unordered_map<std::string, Symbol>> scopes;
        std::vector<std::string> current_module_ns;
    };

}
