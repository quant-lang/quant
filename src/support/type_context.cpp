#include "quant/support/type_context.h"

namespace quant::types {

namespace {
    std::string mangle_name(const std::string& base, const std::vector<const Type*>& args) {
        std::string out = base;
        for (const auto* t : args) {
            out += "$";
            if (t->kind == TypeKind::Struct) {
                out += t->struct_name;
            } else if (t->kind == TypeKind::Generic) {
                out += t->struct_name;
            } else {
                out += std::to_string((int)t->kind);
            }
        }
        return out;
    }
}

    TypeContext::TypeContext() {
        builtin_types[(size_t)TypeKind::Void].kind = TypeKind::Void;
        builtin_types[(size_t)TypeKind::Bool].kind = TypeKind::Bool;

        builtin_types[(size_t)TypeKind::I8].kind  = TypeKind::I8;
        builtin_types[(size_t)TypeKind::I16].kind = TypeKind::I16;
        builtin_types[(size_t)TypeKind::I32].kind = TypeKind::I32;
        builtin_types[(size_t)TypeKind::I64].kind = TypeKind::I64;

        builtin_types[(size_t)TypeKind::U8].kind  = TypeKind::U8;
        builtin_types[(size_t)TypeKind::U16].kind = TypeKind::U16;
        builtin_types[(size_t)TypeKind::U32].kind = TypeKind::U32;
        builtin_types[(size_t)TypeKind::U64].kind = TypeKind::U64;

        builtin_types[(size_t)TypeKind::F32].kind = TypeKind::F32;
        builtin_types[(size_t)TypeKind::F64].kind = TypeKind::F64;

        builtin_types[(size_t)TypeKind::String].kind = TypeKind::String;
        builtin_types[(size_t)TypeKind::NullPtr].kind = TypeKind::NullPtr;
    }

    const Type* TypeContext::get_builtin(TypeKind kind) {
        return &builtin_types[(int)kind];
    }

    const Type* TypeContext::get_struct(const std::string& name) {
        auto it = struct_types.find(name);
        if (it != struct_types.end())
            return &it->second;

        Type t;
        t.kind = TypeKind::Struct;
        t.struct_name = name;

        auto [iter, _] = struct_types.emplace(name, std::move(t));
        return &iter->second;
    }

    const Type* TypeContext::get_generic_param(const std::string& name) {
        auto it = generic_param_types.find(name);
        if (it != generic_param_types.end())
            return &it->second;

        Type t;
        t.kind = TypeKind::Generic;
        t.struct_name = name;

        auto [iter, _] = generic_param_types.emplace(name, std::move(t));
        return &iter->second;
    }

    const Type* TypeContext::get_generic_instantiation(
        const std::string& name,
        const std::vector<const Type*>& args
    ) {
        auto def_it = generic_defs.find(name);
        if (def_it == generic_defs.end()) {
            return get_struct(name); // fallback — not a generic
        }

        std::string mangled = mangle_name(name, args);

        // Already registered with concrete fields? Skip.
        if (structs.find(mangled) != structs.end()) {
            auto existing = struct_types.find(mangled);
            if (existing != struct_types.end())
                return &existing->second;
        }

        // Build a substitution map: param_name → concrete type
        std::unordered_map<std::string, const Type*> subst;
        for (size_t i = 0; i < def_it->second.params.size() && i < args.size(); ++i) {
            subst[def_it->second.params[i]] = args[i];
        }

        // Create concrete fields by substituting type params
        std::vector<std::pair<std::string, const Type*>> concrete_fields;
        std::vector<std::vector<ast::Attribute>> concrete_field_attrs;
        for (const auto& f : def_it->second.fields) {
            const Type* field_type = substitute_type(f.type, subst);
            concrete_fields.emplace_back(f.name, field_type);
            concrete_field_attrs.push_back(f.attributes);
        }

        register_struct(mangled, concrete_fields, concrete_field_attrs);
        mangled_to_base[mangled] = name;

        return get_struct(mangled);
    }

    const Type* TypeContext::get_deferred_generic(const std::string& name, const std::vector<const Type*>& args) const {
        std::string mangled = mangle_name(name, args);
        auto existing = struct_types.find(mangled);
        if (existing != struct_types.end()) {
            // The same mangled name can be requested twice with different
            // binding of type params: the return type of a generic function
            // is parsed before `<T>` is known, so its args may be unbound
            // Struct types (e.g. Struct "T"). A later request carrying the
            // properly bound params must win, otherwise the cached entry
            // keeps unbound type args and generic substitution fails.
            if (existing->second.type_args != args) {
                existing->second.type_args = args;
            }
            return &existing->second;
        }

        Type t;
        t.kind = TypeKind::Struct;
        t.struct_name = mangled;
        t.type_args = args;

        auto [iter, _] = struct_types.emplace(mangled, std::move(t));
        mangled_to_base[mangled] = name;
        return &iter->second;
    }

    bool TypeContext::try_instantiate(const std::string& mangled, const std::vector<const Type*>& type_args) {
        // Check if this mangled name was already instantiated
        if (structs.find(mangled) != structs.end())
            return true;

        // Iterate all generic defs to find the matching base name
        for (const auto& [base_name, def] : generic_defs) {
            std::string candidate = mangle_name(base_name, type_args);
            if (candidate == mangled) {
                // Found the matching generic — instantiate
                get_generic_instantiation(base_name, type_args);
                return true;
            }
        }
        return false;
    }

    bool TypeContext::is_mangled_name(const std::string& mangled, std::string& out_base) const {
        auto it = mangled_to_base.find(mangled);
        if (it != mangled_to_base.end()) {
            out_base = it->second;
            return true;
        }
        return false;
    }

    void TypeContext::register_struct(
        const std::string& name,
        const std::vector<std::pair<std::string, const Type*>>& fields,
        const std::vector<std::vector<ast::Attribute>>& field_attrs
    ) {
        structs[name] = fields;
        if (!field_attrs.empty()) {
            struct_field_attrs_map[name] = field_attrs;
        }
    }

    void TypeContext::register_generic_struct(const std::string& name, const GenericStructDef& def) {
        generic_defs[name] = def;
    }

    void TypeContext::register_generic_func(const std::string& name, const GenericFuncDef& def) {
        generic_func_defs[name] = def;
    }

    const GenericFuncDef* TypeContext::get_generic_func(const std::string& name) const {
        auto it = generic_func_defs.find(name);
        if (it == generic_func_defs.end())
            return nullptr;
        return &it->second;
    }

    const GenericStructDef* TypeContext::get_generic_struct(const std::string& name) const {
        auto it = generic_defs.find(name);
        if (it == generic_defs.end())
            return nullptr;
        return &it->second;
    }

    std::string TypeContext::mangle_func_name(const std::string& name, const std::vector<const Type*>& args) const {
        return mangle_name(name, args);
    }

    ast::FuncArg TypeContext::substitute_func_arg(const ast::FuncArg& arg, const std::unordered_map<std::string, const Type*>& subst) const {
        ast::FuncArg result;
        result.name = arg.name;
        result.is_mut = arg.is_mut;
        result.type = substitute_type(arg.type, subst);
        return result;
    }

    const Type* TypeContext::substitute_type(const Type* type, const std::unordered_map<std::string, const Type*>& subst) const {
        if (!type) return nullptr;
        if (type->kind == TypeKind::Generic) {
            auto it = subst.find(type->struct_name);
            if (it != subst.end())
                return it->second;
            return type;
        }
        if (type->kind == TypeKind::Pointer && type->pointed) {
            const Type* new_pointed = substitute_type(type->pointed, subst);
            if (new_pointed != type->pointed)
                return get_pointer(new_pointed);
            return type;
        }
        if (type->kind == TypeKind::Reference && type->pointed) {
            const Type* new_pointed = substitute_type(type->pointed, subst);
            if (new_pointed != type->pointed)
                return get_reference(new_pointed);
            return type;
        }
        if (type->kind == TypeKind::Struct && !type->type_args.empty()) {
            std::vector<const Type*> new_args;
            bool changed = false;
            for (const auto* arg : type->type_args) {
                const Type* new_arg = substitute_type(arg, subst);
                new_args.push_back(new_arg);
                if (new_arg != arg) changed = true;
            }
            if (changed) {
                std::string base_name = type->struct_name;
                std::string unmangled;
                if (is_mangled_name(base_name, unmangled)) {
                    base_name = unmangled;
                }
                return get_deferred_generic(base_name, new_args);
            }
        }
        return type;
    }

    const std::vector<std::vector<ast::Attribute>>* TypeContext::get_struct_field_attrs(const std::string& name) const {
        auto it = struct_field_attrs_map.find(name);
        if (it == struct_field_attrs_map.end())
            return nullptr;
        return &it->second;
    }

    const std::vector<std::pair<std::string, const Type*>>* TypeContext::get_struct_fields(const std::string& name) const{
        auto it = structs.find(name);
        if (it == structs.end())
            return nullptr;
        return &it->second;
    }

    int TypeContext::get_field_index(const std::string& struct_name, const std::string& field) const {
        auto fields = get_struct_fields(struct_name);
        if (!fields) return -1;

        for (int i = 0; i < (int)fields->size(); i++) {
            if ((*fields)[i].first == field)
                return i;
        }
        return -1;
    }

    const Type* TypeContext::get_field_type(const std::string& struct_name, const std::string& field) const {
        auto fields = get_struct_fields(struct_name);
        if (!fields) return nullptr;

        for (auto& [name, type] : *fields) {
            if (name == field)
                return type;
        }
        return nullptr;
    }

    int TypeContext::type_size(const Type* t) const {
        if (!t) return 0;
        switch (t->kind) {
            case TypeKind::Bool:
            case TypeKind::I8:
            case TypeKind::U8:   return 1;
            case TypeKind::I16:
            case TypeKind::U16:  return 2;
            case TypeKind::F32:
            case TypeKind::I32:
            case TypeKind::U32:  return 4;
            case TypeKind::F64:
            case TypeKind::I64:
            case TypeKind::U64:  return 8;
            case TypeKind::Pointer:
            case TypeKind::Reference:
            case TypeKind::NullPtr: return 8;
            case TypeKind::Struct: {
                auto it = structs.find(t->struct_name);
                if (it != structs.end()) {
                    int total = 0;
                    for (size_t i = 0; i < it->second.size(); ++i) {
                        const Type* ft = it->second[i].second;
                        if (ft && ft->kind == TypeKind::Struct) {
                            total += type_size(ft);
                        } else {
                            total += 8;
                        }
                    }
                    return total;
                }
                return 0;
            }
            default: return 0;
        }
    }

    const ast::Expr* TypeContext::get_field_default_value(const std::string& struct_name, const std::string& field_name) const {
        // Check generic struct definitions (they store the original AST with default values)
        for (const auto& [name, def] : generic_defs) {
            if (name == struct_name) {
                for (const auto& f : def.fields) {
                    if (f.name == field_name) {
                        return f.default_value;
                    }
                }
                return nullptr;
            }
        }
        // Non-generic structs: default values aren't stored in TypeContext
        return nullptr;
    }

    const Type* TypeContext::get_pointer(const Type* base) const {
        auto it = pointer_cache.find(base);
        if (it != pointer_cache.end())
            return &it->second;

        Type t;
        t.kind = TypeKind::Pointer;
        t.pointed = base;

        auto [iter, _] = pointer_cache.emplace(base, std::move(t));
        return &iter->second;
    }

    const Type* TypeContext::get_reference(const Type* base) const {
        auto it = reference_cache.find(base);
        if (it != reference_cache.end())
            return &it->second;

        Type t;
        t.kind = TypeKind::Reference;
        t.pointed = base;

        auto [iter, _] = reference_cache.emplace(base, std::move(t));
        return &iter->second;
    }

}
