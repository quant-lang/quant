#pragma once
#include <unordered_map>
#include <vector>
#include <string>
#include <array>
#include <optional>
#include "quant/frontend/ast.h"

using namespace quant::ast;

namespace quant::types {

struct GenericStructDef {
    std::vector<std::string> params;
    std::vector<ast::StructField> fields;
    // Method declarations of the generic struct, kept so concrete
    // instantiations can be monomorphized together with the fields.
    std::vector<ast::FuncStmt> methods;
    // Namespace in which the struct was declared (for resolving unqualified
    // references inside instantiated method bodies and for symbol layout).
    std::vector<std::string> module_namespace;
};

struct GenericFuncDef {
    std::vector<std::string> params;
    std::vector<ast::FuncArg> args;
    const ast::Type* return_type;
    const ast::Block* body = nullptr;
    std::vector<ast::Attribute> attributes;
    std::vector<std::string> module_namespace;
};

class TypeContext {
    public:
        const Type* get_builtin(TypeKind kind);
        const Type* get_struct(const std::string& name);
        const ast::Type* get_enum(const std::string& name);
        const Type* get_pointer(const Type* base) const;
        const Type* get_reference(const Type* base) const;
        const Type* get_generic_param(const std::string& name);
        const Type* get_generic_instantiation(const std::string& name, const std::vector<const Type*>& args);
        const Type* get_deferred_generic(const std::string& name, const std::vector<const Type*>& args) const;

        bool is_mangled_name(const std::string& mangled, std::string& out_base) const;
        bool try_instantiate(const std::string& mangled, const std::vector<const Type*>& type_args);

        void register_struct(const std::string& name,
            const std::vector<std::pair<std::string, const Type*>>& fields,
            const std::vector<std::vector<ast::Attribute>>& field_attrs = {});

        void register_generic_struct(const std::string& name, const GenericStructDef& def);

        void register_generic_func(const std::string& name, const GenericFuncDef& def);
        const GenericFuncDef* get_generic_func(const std::string& name) const;
        const std::unordered_map<std::string, GenericFuncDef>& get_all_generic_funcs() const { return generic_func_defs; }
        const GenericStructDef* get_generic_struct(const std::string& name) const;

        const Type* substitute_type(const Type* type, const std::unordered_map<std::string, const Type*>& subst) const;
        ast::FuncArg substitute_func_arg(const ast::FuncArg& arg, const std::unordered_map<std::string, const Type*>& subst) const;
        std::string mangle_func_name(const std::string& name, const std::vector<const Type*>& args) const;

        const std::vector<std::pair<std::string, const Type*>>* get_struct_fields(const std::string& name) const;

        const std::vector<std::vector<ast::Attribute>>* get_struct_field_attrs(const std::string& name) const;

        int get_field_index(const std::string& struct_name, const std::string& field) const;

        const Type* get_field_type(const std::string& struct_name, const std::string& field) const;

        const ast::Expr* get_field_default_value(const std::string& struct_name, const std::string& field_name) const;

        int type_size(const Type* t) const;

        TypeContext();

    private:
        std::array<Type, (size_t)TypeKind::Count> builtin_types;
        mutable std::unordered_map<std::string, Type> struct_types;
        mutable std::unordered_map<std::string, Type> enum_types;
        mutable std::unordered_map<const Type*, Type> pointer_cache;
        mutable std::unordered_map<const Type*, Type> reference_cache;
        std::unordered_map<std::string, Type> generic_param_types;

        std::unordered_map<
            std::string,
            std::vector<std::pair<std::string, const Type*>>
        > structs;

        std::unordered_map<std::string, std::vector<std::vector<ast::Attribute>>> struct_field_attrs_map;

        std::unordered_map<std::string, GenericStructDef> generic_defs;
        std::unordered_map<std::string, GenericFuncDef> generic_func_defs;
        mutable std::unordered_map<std::string, std::string> mangled_to_base;
    };

}
