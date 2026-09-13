#include "quant/semantic/symbol_table.h"
#include "utils/logger.h"
#include "quant/support/compiler_context.h"


#include <type_traits>
#include <utility>

using namespace utils::logger;

namespace quant::symb_t {
    SymbolTable::SymbolTable(memory::Arena& a) : arena(a){
        global_namespace = memory::make<Namespace>(arena);

        global_namespace->name = "::";

        current_namespace = global_namespace;
    }

    void SymbolTable::enter_scope() {
        scopes.emplace_back();
    }

    void SymbolTable::exit_scope() {
        if (scopes.empty()) {
            error("Trying to exit empty scope stack");
            return;
        }

        scopes.pop_back();
    }

    bool SymbolTable::enter_namespace(const std::string& name) {
        auto it = current_namespace->children.find(name);

        if (it == current_namespace->children.end()) {
            auto ns = memory::make<Namespace>(arena);
            ns->name = name;
            ns->parent = current_namespace;

            current_namespace->children.emplace(name, std::move(ns));
        }

        current_namespace = current_namespace->children[name];
        return true;
    }

    void SymbolTable::exit_namespace() {
        if (current_namespace == nullptr || current_namespace->parent == nullptr) {
            error("Cannot exit global namespace");
            return;
        }

        current_namespace = current_namespace->parent;
    }

    Namespace* SymbolTable::resolve_namespace(const std::vector<std::string>& path) {
        Namespace* current = global_namespace;

        for (const auto& part : path) {
            auto it = current->children.find(part);
            if (it == current->children.end()) {
                return nullptr;
            }

            current = it->second;
        }

        return current;
    }

    void SymbolTable::set_current_module_ns(const std::vector<std::string>& ns) {
        current_module_ns = ns;
    }

    const std::vector<std::string>& SymbolTable::get_current_module_ns() const {
        return current_module_ns;
    }

    bool SymbolTable::declare_symbol(const std::string& name, Symbol symbol, bool preserve_owning_module) {
        if (!preserve_owning_module) {
            symbol.owning_module = current_module_ns;
        }

        if (!scopes.empty()) {
            auto& current = scopes.back();

            if (current.contains(name)) {
                return false;
            }
            current.emplace(name, std::move(symbol));
            return true;
        }

        auto& current = current_namespace->symbols;

        if (current.contains(name)) {
            return false;
        }

        auto* sym = memory::make<Symbol>(arena, std::move(symbol));

        current.emplace(name, sym);
        return true;
    }

    bool SymbolTable::declare(const ast::VarDecl& decl) {
        return declare_symbol(decl.name, Symbol{
            decl.name,
            VarSymbol{
                decl.type,
                decl.is_mut,
                decl.value != nullptr
            },
            decl.attributes
        });
    }

    bool SymbolTable::declare(const ast::FuncArg& arg) {
        return declare_symbol(arg.name, Symbol{
            arg.name,
            FuncArgSymbol{
                arg.type,
                arg.is_mut
            },
            {}
        });
    }
    bool SymbolTable::declare(const ast::FuncStmt& fn) {
        FuncSymbol sym;

        sym.return_type = fn.return_type;
        sym.is_extern = fn.is_extern;
        sym.is_defined = fn.body != nullptr;
        sym.is_entry = fn.is_entry;
        sym.func_decl = fn.body ? &fn : nullptr;

        sym.arg_types.reserve(fn.args.size());

        for (const auto& arg : fn.args) {
            sym.arg_types.push_back(arg.type);
        }

        auto* existing = lookup(fn.name);

        if (existing) {
            auto* fs = std::get_if<FuncSymbol>(&existing->data);

            if (!fs) {
                return false;
            }

            // signature check
            if (fs->arg_types.size() != sym.arg_types.size()) {
                return false;
            }

            for (size_t i = 0; i < fs->arg_types.size(); ++i) {
                if (fs->arg_types[i] != sym.arg_types[i]) {
                    return false;
                }
            }

            if (fs->return_type != sym.return_type) {
                return false;
            }

            // duplicate definition
            if (fs->is_defined && sym.is_defined) {
                error("Function already defined: " + fn.name);
                return false;
            }

            // upgrade forward decl -> definition
            if (!fs->is_defined && sym.is_defined) {
                fs->is_defined = true;
                fs->func_decl = &fn;
                existing->attributes = fn.attributes;
            }

            return true;
        }

        return declare_symbol(fn.name, Symbol{
            fn.name,
            sym,
            fn.attributes
        });
    }

    bool SymbolTable::declare(const ast::StructDecl& str) {
        StructSymbol sym;

        sym.field_names.reserve(str.fields.size());
        sym.field_types.reserve(str.fields.size());
        sym.field_attributes.reserve(str.fields.size());
        sym.method_names.reserve(str.fields.size());

        for (const auto& value : str.fields) {
            if (const auto* field = std::get_if<ast::StructField>(&value)) {
                sym.field_names.push_back(field->name);
                sym.field_types.push_back(field->type);
                sym.field_attributes.push_back(field->attributes);
            } else if (const auto* fn = std::get_if<ast::FuncStmt>(&value)) {
                sym.method_names.push_back(fn->name);
            }
        }

        if (!declare_symbol(str.name, Symbol{
            str.name,
            sym,
            str.attributes
        })) {
            return false;
        }

        // Bind the struct's methods: register each one as a function under the
        // qualified name "<struct>::<method>" so it stays resolvable without
        // colliding with free functions of the same name.
        for (const auto& value : str.fields) {
            const auto* fn = std::get_if<ast::FuncStmt>(&value);
            if (!fn) continue;

            if (!declare_method(str.name, *fn)) {
                std::string msg = "Method redeclaration: " + str.name + "::" + fn->name;
                error(msg);
                if (error_bag) {
                    error_bag->add(msg);
                }
            }
        }

        return true;
    }

    bool SymbolTable::declare_method(const std::string& struct_name, const ast::FuncStmt& fn) {
        FuncSymbol sym;

        sym.return_type = fn.return_type;
        sym.is_extern = fn.is_extern;
        sym.is_defined = fn.body != nullptr;
        sym.is_entry = fn.is_entry;
        sym.func_decl = fn.body ? &fn : nullptr;

        sym.arg_types.reserve(fn.args.size());

        for (const auto& arg : fn.args) {
            sym.arg_types.push_back(arg.type);
        }

        std::string qualified = struct_name + "::" + fn.name;

        return declare_symbol(qualified, Symbol{
            qualified,
            sym,
            fn.attributes
        });
    }

    bool SymbolTable::declare_struct(
        const std::string& name,
        const std::vector<std::pair<std::string, const ast::Type*>>& fields,
        const std::vector<ast::Attribute>& attrs,
        const std::vector<std::vector<ast::Attribute>>& field_attrs
    ) {
        StructSymbol sym;
        sym.field_names.reserve(fields.size());
        sym.field_types.reserve(fields.size());
        sym.field_attributes = field_attrs;
        for (const auto& [fname, ftype] : fields) {
            sym.field_names.push_back(fname);
            sym.field_types.push_back(ftype);
        }
        return declare_symbol(name, Symbol{name, sym, attrs});
    }

    bool SymbolTable::declare_struct_global(
        const std::string& name,
        const std::vector<std::pair<std::string, const ast::Type*>>& fields,
        const std::vector<ast::Attribute>& attrs,
        const std::vector<std::vector<ast::Attribute>>& field_attrs
    ) {
        StructSymbol sym;
        sym.field_names.reserve(fields.size());
        sym.field_types.reserve(fields.size());
        sym.field_attributes = field_attrs;
        for (const auto& [fname, ftype] : fields) {
            sym.field_names.push_back(fname);
            sym.field_types.push_back(ftype);
        }

        // Walk to root namespace to ensure lookup from any scope can find it
        Namespace* saved = current_namespace;
        while (current_namespace->parent) {
            current_namespace = current_namespace->parent;
        }
        Symbol symbol{name, std::move(sym), attrs};
        symbol.owning_module = current_module_ns;
        auto& current = current_namespace->symbols;
        if (current.contains(name)) {
            current_namespace = saved;
            return false;
        }
        auto* sym_ptr = memory::make<Symbol>(arena, std::move(symbol));
        current.emplace(name, sym_ptr);
        current_namespace = saved;
        return true;
    }

    bool SymbolTable::declare_struct_in_ns(
        const std::vector<std::string>& ns_path,
        const std::string& name,
        const std::vector<std::pair<std::string, const ast::Type*>>& fields,
        const std::vector<ast::Attribute>& attrs,
        const std::vector<std::vector<ast::Attribute>>& field_attrs
    ) {
        StructSymbol sym;
        sym.field_names.reserve(fields.size());
        sym.field_types.reserve(fields.size());
        sym.field_attributes = field_attrs;
        for (const auto& [fname, ftype] : fields) {
            sym.field_names.push_back(fname);
            sym.field_types.push_back(ftype);
        }

        Namespace* saved = current_namespace;
        current_namespace = global_namespace;
        for (const auto& part : ns_path) {
            auto it = current_namespace->children.find(part);
            if (it == current_namespace->children.end()) {
                current_namespace = saved;
                return false;
            }
            current_namespace = it->second;
        }
        Symbol symbol{name, std::move(sym), attrs};
        symbol.owning_module = current_module_ns;
        auto& current = current_namespace->symbols;
        if (current.contains(name)) {
            current_namespace = saved;
            return false;
        }
        auto* sym_ptr = memory::make<Symbol>(arena, std::move(symbol));
        current.emplace(name, sym_ptr);
        current_namespace = saved;
        return true;
    }

    Symbol* SymbolTable::lookup(const std::string& name) {
        for (int i = static_cast<int>(scopes.size()) - 1; i >= 0; --i) {
            auto it = scopes[i].find(name);
            if (it != scopes[i].end()) {
                return &it->second;
            }
        }

        Namespace* ns = current_namespace;

        while (ns != nullptr) {
            auto it = ns->symbols.find(name);
            if (it != ns->symbols.end()) {
                return it->second;
            }

            ns = ns->parent;
        }

        return nullptr;
    }

    Symbol* SymbolTable::lookup_qualified(const std::vector<std::string>& path) {
        if (path.empty()) {
            return nullptr;
        }

        Namespace* current = global_namespace;

        for (size_t i = 0; i + 1 < path.size(); ++i) {
            auto it = current->children.find(path[i]);
            if (it == current->children.end()) {
                return nullptr;
            }

            current = it->second;
        }

        auto it = current->symbols.find(path.back());
        if (it == current->symbols.end()) {
            return nullptr;
        }

        return it->second;
    }
    Symbol* SymbolTable::lookup_current_namespace(const std::string& name) {
        auto it = current_namespace->symbols.find(name);

        if (it == current_namespace->symbols.end()) {
            return nullptr;
        }

        return it->second;
    }

    bool SymbolTable::declare_symbol_in_namespace(const std::vector<std::string>& ns_path,
                                                   const std::string& name, Symbol symbol) {
        symbol.owning_module = current_module_ns;

        Namespace* ns = create_namespace_path(ns_path);

        if (ns->symbols.contains(name)) {
            return false;
        }

        auto* sym = memory::make<Symbol>(arena, std::move(symbol));
        ns->symbols.emplace(name, sym);
        return true;
    }

    void SymbolTable::mark_initialized(const std::string& name) {
        auto* sym = lookup(name);

        if (!sym) {
            error("There is no declared variable: " + name);
            return;
        }

        std::visit([&](auto& s) {
            using T = std::decay_t<decltype(s)>;

            if constexpr (std::is_same_v<T, VarSymbol>) {
                s.is_initialized = true;
            } else {
                error("Symbol is not a variable: " + name);
            }
        }, sym->data);
    }
    Namespace* SymbolTable::create_namespace_path(const std::vector<std::string>& path) {
        Namespace* current = global_namespace;

        for (const auto& part : path) {
            auto it = current->children.find(part);

            if (it == current->children.end()) {
                auto* ns = memory::make<Namespace>(arena);

                ns->name = part;
                ns->parent = current;

                current->children.emplace(part, ns);

                current = ns;
            } else {
                current = it->second;
            }
        }

        return current;
    }

}
