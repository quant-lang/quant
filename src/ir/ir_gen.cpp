#include <functional>
#include <utility>
#include <variant>
#include <unordered_set>

#include "quant/ir/ir_gen.h"
#include "quant/attributes/attributes.h"
#include "quant/support/symbol_path.h"
#include "utils/logger.h"

using namespace utils::logger;

namespace quant::codegen {

namespace {

symb_t::Symbol* resolve_qualified(CompilerContext& ctx, const std::vector<std::string>& path) {
    if (path.empty()) return nullptr;
    if (path.size() == 1) return ctx.symbols.lookup(path[0]);

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
                return sym_it->second;
            }
        }
        ns = ns->parent;
    }

    if (path.size() >= 2) {
        auto* first_sym = ctx.symbols.lookup(path[0]);
        if (first_sym && std::holds_alternative<quant::symb_t::EnumSymbol>(first_sym->data)) {
            return ctx.symbols.lookup(path.back());
        }
    }

    return ctx.symbols.lookup(support::join_namespace(path));
};

auto lookup_struct = [](CompilerContext& ctx, const std::string& struct_name) -> symb_t::Symbol* {
    return resolve_qualified(ctx, support::split_path(struct_name));
};

int type_size(const ast::Type* t, CompilerContext* ctx = nullptr) {
    if (!t) return 0;
    switch (t->kind) {
        case ast::TypeKind::Bool:
        case ast::TypeKind::I8:
        case ast::TypeKind::U8:   return 1;
        case ast::TypeKind::I16:
        case ast::TypeKind::U16:  return 2;
        case ast::TypeKind::F32:
        case ast::TypeKind::I32:
        case ast::TypeKind::U32:  return 4;
        case ast::TypeKind::F64:
        case ast::TypeKind::I64:
        case ast::TypeKind::U64:  return 8;
        case ast::TypeKind::Pointer: return 8;
        case ast::TypeKind::Reference: return 8;
        case ast::TypeKind::NullPtr: return 8;
        case ast::TypeKind::Struct: {
            if (!ctx) return 0;
            auto* sym = lookup_struct(*ctx, t->struct_name);
            if (!sym && !t->type_args.empty()) {
                if (ctx->types.try_instantiate(t->struct_name, t->type_args)) {
                    const auto* fields = ctx->types.get_struct_fields(t->struct_name);
                    if (fields) {
                        const auto* field_attrs = ctx->types.get_struct_field_attrs(t->struct_name);
                        ctx->symbols.declare_struct_global(
                            t->struct_name, *fields, {},
                            field_attrs ? *field_attrs : std::vector<std::vector<ast::Attribute>>{}
                        );
                        sym = lookup_struct(*ctx, t->struct_name);
                    }
                }
            }
            if (!sym) {
                // Fallback: check type context for imported structs
                const auto* fields = ctx->types.get_struct_fields(t->struct_name);
                if (fields) {
                    int total = 0;
                    for (size_t i = 0; i < fields->size(); ++i) {
                        total += 8;
                    }
                    return total;
                }
                return 0;
            }
            auto* ss = std::get_if<quant::symb_t::StructSymbol>(&sym->data);
            if (!ss) return 0;
            int total = 0;
            for (size_t i = 0; i < ss->field_types.size(); ++i) {
                const ast::Type* ft = ss->field_types[i];
                if (ft && ft->kind == ast::TypeKind::Struct) {
                    total += type_size(ft, ctx);
                } else {
                    total += 8;  // each scalar field occupies an 8-byte qword
                }
            }
            return total;
        }
        default: return 0;
    }
}

// Byte offsets of each 8-byte qword in a struct's inline layout (flattened
// recursively for nested struct fields). Used for whole-struct copies.
std::vector<uint32_t> struct_flat_offsets(
    const ast::Type* t,
    CompilerContext& ctx
) {
    std::vector<uint32_t> out;
    std::function<void(const ast::Type*, uint32_t)> walk =
        [&](const ast::Type* ty, uint32_t base) {
            if (!ty || ty->kind != ast::TypeKind::Struct) {
                out.push_back(base);
                return;
            }
            auto* sym = lookup_struct(ctx, ty->struct_name);
            if (!sym && !ty->type_args.empty()) {
                if (ctx.types.try_instantiate(ty->struct_name, ty->type_args)) {
                    const auto* fields = ctx.types.get_struct_fields(ty->struct_name);
                    if (fields) {
                        const auto* field_attrs = ctx.types.get_struct_field_attrs(ty->struct_name);
                        ctx.symbols.declare_struct_global(
                            ty->struct_name, *fields, {},
                            field_attrs ? *field_attrs : std::vector<std::vector<ast::Attribute>>{}
                        );
                        sym = lookup_struct(ctx, ty->struct_name);
                    }
                }
            }
            auto* ss = sym ? std::get_if<quant::symb_t::StructSymbol>(&sym->data) : nullptr;
            if (!ss) return;
            uint32_t off = base;
            for (size_t i = 0; i < ss->field_types.size(); ++i) {
                const ast::Type* ft = ss->field_types[i];
                if (ft && ft->kind == ast::TypeKind::Struct) {
                    walk(ft, off);
                } else {
                    out.push_back(off);
                }
                off += (ft && ft->kind == ast::TypeKind::Struct) ? type_size(ft, &ctx) : 8;
            }
        };
    walk(t, 0);
    return out;
}

uint32_t struct_base_offset(
    const quant::symb_t::StructSymbol* ss,
    size_t index,
    CompilerContext& ctx
) {
    uint32_t offset = 0;
    for (size_t m = 0; m < index && m < ss->field_types.size(); ++m) {
        const ast::Type* ft = ss->field_types[m];
        offset += (ft && ft->kind == ast::TypeKind::Struct) ? type_size(ft, &ctx) : 8;
    }
    return offset;
}


template<class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

template<class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

bool is_declaration_stmt(const ast::Stmt& stmt) {
    return std::holds_alternative<ast::FuncStmt>(stmt.kind) ||
           std::holds_alternative<ast::NamespaceStmt>(stmt.kind) ||
           std::holds_alternative<ast::StructDecl>(stmt.kind) ||
           std::holds_alternative<ast::EnumDecl>(stmt.kind) ||
           std::holds_alternative<ast::UsingStmt>(stmt.kind) ||
           std::holds_alternative<ast::VarDecl>(stmt.kind);
}

const ast::Type* symbol_type(const quant::symb_t::Symbol& sym) {
    if (const auto* v = std::get_if<quant::symb_t::VarSymbol>(&sym.data)) {
        return v->type;
    }
    if (const auto* a = std::get_if<quant::symb_t::FuncArgSymbol>(&sym.data)) {
        return a->type;
    }
    return nullptr;
}

bool symbol_is_mutable(const quant::symb_t::Symbol& sym) {
    if (const auto* v = std::get_if<quant::symb_t::VarSymbol>(&sym.data)) {
        return v->is_mut;
    }
    if (const auto* a = std::get_if<quant::symb_t::FuncArgSymbol>(&sym.data)) {
        return a->is_mut;
    }
    return false;
}

std::pair<uint32_t, const ast::Type*> resolve_struct_field(
    CompilerContext& ctx,
    const ast::Type* base_type,
    const std::string& field_name
) {
    if (!base_type) {
        ctx.errors.add("Field access base type is null"); return {};
    }

    // Auto-deref references and pointers
    if (base_type->kind == ast::TypeKind::Reference && base_type->pointed) {
        base_type = base_type->pointed;
    }
    if (base_type->kind == ast::TypeKind::Pointer && base_type->pointed) {
        base_type = base_type->pointed;
    }

    if (base_type->kind != ast::TypeKind::Struct) {
        ctx.errors.add("Field access on non-struct type"); return {};
    }

    auto* sym = lookup_struct(ctx, base_type->struct_name);
    if (!sym && !base_type->type_args.empty()) {
        if (ctx.types.try_instantiate(base_type->struct_name, base_type->type_args)) {
            const auto* fields = ctx.types.get_struct_fields(base_type->struct_name);
            if (fields) {
                const auto* field_attrs = ctx.types.get_struct_field_attrs(base_type->struct_name);
                ctx.symbols.declare_struct_global(
                    base_type->struct_name, *fields, {},
                    field_attrs ? *field_attrs : std::vector<std::vector<ast::Attribute>>{}
                );
                sym = lookup_struct(ctx, base_type->struct_name);
            }
        }
    }
    if (!sym) {
        ctx.errors.add("Unknown struct: " + base_type->struct_name); return {};
    }

    auto* ss = std::get_if<quant::symb_t::StructSymbol>(&sym->data);
    if (!ss) {
        ctx.errors.add("Invalid struct symbol: " + base_type->struct_name); return {};
    }

    for (size_t i = 0; i < ss->field_names.size(); ++i) {
        if (ss->field_names[i] == field_name) {
            return {
                struct_base_offset(ss, i, ctx),
                ss->field_types[i]
            };
        }
    }

    ctx.errors.add("Unknown field: " + field_name); return {};
}

// Resolve a struct's field directly by struct name (used for implicit
// receiver field access in methods). Returns {offset, field_type}.
std::pair<uint32_t, const ast::Type*> resolve_struct_field_by_name(
    CompilerContext& ctx,
    const std::string& struct_name,
    const std::string& field_name
) {
    auto* sym = lookup_struct(ctx, struct_name);
    if (!sym) return {};

    auto* ss = std::get_if<quant::symb_t::StructSymbol>(&sym->data);
    if (!ss) return {};

    for (size_t i = 0; i < ss->field_names.size(); ++i) {
        if (ss->field_names[i] == field_name) {
            return {
                struct_base_offset(ss, i, ctx),
                ss->field_types[i]
            };
        }
    }

    return {};
}

// True if `method_name` is declared as a method of `struct_name`.
bool struct_has_method(CompilerContext& ctx,
                       const std::string& struct_name,
                       const std::string& method_name) {
    auto* sym = lookup_struct(ctx, struct_name);
    if (!sym) return false;

    auto* ss = std::get_if<quant::symb_t::StructSymbol>(&sym->data);
    if (!ss) return false;

    return std::find(ss->method_names.begin(), ss->method_names.end(), method_name)
        != ss->method_names.end();
}

bool is_signed_int_kind(ast::TypeKind k) {
    return k >= ast::TypeKind::I8 && k <= ast::TypeKind::I64;
}

bool is_unsigned_int_kind(ast::TypeKind k) {
    return k >= ast::TypeKind::U8 && k <= ast::TypeKind::U64;
}

bool is_float_kind(ast::TypeKind k) {
    return k == ast::TypeKind::F32 || k == ast::TypeKind::F64;
}

bool is_numeric_kind(ast::TypeKind k) {
    return k >= ast::TypeKind::I8 && k <= ast::TypeKind::F64;
}

int numeric_size(ast::TypeKind k) {
    switch (k) {
        case ast::TypeKind::I8:   case ast::TypeKind::U8:   return 1;
        case ast::TypeKind::I16:  case ast::TypeKind::U16:  return 2;
        case ast::TypeKind::I32:  case ast::TypeKind::U32:
        case ast::TypeKind::F32:                             return 4;
        case ast::TypeKind::I64:  case ast::TypeKind::U64:
        case ast::TypeKind::F64:                             return 8;
        default:                                             return 0;
    }
}

// Usual arithmetic conversions for binary IR emission.
ast::TypeKind binary_common_kind(ast::TypeKind a, ast::TypeKind b) {
    if (a == b) return a;
    if (a == ast::TypeKind::F64 || b == ast::TypeKind::F64) return ast::TypeKind::F64;
    if (a == ast::TypeKind::F32 || b == ast::TypeKind::F32) return ast::TypeKind::F32;

    const int sa = numeric_size(a);
    const int sb = numeric_size(b);
    const bool sia = is_signed_int_kind(a);
    const bool sib = is_signed_int_kind(b);
    if (sia == sib) return (sa >= sb) ? a : b;
    if (sia) return (sa > sb) ? a : b;
    return (sb > sa) ? b : a;
}


} // namespace

// For runtime attributes. (Now there is no runtime attributes)
void IRGenerator::emit_attr_lowering(const std::string& var_name) {
    (void)var_name;
}

void IRGenerator::emit_attr_lowering(const std::string& var_name, const std::vector<ast::Attribute>& attrs) {
    (void)var_name;
    (void)attrs;
}

IRGenerator::IRGenerator(CompilerContext& c)
    : ctx(c) {}

uint32_t IRGenerator::new_reg() {
    return next_reg++;
}

uint32_t IRGenerator::new_label() {
    return next_label++;
}

uint32_t IRGenerator::new_local() {
    return next_local++;
}

void IRGenerator::emit(const IRInst& inst) {
    if (!current_func) {
        ctx.errors.add("No current IR function set"); return;
    }

    current_func->body.push_back(inst);
}

IRBinaryOp IRGenerator::map_op(ast::BinaryOp op) {
    switch (op) {
        case ast::BinaryOp::Add:  return IRBinaryOp::Add;
        case ast::BinaryOp::Sub:  return IRBinaryOp::Sub;
        case ast::BinaryOp::Mul:  return IRBinaryOp::Mul;
        case ast::BinaryOp::Div:  return IRBinaryOp::Div;
        case ast::BinaryOp::Eq:   return IRBinaryOp::Eq;
        case ast::BinaryOp::Neq:  return IRBinaryOp::NotEq;
        case ast::BinaryOp::Lt:   return IRBinaryOp::Lt;
        case ast::BinaryOp::Lte:  return IRBinaryOp::Lte;
        case ast::BinaryOp::Gt:   return IRBinaryOp::Gt;
        case ast::BinaryOp::Gte:  return IRBinaryOp::Gte;
        case ast::BinaryOp::BitAnd:   return IRBinaryOp::BitAnd;
        case ast::BinaryOp::BitOr:    return IRBinaryOp::BitOr;
        case ast::BinaryOp::LogicAnd: return IRBinaryOp::LogicAnd;
        case ast::BinaryOp::LogicOr:  return IRBinaryOp::LogicOr;
        default:
            ctx.errors.add("Unsupported binary op"); return IRBinaryOp::Add;
    }
}

void IRGenerator::gen_program(std::span<quant::modules::Module* const> modules) {
    program.functions.clear();
    program.strings.clear();
    program.globals.clear();
    function_ids.clear();
    global_ids.clear();
    namespace_stack.clear();
    local_scopes.clear();
    type_scopes.clear();
    local_var_attrs.clear();

    next_reg = 0;
    next_label = 0;
    next_local = 0;
    current_terminated = false;
    current_func = nullptr;
    current_module = nullptr;

    auto register_function = [&](const std::string& qname) -> uint32_t {
        auto it = function_ids.find(qname);
        if (it != function_ids.end()) {
            return it->second;
        }

        const uint32_t id = static_cast<uint32_t>(program.functions.size());
        function_ids.emplace(qname, id);

        IRFunction fn;
        fn.id = id;
        fn.name = qname;
        program.functions.push_back(std::move(fn));

        return id;
    };

    // When using the pre-compiled static stdlib, track which function/global IDs
    // belong to std modules so we can mark them as extern later
    std::unordered_set<uint32_t> std_func_ids;
    std::unordered_set<uint32_t> std_global_ids;

    auto collect_functions_in_stmts =
        [&](auto&& self,
            const std::vector<ast::Stmt*>& stmts,
            const modules::Module* module) -> void
    {
        bool is_std = ctx.use_static_std && module->name.rfind("std::", 0) == 0;

        for (const auto* stmt : stmts) {
            if (!stmt) continue;

            std::visit(overloaded{
                [&](const ast::FuncStmt& fn) {
                    uint32_t id = register_function(
                        support::qualify_name(
                            module->namespace_path,
                            namespace_stack,
                            fn.name
                        )
                    );
                    if (is_std) std_func_ids.insert(id);
                },
                [&](const ast::StructDecl& str) {
                    // Register the struct's methods under
                    // <struct>::<method>; generic structs are skipped.
                    if (!str.type_params.empty()) {
                        return;
                    }
                    for (const auto& value : str.fields) {
                        const auto* fn = std::get_if<ast::FuncStmt>(&value);
                        if (!fn || !fn->type_params.empty()) continue;
                        uint32_t id = register_function(
                            support::qualify_name(
                                module->namespace_path,
                                namespace_stack,
                                str.name + "::" + fn->name
                            )
                        );
                        if (is_std) std_func_ids.insert(id);
                    }
                },
                [&](const ast::NamespaceStmt& ns) {
                    namespace_stack.push_back(ns.name);
                    if (ns.body) {
                        self(self, ns.body->stmts, module);
                    }
                    namespace_stack.pop_back();
                },
                [&](const auto&) {}
            }, stmt->kind);
        }
    };

    for (auto* mod : modules) {
        if (!mod) continue;
        collect_functions_in_stmts(collect_functions_in_stmts, mod->ast, mod);
    }

    // Register concrete generic function instantiations
    for (const auto& inst : ctx.generic_instantiations) {
        const auto& fn = inst.stmt;
        const std::string fkey = fn.struct_name
            ? std::string(fn.struct_name) + "::" + fn.name
            : fn.name;
        register_function(fkey);
    }

    auto register_global = [&](const std::string& qname, const ast::Type* type) -> uint32_t {
        auto it = global_ids.find(qname);
        if (it != global_ids.end()) {
            return it->second;
        }

        uint32_t sz = type_size(type, &ctx);
        if (sz == 0) sz = 8;
        if (sz < 8) sz = 8;

        const uint32_t id = static_cast<uint32_t>(program.globals.size());
        global_ids.emplace(qname, id);

        IRGlobal g;
        g.name = qname;
        g.size = sz;
        program.globals.push_back(std::move(g));

        return id;
    };

    auto collect_globals_in_stmts =
        [&](auto&& self,
            const std::vector<ast::Stmt*>& stmts,
            const modules::Module* module) -> void
        {
            bool is_std = ctx.use_static_std && module->name.rfind("std::", 0) == 0;
            for (const auto* stmt : stmts) {
                if (!stmt) continue;

                std::visit(overloaded{
                    [&](const ast::VarDecl& v) {
                        auto* sym = ctx.symbols.lookup(v.name);
                        if (!sym) {
                            return;
                        }
                        auto* vs = std::get_if<quant::symb_t::VarSymbol>(&sym->data);
                        if (!vs) {
                            return;
                        }

                        const std::string qname = support::qualify_name(
                            sym->owning_module, {}, v.name
                        );
                        uint32_t gid = register_global(qname, vs->type);
                        if (is_std) std_global_ids.insert(gid);
                    },
                [&](const ast::NamespaceStmt& ns) {
                    namespace_stack.push_back(ns.name);
                    if (ns.body) {
                        self(self, ns.body->stmts, module);
                    }
                    namespace_stack.pop_back();
                },
                [&](const auto&) {}
            }, stmt->kind);
        }
    };

    for (auto* mod : modules) {
        if (!mod) continue;
        for (const auto& part : mod->namespace_path) {
            ctx.symbols.enter_namespace(part);
        }
        collect_globals_in_stmts(collect_globals_in_stmts, mod->ast, mod);
        for (size_t i = 0; i < mod->namespace_path.size(); ++i) {
            ctx.symbols.exit_namespace();
        }
    }

    for (auto* mod : modules) {
        if (!mod) continue;

        // Static stdlib is linked from .a, so skip its IR generation.
        if (ctx.use_static_std && mod->name.rfind("std::", 0) == 0) continue;

        gen_module(*mod);
    }

    // Mark static stdlib symbols as extern for linker resolution.
    if (ctx.use_static_std) {
        for (uint32_t id : std_func_ids)
            program.functions[id].is_extern = true;

        for (uint32_t id : std_global_ids)
            program.globals[id].is_extern = true;
    }

    // Generate IR for concrete generic instantiations
    for (const auto& inst : ctx.generic_instantiations) {
        const auto& fn = inst.stmt;
        const std::string fkey = fn.struct_name
            ? std::string(fn.struct_name) + "::" + fn.name
            : fn.name;
        auto it = function_ids.find(fkey);
        if (it == function_ids.end()) {
            ctx.errors.add("Generic instantiation not registered: " + fkey); return;
        }

        // Skip codegen for static stdlib generics.
        if (ctx.use_static_std && std_func_ids.count(it->second)) {
            program.functions[it->second].is_extern = true;
            continue;
        }

        IRFunction* saved_func = current_func;
        uint32_t saved_next_reg = next_reg;
        uint32_t saved_next_local = next_local;
        bool saved_terminated = current_terminated;
        auto saved_locals = local_scopes;
        auto saved_types = type_scopes;
        auto saved_generic_ns = generic_module_ns;

        current_func = &program.functions[it->second];
        current_func->body.clear();
        current_func->local_count = 0;
        current_func->temp_count = 0;
        current_func->extra_stack = 0;
        current_func->is_extern = fn.is_extern;

        current_func->is_entry = false;
        current_func_return_type = fn.return_type;

        // Methods of generic structs are generated through the same path; they
        // carry struct_name and receive a pointer to the struct instance as
        // their first argument (like non-generic methods).
        const bool is_method = fn.struct_name != nullptr;
        std::string saved_method_struct_name = std::move(current_method_struct_name);
        current_method_struct_name = is_method ? std::string(fn.struct_name) : std::string();

        // Sret: return struct via hidden pointer arg
        const bool is_sret = fn.return_type &&
                             fn.return_type->kind == ast::TypeKind::Struct;
        current_func->sret = is_sret;
        current_func->arg_count = static_cast<uint32_t>(fn.args.size())
            + (is_sret ? 1 : 0) + (is_method ? 1 : 0);

    next_reg = 0;
    next_local = 0;
    current_terminated = false;

        local_scopes.clear();
        type_scopes.clear();
        local_scopes.emplace_back();
        type_scopes.emplace_back();

        if (is_method) {
            const uint32_t self_local = new_local();
            local_scopes.back()["self"] = self_local;
            type_scopes.back()["self"] = nullptr;
        }

        if (is_sret) {
            const uint32_t sret_local = new_local();
            local_scopes.back()["__sret_ptr"] = sret_local;
            type_scopes.back()["__sret_ptr"] = nullptr;
        }

        for (const auto& arg : fn.args) {
            const uint32_t local = new_local();
            local_scopes.back()[arg.name] = local;
            type_scopes.back()[arg.name] = arg.type;
        }

        if (fn.is_extern) {
            current_func = saved_func;
            next_reg = saved_next_reg;
            next_local = saved_next_local;
            current_terminated = saved_terminated;
            local_scopes = std::move(saved_locals);
            type_scopes = std::move(saved_types);
            current_method_struct_name = std::move(saved_method_struct_name);
            continue;
        }

        if (fn.body) {
            // Resolve unqualified names in the generic body from its defining
            // module's namespace, not the instantiation call site's module.
            generic_module_ns = inst.module_namespace;
            auto* saved_type_subst = current_type_subst;
            current_type_subst = &inst.type_subst;
            auto* saved_sym_ns = ctx.symbols.get_current_namespace();
            if (!inst.module_namespace.empty()) {
                ctx.symbols.set_current_namespace(ctx.symbols.create_namespace_path(inst.module_namespace));
            }
            gen_block(*fn.body);
            ctx.symbols.set_current_namespace(saved_sym_ns);
            current_type_subst = saved_type_subst;
            generic_module_ns = saved_generic_ns;
        }

        if (!current_terminated) {
            if (fn.return_type && fn.return_type->kind == ast::TypeKind::Void) {
                const uint32_t zero = new_reg();
                emit(IRLoadConst{ zero, 0 });
                emit(IRReturn{ zero });
                current_terminated = true;
            } else {
                ctx.errors.add("Missing return in non-void generic function: " + fn.name); return;
            }
        }

        current_func->local_count = next_local;
        current_func->temp_count = next_reg;

        current_func = saved_func;
        next_reg = saved_next_reg;
        next_local = saved_next_local;
        current_terminated = saved_terminated;
        local_scopes = std::move(saved_locals);
        type_scopes = std::move(saved_types);
        current_method_struct_name = std::move(saved_method_struct_name);
    }
}
void IRGenerator::gen_module(const quant::modules::Module& mod) {
    auto saved_namespace = namespace_stack;
    auto saved_module = current_module;

    current_module = &mod;
    namespace_stack.clear();

    // Enter the module's namespace so symbol lookups (e.g. struct types) can find them
    for (const auto& part : mod.namespace_path) {
        ctx.symbols.enter_namespace(part);
    }

    for (const auto* stmt : mod.ast) {
        if (!stmt) continue;

        if (std::holds_alternative<ast::LoadStmt>(stmt->kind) ||
            std::holds_alternative<ast::ModuleDecl>(stmt->kind) ||
            std::holds_alternative<ast::UsingStmt>(stmt->kind)) {
            continue;
        }

        if (!is_declaration_stmt(*stmt)) {
            ctx.errors.add("Top-level executable statements are not supported without __toplevel"); return;
        }

        gen_stmt(*stmt);
    }

    // Exit the module's namespace
    for (size_t i = 0; i < mod.namespace_path.size(); ++i) {
        ctx.symbols.exit_namespace();
    }

    current_module = saved_module;
    namespace_stack = std::move(saved_namespace);
}

void IRGenerator::gen_function(const ast::FuncStmt& func) {
    const std::string fn_key = func.struct_name ? std::string(func.struct_name) + "::" + func.name : func.name;
    const std::string qname = support::qualify_name(current_module->namespace_path, namespace_stack, fn_key);

    auto it = function_ids.find(qname);
    if (it == function_ids.end()) {
        ctx.errors.add("Function was not pre-collected: " + qname); return;
    }

    // Generic functions cannot be codegened without concrete type args
    if (!func.type_params.empty()) {
        return;
    }

    const bool is_method = func.struct_name != nullptr;

    IRFunction* saved_func = current_func;
    uint32_t saved_next_reg = next_reg;
    uint32_t saved_next_local = next_local;
    bool saved_terminated = current_terminated;
    auto saved_namespace = namespace_stack;
    auto saved_locals = local_scopes;
    auto saved_types = type_scopes;
    std::string saved_method_struct_name = std::move(current_method_struct_name);
    current_method_struct_name = is_method ? std::string(func.struct_name) : std::string();

    current_func = &program.functions[it->second];
    current_func->body.clear();
    current_func->local_count = 0;
    current_func->temp_count = 0;
    current_func->extra_stack = 0;
    current_func->is_extern = func.is_extern;
    current_func_return_type = func.return_type;

    current_func->is_entry = false;
    current_func->syscall_number = -1;
    current_func->import_dll.clear();
    current_func->import_name.clear();
    for (const auto& attr : func.attributes) {
        if (attr.name == "entry") {
            current_func->is_entry = true;
        } else if (attr.name == "syscall" && !attr.args.empty()) {
            if (const auto* num = std::get_if<ast::IntExpr>(&attr.args[0]->kind)) {
                current_func->syscall_number = num->value;
            }
        } else if (attr.name == "export" && !attr.args.empty()) {
            if (const auto* se = std::get_if<ast::StringExpr>(&attr.args[0]->kind)) {
                current_func->export_name = se->value;
            }
        } else if (attr.name == "import" && !attr.args.empty()) {
            if (const auto* se = std::get_if<ast::StringExpr>(&attr.args[0]->kind)) {
                current_func->import_dll = se->value;
                if (attr.args.size() > 1) {
                    if (const auto* se2 = std::get_if<ast::StringExpr>(&attr.args[1]->kind)) {
                        current_func->import_name = se2->value;
                    }
                }
            }
        }
    }

    // Sret: return struct via hidden pointer arg
    const bool is_sret = func.return_type &&
                         func.return_type->kind == ast::TypeKind::Struct;
    current_func->sret = is_sret;
    current_func->arg_count = static_cast<uint32_t>(func.args.size()) + (is_sret ? 1 : 0) + (is_method ? 1 : 0);

    next_reg = 0;
    next_local = 0;
    current_terminated = false;

    local_scopes.clear();
    type_scopes.clear();
    local_scopes.emplace_back();
    type_scopes.emplace_back();

    // Methods receive a pointer to the struct instance as their first
    // argument (arg slot 0), before any user arguments or the sret pointer.
    if (is_method) {
        const uint32_t self_local = new_local();
        local_scopes.back()["self"] = self_local;
        type_scopes.back()["self"] = nullptr;
    }

    if (is_sret) {
        const uint32_t sret_local = new_local();
        local_scopes.back()["__sret_ptr"] = sret_local;
        type_scopes.back()["__sret_ptr"] = nullptr;
    }

    for (const auto& arg : func.args) {
        const uint32_t local = new_local();
        local_scopes.back()[arg.name] = local;
        type_scopes.back()[arg.name] = arg.type;
    }

    if (func.is_extern) {
        current_func = saved_func;
        next_reg = saved_next_reg;
        next_local = saved_next_local;
        current_terminated = saved_terminated;
        namespace_stack = std::move(saved_namespace);
        local_scopes = std::move(saved_locals);
        type_scopes = std::move(saved_types);
        current_method_struct_name = std::move(saved_method_struct_name);
        return;
    }

    if (func.body) {
        gen_block(*func.body);
    }

    if (!current_terminated) {
        if (func.return_type && func.return_type->kind == ast::TypeKind::Void) {
            const uint32_t zero = new_reg();
            emit(IRLoadConst{ zero, 0 });
            emit(IRReturn{ zero });
            current_terminated = true;
        } else {
            ctx.errors.add("Missing return in non-void function: " + func.name); return;
        }
    }

    current_func->local_count = next_local;
    current_func->temp_count = next_reg;

    current_func = saved_func;
    next_reg = saved_next_reg;
    next_local = saved_next_local;
    current_terminated = saved_terminated;
    namespace_stack = std::move(saved_namespace);
    local_scopes = std::move(saved_locals);
    type_scopes = std::move(saved_types);
    current_method_struct_name = std::move(saved_method_struct_name);
}

void IRGenerator::gen_block(const ast::Block& block) {
    const bool outer_terminated = current_terminated;
    current_terminated = false;

    local_scopes.emplace_back();
    type_scopes.emplace_back();

    for (const auto* stmt : block.stmts) {
        if (!stmt || current_terminated) {
            break;
        }
        gen_stmt(*stmt);
    }

    const bool block_terminated = current_terminated;

    type_scopes.pop_back();
    local_scopes.pop_back();

    current_terminated = outer_terminated || block_terminated;
}

void IRGenerator::gen_stmt(const ast::Stmt& stmt) {
    if (current_terminated) {
        return;
    }

    std::visit(overloaded{
        [&](const ast::ExprStmt& node) {
            if (node.expr) {
                (void)gen_expr(*node.expr);
            }
        },

        [&](const ast::VarDecl& node) {
            if (!current_func) {
                // Top-level globals are handled by the collection pass in gen_program.
                return;
            }
            if (!node.type) {
                ctx.errors.add("Variable declaration missing type: " + node.name); return;
            }

            const uint32_t local = new_local();
            local_scopes.back()[node.name] = local;
            // Use concrete type from symbol table if available (generic instantiation)
            const ast::Type* var_type = node.type;
            {
                auto* sym = ctx.symbols.lookup(node.name);
                if (sym) {
                    if (auto* vs = std::get_if<symb_t::VarSymbol>(&sym->data)) {
                        var_type = vs->type;
                    } else if (auto* as = std::get_if<symb_t::FuncArgSymbol>(&sym->data)) {
                        var_type = as->type;
                    }
                }
            }
            type_scopes.back()[node.name] = var_type;
            local_var_attrs[node.name] = node.attributes;

            if (var_type->kind == ast::TypeKind::Struct) {
                int sz = type_size(var_type, &ctx);
                if (sz > 0) {
                    if (current_func) current_func->extra_stack += sz;
                    Reg ptr = new_reg();
                    emit(IRAlloca{ptr, static_cast<uint32_t>(current_func ? current_func->extra_stack : 0)});
                    emit(IRStoreLocal{local, ptr});
                }
                if (node.value) {
                    const uint32_t value = gen_expr(*node.value);
                    // Copy struct fields from result into local's allocated space
                    const uint32_t local_ptr = new_reg();
                    emit(IRLoadLocal{local_ptr, local});
                    const auto flat = struct_flat_offsets(var_type, ctx);
                    for (uint32_t offset : flat) {
                        const uint32_t src_val = new_reg();
                        emit(IRGetField{src_val, value, offset});
                        emit(IRSetField{local_ptr, src_val, offset});
                    }
                }
            } else if (node.value) {
                const uint32_t value = gen_expr_as(*node.value, var_type);
                emit(IRStoreLocal{ local, value });
            }
        },

        [&](const ast::ReturnStmt& node) {
            if (!current_func) {
                ctx.errors.add("Return outside function"); return;
            }

            if (current_func->sret && node.value) {
                // Load sret pointer from the hidden arg
                uint32_t sret_local = 0;
                {
                    bool found = false;
                    for (int i = static_cast<int>(local_scopes.size()) - 1; i >= 0; --i) {
                        auto it = local_scopes[i].find("__sret_ptr");
                        if (it != local_scopes[i].end()) {
                            sret_local = it->second;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        ctx.errors.add("sret pointer not found"); return;
                    }
                }
                const uint32_t sret_ptr = new_reg();
                emit(IRLoadLocal{ sret_ptr, sret_local });

                // Evaluate return value (produces a pointer to the struct data)
                const uint32_t result_ptr = gen_expr(*node.value);

                // Copy each field from result to sret buffer
                if (current_func_return_type &&
                    current_func_return_type->kind == ast::TypeKind::Struct) {
                    auto* sym = lookup_struct(ctx, current_func_return_type->struct_name);
                    if (sym) {
                        auto* ss = std::get_if<quant::symb_t::StructSymbol>(&sym->data);
                        if (ss) {
                            const auto flat = struct_flat_offsets(current_func_return_type, ctx);
                            for (uint32_t offset : flat) {
                                const uint32_t tmp = new_reg();
                                emit(IRGetField{ tmp, result_ptr, offset });
                                emit(IRSetField{ sret_ptr, tmp, offset });
                            }
                        }
                    }
                }

                const uint32_t zero = new_reg();
                emit(IRLoadConst{ zero, 0 });
                emit(IRReturn{ zero });
            } else if (node.value) {
                const uint32_t value = gen_expr_as(*node.value, current_func_return_type);
                emit(IRReturn{ value });
            } else {
                const uint32_t zero = new_reg();
                emit(IRLoadConst{ zero, 0 });
                emit(IRReturn{ zero });
            }

            current_terminated = true;
        },

        [&](const ast::BreakStmt&) {
            if (break_labels.empty()) {
                ctx.errors.add("'break' must be inside a loop or switch");
                return;
            }
            emit(IRJump{ break_labels.back() });
            current_terminated = true;
        },

        [&](const ast::ContinueStmt&) {
            if (continue_labels.empty()) {
                ctx.errors.add("'continue' must be inside a loop");
                return;
            }
            emit(IRJump{ continue_labels.back() });
            current_terminated = true;
        },

        [&](const ast::IfStmt& node) {
            if (!node.condition) {
                ctx.errors.add("If statement missing condition"); return;
            }

            const uint32_t cond = gen_expr(*node.condition);

            const uint32_t then_label = new_label();
            const uint32_t end_label  = new_label();

            const bool has_else_if = node.else_if != nullptr;
            const bool has_else = node.else_block != nullptr;
            const uint32_t else_label = (has_else || has_else_if) ? new_label() : end_label;

            emit(IRBranch{
                cond,
                then_label,
                else_label
            });

            emit(IRLabel{ then_label });
            current_terminated = false;
            if (node.then_block) {
                gen_block(*node.then_block);
            }

            bool all_terminated = current_terminated;
            if (!all_terminated) {
                emit(IRJump{ end_label });
            }

            bool has_default = false;
            if (has_else_if) {
                emit(IRLabel{ else_label });
                current_terminated = false;

                for (auto* ei = node.else_if; ei; ei = ei->next) {
                    const uint32_t ei_cond = gen_expr(*ei->condition);
                    const uint32_t ei_then = new_label();
                    const bool has_rest = ei->next || ei->else_block;
                    const uint32_t ei_else = has_rest ? new_label() : end_label;
                    emit(IRBranch{ ei_cond, ei_then, ei_else });

                    emit(IRLabel{ ei_then });
                    current_terminated = false;
                    if (ei->then_block) {
                        gen_block(*ei->then_block);
                    }
                    all_terminated = all_terminated && current_terminated;
                    if (!current_terminated) {
                        emit(IRJump{ end_label });
                    }

                    if (ei->next) {
                        emit(IRLabel{ ei_else });
                        continue;
                    }

                    if (ei->else_block) {
                        emit(IRLabel{ ei_else });
                        current_terminated = false;
                        gen_block(*ei->else_block);
                        all_terminated = all_terminated && current_terminated;
                        if (!current_terminated) {
                            emit(IRJump{ end_label });
                        }
                        has_default = true;
                    }
                }
            } else if (has_else) {
                emit(IRLabel{ else_label });
                current_terminated = false;
                gen_block(*node.else_block);
                all_terminated = all_terminated && current_terminated;
                if (!current_terminated) {
                    emit(IRJump{ end_label });
                }
                has_default = true;
            }

            emit(IRLabel{ end_label });
            current_terminated = has_default && all_terminated;
        },

        [&](const ast::WhileStmt& node) {
            if (!node.condition) {
                ctx.errors.add("While statement missing condition"); return;
            }

            const uint32_t cond_label = new_label();
            const uint32_t body_label = new_label();
            const uint32_t end_label  = new_label();
            // For a desugared 'for' loop, 'continue' must run the step before
            // the condition re-check, so it targets a label placed right
            // before the step instead of the loop head.
            const bool has_step = node.for_step != nullptr;
            const uint32_t continue_label = has_step ? new_label() : cond_label;

            emit(IRJump{ cond_label });

            emit(IRLabel{ cond_label });
            const uint32_t cond = gen_expr(*node.condition);
            emit(IRBranch{ cond, body_label, end_label });

            emit(IRLabel{ body_label });
            current_terminated = false;
            break_labels.push_back(end_label);
            continue_labels.push_back(continue_label);
            if (node.body) {
                gen_block(*node.body);
            }
            break_labels.pop_back();
            continue_labels.pop_back();

            if (has_step) {
                emit(IRLabel{ continue_label });
                (void)gen_expr(*node.for_step);
                current_terminated = false;
            }

            if (!current_terminated) {
                emit(IRJump{ cond_label });
            }

            emit(IRLabel{ end_label });
            current_terminated = false;
        },

        [&](const ast::BlockStmt& node) {
            if (node.body) {
                gen_block(*node.body);
            }
        },

        [&](const ast::SwitchStmt& node) {
            if (!node.condition) {
                ctx.errors.add("Switch statement missing condition"); return;
            }

            ast::TypeKind tk = ast::TypeKind::I32;
            if (node.condition->resolved_type) {
                tk = node.condition->resolved_type->kind;
            }

            const uint32_t cond = gen_expr(*node.condition);

            const uint32_t end_label = new_label();
            emit(IRLabel{ new_label() });   // entry point of the first case check

            bool all_terminated = true;

            break_labels.push_back(end_label);

            for (const auto& cs : node.cases) {
                if (cs.values.empty() || cs.const_values.size() != cs.values.size()) {
                    ctx.errors.add("Case is missing a value"); return;
                }

                const uint32_t body_label = new_label();
                const uint32_t after_label = new_label(); // entry to the next group / default

                for (size_t i = 0; i < cs.values.size(); ++i) {
                    if (!cs.const_values[i]) {
                        ctx.errors.add("Case value was not resolved to a constant"); return;
                    }

                    const uint32_t case_const = new_reg();
                    emit(IRLoadConst{ case_const, *cs.const_values[i] });

                    const uint32_t cmp = new_reg();
                    emit(IRBinary{ IRBinaryOp::Eq, cmp, cond, case_const, tk });

                    if (i + 1 < cs.values.size()) {
                        const uint32_t mid = new_label();
                        emit(IRBranch{ cmp, body_label, mid });
                        emit(IRLabel{ mid });
                    } else {
                        emit(IRBranch{ cmp, body_label, after_label });
                    }
                }

                emit(IRLabel{ body_label });
                current_terminated = false;
                if (cs.body) {
                    gen_block(*cs.body);
                }
                all_terminated = all_terminated && current_terminated;
                if (!current_terminated) {
                    emit(IRJump{ end_label });
                }

                emit(IRLabel{ after_label });
            }

            bool has_default = node.default_block != nullptr;
            if (has_default) {
                current_terminated = false;
                gen_block(*node.default_block);
                all_terminated = all_terminated && current_terminated;
                if (!current_terminated) {
                    emit(IRJump{ end_label });
                }
            }

            emit(IRLabel{ end_label });
            current_terminated = has_default && all_terminated;

            break_labels.pop_back();
        },

        [&](const ast::StructDecl& str) {
            // Compile-time only, except the struct's methods. Generic structs
            // are skipped here: their methods are monomorphized per concrete
            // instantiation and generated from ctx.generic_instantiations.
            if (!str.type_params.empty()) {
                return;
            }
            for (const auto& value : str.fields) {
                if (const auto* fn = std::get_if<ast::FuncStmt>(&value)) {
                    gen_function(*fn);
                }
            }
        },

        [&](const ast::EnumDecl&) {
            // Compile-time only.
        },

        [&](const ast::FuncStmt& fn) {
            gen_function(fn);
        },

        [&](const ast::NamespaceStmt& ns) {
            namespace_stack.push_back(ns.name);

            const bool saved_terminated = current_terminated;

            if (ns.body) {
                for (const auto* child : ns.body->stmts) {
                    if (!child) {
                        continue;
                    }

                    if (!is_declaration_stmt(*child)) {
                        ctx.errors.add("Namespace scopes may only contain declarations"); return;
                    }

                    gen_stmt(*child);
                }
            }

            current_terminated = saved_terminated;
            namespace_stack.pop_back();
        },
        [&](const ast::LoadStmt&) {
            // compile-time only
        },
        [&](const ast::UsingStmt&) {
            // compile-time only
        },

        [&](const ast::RegionStmt& node) {
            gen_region(node);
        },

        [&](const auto&) {
            ctx.errors.add("Unsupported statement node in IR generation"); return;
        }
    }, stmt.kind);
}

void IRGenerator::gen_region(const ast::RegionStmt& reg) {
    if (!current_func) {
        ctx.errors.add("Region outside function"); return;
    }

    // Allocate Region struct on stack (3 fields * 8 = 24 bytes)
    if (current_func) current_func->extra_stack += 24;
    Local region_local = new_local();
    Reg struct_ptr = new_reg();
    emit(IRAlloca{struct_ptr, static_cast<uint32_t>(current_func->extra_stack)});
    emit(IRStoreLocal{region_local, struct_ptr});

    region_stack.push_back({region_local});

    emit(IRRegionBegin{region_local, 1024 * 1024});

    if (reg.body) {
        gen_block(*reg.body);
    }

    emit(IRRegionEnd{region_local});

    region_stack.pop_back();
}

uint32_t IRGenerator::gen_expr(const ast::Expr& expr) {
    auto lookup_local = [&](const std::string& name, uint32_t& local_out, const ast::Type*& type_out) -> bool {
        for (int i = static_cast<int>(local_scopes.size()) - 1; i >= 0; --i) {
            auto it = local_scopes[i].find(name);
            if (it != local_scopes[i].end()) {
                local_out = it->second;
                auto tit = type_scopes[i].find(name);
                if (tit != type_scopes[i].end()) {
                    type_out = tit->second;
                } else {
                    type_out = nullptr;
                }
                return true;
            }
        }
        return false;
    };

    auto resolve_function_id = [&](const std::vector<std::string>& path) -> uint32_t {
        const std::string qname = support::join_namespace(path);
        auto it = function_ids.find(qname);
        if (it != function_ids.end()) {
            return it->second;
        }


        if (current_module || !generic_module_ns.empty()) {
            const std::vector<std::string>& mod_path = generic_module_ns.empty()
                ? current_module->namespace_path : generic_module_ns;
            {
                const std::string qualified = support::qualify_name(
                    mod_path,
                    namespace_stack,
                    support::join_namespace(path)
                );
                auto it2 = function_ids.find(qualified);
                if (it2 != function_ids.end()) {
                    return it2->second;
                }
            }
            {
                std::vector<std::string> full = mod_path;
                full.insert(full.end(), path.begin(), path.end());
                const std::string qualified = support::join_namespace(full);
                auto it2 = function_ids.find(qualified);
                if (it2 != function_ids.end()) {
                    return it2->second;
                }
            }
            if (path.size() == 1) {
                const std::string qualified = support::qualify_name(
                    mod_path,
                    namespace_stack,
                    path[0]
                );
                auto it2 = function_ids.find(qualified);
                if (it2 != function_ids.end()) {
                    return it2->second;
                }
            }
        }

        // Last resort: check if the symbol was imported via `using`
        if (auto* sym = resolve_qualified(ctx, path)) {
            if (std::get_if<quant::symb_t::FuncSymbol>(&sym->data)) {
                std::vector<std::string> full = sym->owning_module;
                full.insert(full.end(), path.begin(), path.end());
                const std::string qualified = support::join_namespace(full);
                auto it3 = function_ids.find(qualified);
                if (it3 != function_ids.end()) {
                    return it3->second;
                }
            }
        }

        ctx.errors.add("Undefined function: " + qname);
        return 0;
    };

    return std::visit(overloaded{
        [&](const ast::IntExpr& node) -> uint32_t {
            const uint32_t dst = new_reg();
            emit(IRLoadConst{ dst, node.value });
            return dst;
        },

        [&](const ast::BoolExpr& node) -> uint32_t {
            const uint32_t dst = new_reg();
            emit(IRLoadConst{ dst, node.value ? 1 : 0 });
            return dst;
        },

        [&](const ast::NullPtrExpr&) -> uint32_t {
            const uint32_t dst = new_reg();
            emit(IRLoadConst{ dst, 0 });
            return dst;
        },

        [&](const ast::FloatExpr& node) -> uint32_t {
            const uint32_t dst = new_reg();
            ast::TypeKind kind = ast::TypeKind::F64;
            if (expr.resolved_type && expr.resolved_type->kind == ast::TypeKind::F32) {
                kind = ast::TypeKind::F32;
            }
            emit(IRLoadFloatConst{ dst, node.value, kind });
            return dst;
        },

        [&](const ast::StringExpr& node) -> uint32_t {
            const Reg dst = new_reg();

            uint32_t id = program.strings.size();

            program.strings.push_back(IRString{
                id,
                node.value
            });

            emit(IRLoadString{ dst, id });

            return dst;
        },

        [&](const ast::CharExpr& node) -> uint32_t {
            const uint32_t dst = new_reg();
            emit(IRLoadConst{ dst, node.value });
            return dst;
        },

        [&](const ast::VarExpr& node) -> uint32_t {
            uint32_t local = 0;
            const ast::Type* type = nullptr;

            if (lookup_local(node.name, local, type)) {
                emit_attr_lowering(node.name);
                (void)type;
                const uint32_t dst = new_reg();
                emit(IRLoadLocal{ dst, local });
                return dst;
            }

            // Bare field read inside a method: <self>.<field>.
            if (!current_method_struct_name.empty()) {
                const auto field = resolve_struct_field_by_name(ctx, current_method_struct_name, node.name);
                if (field.second) {
                    uint32_t self_local = 0;
                    const ast::Type* self_type = nullptr;
                    if (lookup_local("self", self_local, self_type)) {
                        const uint32_t self_ptr = new_reg();
                        emit(IRLoadLocal{ self_ptr, self_local });
                        const uint32_t dst = new_reg();
                        emit(IRGetField{ dst, self_ptr, field.first });
                        return dst;
                    }
                }
            }

            auto* sym = ctx.symbols.lookup(node.name);
            if (!sym) {
                ctx.errors.add("Undefined variable: " + node.name); return 0;
            }

            if (!symbol_type(*sym)) {
                ctx.errors.add("Symbol is not a value: " + node.name); return 0;
            }

            if (auto* vs = std::get_if<symb_t::VarSymbol>(&sym->data)) {
                if (!vs->is_mut && vs->const_value.has_value()) {
                    const uint32_t dst = new_reg();
                    emit(IRLoadConst{ dst, static_cast<int32_t>(*vs->const_value) });
                    return dst;
                }

                const std::string qname = support::qualify_name(
                    sym->owning_module, {}, node.name
                );
                auto git = global_ids.find(qname);
                if (git != global_ids.end()) {
                    const uint32_t dst = new_reg();
                    if (vs->type && vs->type->kind == ast::TypeKind::Struct) {
                        emit(IRLoadGlobalAddr{ dst, git->second });
                    } else {
                        emit(IRLoadGlobal{ dst, git->second });
                    }
                    return dst;
                }
            }

            ctx.errors.add("Global value lowering is not implemented yet: " + node.name); return 0;
        },

        [&](const ast::BinaryExpr& node) -> uint32_t {
            ast::TypeKind lhs_kind = node.lhs && node.lhs->resolved_type
                ? node.lhs->resolved_type->kind : ast::TypeKind::I32;
            ast::TypeKind rhs_kind = node.rhs && node.rhs->resolved_type
                ? node.rhs->resolved_type->kind : lhs_kind;
            const ast::TypeKind common = binary_common_kind(lhs_kind, rhs_kind);

            // Widen each operand to the common type before the operation.
            // nullptr is a pointer-sized zero, so it needs no cast (the
            // comparison just works as a plain 8-byte compare).
            auto widen_operand = [&](const ast::Expr* op, ast::TypeKind k) -> uint32_t {
                const uint32_t reg = gen_expr(*op);
                if (k == common || k == ast::TypeKind::NullPtr) return reg;
                const uint32_t c = new_reg();
                emit(IRCast{ c, reg, k, common, ast::CastKind::ValueCast });
                return c;
            };

            const uint32_t lhs = widen_operand(node.lhs, lhs_kind);
            const uint32_t rhs = widen_operand(node.rhs, rhs_kind);
            const uint32_t dst = new_reg();

            emit(IRBinary{
                map_op(node.op),
                dst,
                lhs,
                rhs,
                common
            });

            return dst;
        },

        [&](const ast::UnaryExpr& node) -> uint32_t {
            const ast::Type* op_type = node.operand ? node.operand->resolved_type : nullptr;

            // Address-of: &expr
            if (node.op == ast::UnaryOp::AddrOf) {
                // Handle &var (local variable)
                if (const auto* var = std::get_if<ast::VarExpr>(&node.operand->kind)) {
                    const ast::Type* var_type = nullptr;
                    for (int i = static_cast<int>(type_scopes.size()) - 1; i >= 0; --i) {
                        auto tit = type_scopes[i].find(var->name);
                        if (tit != type_scopes[i].end()) {
                            var_type = tit->second;
                            break;
                        }
                    }
                    for (int i = static_cast<int>(local_scopes.size()) - 1; i >= 0; --i) {
                        auto it = local_scopes[i].find(var->name);
                        if (it != local_scopes[i].end()) {
                            const uint32_t local = it->second;
                            const uint32_t dst = new_reg();
                            // Struct locals store a pointer via IRAlloca — the local IS the pointer
                            if (var_type && var_type->kind == ast::TypeKind::Struct) {
                                emit(IRLoadLocal{ dst, local });
                            } else {
                                emit(IRAddrOf{ dst, local });
                            }
                            return dst;
                        }
                    }

                    // Global variable address
                    auto* gsym = ctx.symbols.lookup(var->name);
                    if (gsym) {
                        if (auto* gvs = std::get_if<symb_t::VarSymbol>(&gsym->data)) {
                            if (gvs->is_mut || !gvs->const_value.has_value()) {
                                const std::string qname = support::qualify_name(
                                    gsym->owning_module, {}, var->name
                                );
                                auto git = global_ids.find(qname);
                                if (git != global_ids.end()) {
                                    const uint32_t dst = new_reg();
                                    emit(IRLoadGlobalAddr{ dst, git->second });
                                    return dst;
                                }
                            }
                        }
                    }
                }
                ctx.errors.add("Cannot take address of non-local expression");
                return 0;
            }

            const uint32_t operand = gen_expr(*node.operand);
            const uint32_t dst = new_reg();
            const uint32_t zero = new_reg();
            emit(IRLoadConst{ zero, 0 });

            ast::TypeKind tk = ast::TypeKind::I32;
            if (node.operand && node.operand->resolved_type) {
                tk = node.operand->resolved_type->kind;
            }

            if (node.op == ast::UnaryOp::Neg) {
                emit(IRBinary{ IRBinaryOp::Sub, dst, zero, operand, tk });
            } else {
                emit(IRBinary{ IRBinaryOp::Eq, dst, operand, zero, tk });
            }

            return dst;
        },

        [&](const ast::AssignExpr& node) -> uint32_t {
            if (!node.target || !node.value) {
                ctx.errors.add("Assignment target or value is missing"); return 0;
            }

            if (const auto* var = std::get_if<ast::VarExpr>(&node.target->kind)) {
                for (int i = static_cast<int>(local_scopes.size()) - 1; i >= 0; --i) {
                    auto it = local_scopes[i].find(var->name);
                    if (it != local_scopes[i].end()) {
                        const uint32_t local = it->second;
                        const ast::Type* var_type = nullptr;
                        auto tit = type_scopes[i].find(var->name);
                        if (tit != type_scopes[i].end()) var_type = tit->second;
                        const uint32_t v = gen_expr_as(*node.value, var_type);
                        emit(IRStoreLocal{ local, v });
                        return v;
                    }
                }

                // Bare field write inside a method: <self>.<field> = value.
                if (!current_method_struct_name.empty()) {
                    const auto field = resolve_struct_field_by_name(ctx, current_method_struct_name, var->name);
                    if (field.second) {
                        uint32_t self_local = 0;
                        const ast::Type* self_type = nullptr;
                        if (lookup_local("self", self_local, self_type)) {
                            const uint32_t self_ptr = new_reg();
                            emit(IRLoadLocal{ self_ptr, self_local });
                            const uint32_t v = gen_expr_as(*node.value, field.second);
                            emit(IRSetField{ self_ptr, v, field.first });
                            return v;
                        }
                    }
                }

                auto* sym = ctx.symbols.lookup(var->name);
                if (!sym) {
                    ctx.errors.add("Undefined variable: " + var->name); return 0;
                }

                if (!symbol_is_mutable(*sym)) {
                    ctx.errors.add("Cannot assign to immutable variable: " + var->name); return 0;
                }

                const std::string qname = support::qualify_name(
                    sym->owning_module, {}, var->name
                );
                auto git = global_ids.find(qname);
                if (git != global_ids.end()) {
                    const uint32_t v = gen_expr_as(*node.value, symbol_type(*sym));
                    emit(IRStoreGlobal{ git->second, v });
                    return v;
                }

                ctx.errors.add("Global assignment lowering is not implemented yet: " + var->name); return 0;
            }

            if (const auto* field = std::get_if<ast::FieldExpr>(&node.target->kind)) {
                const uint32_t base = [&]() -> uint32_t {
                    if (const auto* bv = std::get_if<ast::VarExpr>(&field->base->kind)) {
                        uint32_t loc = 0;
                        const ast::Type* t = nullptr;
                        if (lookup_local(bv->name, loc, t)) {
                            const uint32_t d = new_reg();
                            emit(IRLoadLocal{ d, loc });
                            return d;
                        }
                    }
                    return gen_expr(*field->base);
                }();
                const ast::Type* base_type = nullptr;

                if (const auto* base_var = std::get_if<ast::VarExpr>(&field->base->kind)) {
                    for (int i = static_cast<int>(local_scopes.size()) - 1; i >= 0; --i) {
                        auto tit = type_scopes[i].find(base_var->name);
                        if (tit != type_scopes[i].end()) {
                            base_type = tit->second;
                            break;
                        }
                    }

                    if (!base_type) {
                        auto* sym = ctx.symbols.lookup(base_var->name);
                        if (sym) {
                            base_type = symbol_type(*sym);
                        }
                    }
                }

                // Nested field access (e.g. a.b.c.d = x): the semantic pass
                // already resolved the intermediate expression's type.
                if (!base_type) {
                    base_type = field->base->resolved_type;
                }

                const auto [offset, field_type] = resolve_struct_field(ctx, base_type, field->field);

                const uint32_t v = gen_expr_as(*node.value, field_type);

                if (field_type && field_type->kind == ast::TypeKind::Struct) {
                    const auto flat = struct_flat_offsets(field_type, ctx);
                    for (uint32_t sub : flat) {
                        const uint32_t src_val = new_reg();
                        emit(IRGetField{src_val, v, sub});
                        emit(IRSetField{base, src_val, static_cast<uint32_t>(offset + sub)});
                    }
                    return v;
                }

                emit(IRSetField{
                    base,
                    v,
                    offset
                });

                return v;
            }

            if (const auto* index = std::get_if<ast::IndexExpr>(&node.target->kind)) {
                const uint32_t base = gen_expr(*index->base);
                const uint32_t idx = gen_expr(*index->index);

                const ast::Type* base_type = index->base->resolved_type;
                // Auto-deref references
                if (base_type && base_type->kind == ast::TypeKind::Reference && base_type->pointed) {
                    base_type = base_type->pointed;
                }
                if (!base_type || base_type->kind != ast::TypeKind::Pointer || !base_type->pointed) {
                    ctx.errors.add("Invalid pointer index in assignment"); return 0;
                }

                const ast::Type* elem_type = base_type->pointed;
                uint32_t elem_size = type_size(elem_type, &ctx);
                const uint32_t v = gen_expr_as(*node.value, elem_type);

                if (elem_type && elem_type->kind == ast::TypeKind::Struct) {
                    const uint32_t sz = new_reg();
                    emit(IRLoadConst{sz, static_cast<int64_t>(elem_size)});
                    const uint32_t scaled = new_reg();
                    emit(IRBinary{IRBinaryOp::Mul, scaled, idx, sz, ast::TypeKind::U64});
                    const uint32_t addr = new_reg();
                    emit(IRBinary{IRBinaryOp::Add, addr, base, scaled, ast::TypeKind::U64});

                    const auto flat = struct_flat_offsets(elem_type, ctx);
                    for (uint32_t sub : flat) {
                        const uint32_t src_val = new_reg();
                        emit(IRGetField{src_val, v, sub});
                        emit(IRSetField{addr, src_val, sub});
                    }
                    return v;
                }

                emit(IRStoreElement{base, idx, v, elem_size});
                return v;
            }

            ctx.errors.add("Assignment target must be variable, field access, or index expression"); return 0;
        },

        [&](const ast::FieldExpr& node) -> uint32_t {
            const uint32_t base = gen_expr(*node.base);
            const ast::Type* base_type = nullptr;

            if (const auto* base_var = std::get_if<ast::VarExpr>(&node.base->kind)) {
                for (int i = static_cast<int>(local_scopes.size()) - 1; i >= 0; --i) {
                    auto tit = type_scopes[i].find(base_var->name);
                    if (tit != type_scopes[i].end()) {
                        base_type = tit->second;
                        break;
                    }
                }

                if (!base_type) {
                    auto* sym = ctx.symbols.lookup(base_var->name);
                    if (sym) {
                        base_type = symbol_type(*sym);
                    }
                }
            }

            // Nested field access (e.g. a.b.c): the semantic pass already
            // resolved the intermediate expression's type.
            if (!base_type) {
                base_type = node.base->resolved_type;
            }

            const auto [offset, field_type] = resolve_struct_field(ctx, base_type, node.field);

            // Auto-deref references and pointers for attribute lookup
            if (base_type && (base_type->kind == ast::TypeKind::Reference ||
                              base_type->kind == ast::TypeKind::Pointer) &&
                base_type->pointed) {
                base_type = base_type->pointed;
            }

            // Emit attribute lowering for struct field reads (e.g. @guard)
            if (base_type && base_type->kind == ast::TypeKind::Struct) {
                auto* sym = lookup_struct(ctx, base_type->struct_name);
                if (sym) {
                    auto* ss = std::get_if<quant::symb_t::StructSymbol>(&sym->data);
                    if (ss) {
                        for (size_t i = 0; i < ss->field_names.size(); ++i) {
                            if (ss->field_names[i] == node.field && i < ss->field_attributes.size()) {
                                emit_attr_lowering(node.field, ss->field_attributes[i]);
                                break;
                            }
                        }
                    }
                }
            }

            // Struct-typed fields are stored inline at their cumulative byte offset;
            // return the address of the sub-object so chained field access /
            // method calls keep working on the embedded data.
            if (field_type && field_type->kind == ast::TypeKind::Struct) {
                const uint32_t off = new_reg();
                emit(IRLoadConst{ off, static_cast<int64_t>(offset) });
                const uint32_t dst = new_reg();
                emit(IRBinary{ IRBinaryOp::Add, dst, base, off, ast::TypeKind::U64 });
                return dst;
            }

            const uint32_t dst = new_reg();
            emit(IRGetField{
                dst,
                base,
                offset
            });

            return dst;
        },

        [&](const ast::CallExpr& node) -> uint32_t {
            // Lower a method call: resolve <struct>::<method>, pass the receiver
            // (the struct's address) as the first argument, honor sret, and
            // coerce the user arguments to the declared parameter types.
            auto emit_method_call = [&](const std::string& struct_name,
                                        const std::string& method_name,
                                        uint32_t self_reg,
                                        bool have_self) -> uint32_t {
                std::vector<std::string> method_path = support::split_path(struct_name);
                method_path.back() = method_path.back() + "::" + method_name;

                const uint32_t func_id = resolve_function_id(method_path);

                std::vector<uint32_t> args;
                args.reserve(node.args.size() + 2);
                if (have_self) {
                    args.push_back(self_reg);
                }

                bool is_sret_call = false;
                uint32_t sret_ptr = 0;
                {
                    const ast::Type* ret_type = nullptr;
                    if (auto* fn_sym = resolve_qualified(ctx, method_path)) {
                        if (auto* fs = std::get_if<quant::symb_t::FuncSymbol>(&fn_sym->data)) {
                            ret_type = fs->return_type;
                        }
                    }
                    if (ret_type && ret_type->kind == ast::TypeKind::Struct) {
                        is_sret_call = true;
                        int sz = type_size(ret_type, &ctx);
                        if (current_func) current_func->extra_stack += sz;
                        sret_ptr = new_reg();
                        emit(IRAlloca{ sret_ptr, static_cast<uint32_t>(current_func ? current_func->extra_stack : 0) });
                        args.push_back(sret_ptr);
                    }
                }

                for (size_t i = 0; i < node.args.size(); ++i) {
                    const auto* arg = node.args[i];
                    if (!arg) {
                        ctx.errors.add("Null call argument"); return 0;
                    }
                    const ast::Type* param_type = nullptr;
                    if (auto* fn_sym = resolve_qualified(ctx, method_path)) {
                        if (auto* fs = std::get_if<quant::symb_t::FuncSymbol>(&fn_sym->data)) {
                            if (i < fs->arg_types.size()) param_type = fs->arg_types[i];
                        }
                    }
                    args.push_back(gen_expr_as(*arg, param_type));
                }

                const uint32_t dst = new_reg();
                emit(IRCall{ dst, func_id, args, is_sret_call });
                return is_sret_call ? sret_ptr : dst;
            };

            // Method call: <struct expr>.<method>(args).
            if (node.callee) {
                if (const auto* mf = std::get_if<ast::FieldExpr>(&node.callee->kind)) {
                    const ast::Type* base_type = mf->base ? mf->base->resolved_type : nullptr;
                    if (base_type) {
                        if (base_type->kind == ast::TypeKind::Reference && base_type->pointed) {
                            base_type = base_type->pointed;
                        }
                        if (base_type->kind == ast::TypeKind::Pointer && base_type->pointed) {
                            base_type = base_type->pointed;
                        }
                    }

                    if (!base_type || base_type->kind != ast::TypeKind::Struct) {
                        ctx.errors.add("Method call on non-struct type: " + mf->field); return 0;
                    }

                    const uint32_t self_reg = gen_expr(*mf->base);
                    return emit_method_call(base_type->struct_name, mf->field, self_reg, true);
                }
            }

            if (!region_stack.empty() && node.callee) {
                auto callee_path = support::flatten_path(node.callee);
                if (callee_path.size() == 1 && callee_path[0] == "alloc") {
                    if (node.args.size() == 2) {
                        // alloc(T, count) — typed allocation
                        const auto* type_expr = std::get_if<ast::TypeExpr>(&node.args[0]->kind);
                        if (!type_expr || !type_expr->type) {
                            ctx.errors.add("First argument to alloc must be a type, e.g. alloc(i32, 10)"); return 0;
                        }
                        uint32_t elem_size = type_size(type_expr->type, &ctx);
                        if (elem_size == 0) {
                            ctx.errors.add("Cannot allocate element of unknown size"); return 0;
                        }
                        const uint32_t count = gen_expr(*node.args[1]);
                        const uint32_t elem_reg = new_reg();
                        emit(IRLoadConst{elem_reg, static_cast<int64_t>(elem_size)});
                        const uint32_t total = new_reg();
                        emit(IRBinary{IRBinaryOp::Mul, total, count, elem_reg, ast::TypeKind::U64});

                        const uint32_t dst = new_reg();
                        const auto& ri = region_stack.back();
                        emit(IRRegionAlloc{dst, total, ri.region_local});
                        return dst;
                    }

                    if (node.args.size() != 1 || !node.args[0]) {
                        ctx.errors.add("alloc takes 1 argument (bytes) or 2 arguments (type, count)"); return 0;
                    }
                    const uint32_t size = gen_expr(*node.args[0]);
                    const uint32_t dst = new_reg();
                    const auto& ri = region_stack.back();
                    emit(IRRegionAlloc{dst, size, ri.region_local});
                    return dst;
                }
            }

            if (!node.callee) {
                ctx.errors.add("Call callee is missing"); return 0;
            }

            const auto callee_path = support::flatten_path(node.callee);

            // A bare call inside a method body with no matching free function
            // is a call to a sibling method of the current struct (implicit
            // receiver). Mirrors the semantic pass.
            if (!current_method_struct_name.empty()
                    && node.type_args.empty()
                    && node.resolved_mangled_name.empty()
                    && callee_path.size() == 1) {
                const auto* existing = resolve_qualified(ctx, callee_path);
                const bool is_free_func = existing
                    && std::get_if<quant::symb_t::FuncSymbol>(&existing->data);
                if (!is_free_func
                        && struct_has_method(ctx, current_method_struct_name, callee_path[0])) {
                    uint32_t self_local = 0;
                    const ast::Type* self_type = nullptr;
                    if (lookup_local("self", self_local, self_type)) {
                        const uint32_t self_ptr = new_reg();
                        emit(IRLoadLocal{ self_ptr, self_local });
                        return emit_method_call(current_method_struct_name, callee_path[0], self_ptr, true);
                    }
                }
            }

            // Handle generic function calls: use mangled name. The mangling
            // must include the full module path to match the semantic pass's
            // instantiation name (e.g. "gm::g<i32>" -> "gm::g$4").
            std::string mangled;
            if (!node.type_args.empty()) {
                std::vector<const ast::Type*> resolved_type_args;
                for (const auto* arg : node.type_args) {
                    const ast::Type* a = arg;
                    if (current_type_subst) {
                        a = ctx.types.substitute_type(a, *current_type_subst);
                    }
                    resolved_type_args.push_back(a);
                }
                mangled = ctx.types.mangle_func_name(support::join_namespace(callee_path), resolved_type_args);
            } else if (!node.resolved_mangled_name.empty()) {
                // Implicit generic call: the semantic pass resolved the
                // concrete (mangled) instantiation name for us.
                mangled = node.resolved_mangled_name;
            }
            const bool is_generic_call = !node.type_args.empty() || !node.resolved_mangled_name.empty();

            std::vector<uint32_t> args;
            args.reserve(node.args.size());

            for (size_t i = 0; i < node.args.size(); ++i) {
                const auto* arg = node.args[i];
                if (!arg) {
                    ctx.errors.add("Null call argument"); return 0;
                }
                const ast::Type* param_type = nullptr;
                if (is_generic_call) {
                    auto it = ctx.generic_arg_types.find(mangled);
                    if (it != ctx.generic_arg_types.end() && i < it->second.size()) {
                        param_type = it->second[i];
                    }
                } else if (const auto* fn_sym = resolve_qualified(ctx, callee_path)) {
                    if (auto* fs = std::get_if<quant::symb_t::FuncSymbol>(&fn_sym->data)) {
                        if (i < fs->arg_types.size()) param_type = fs->arg_types[i];
                    }
                }
                args.push_back(gen_expr_as(*arg, param_type));
            }

            uint32_t func_id = 0;

            if (is_generic_call) {
                func_id = resolve_function_id({mangled});
            } else {
                func_id = resolve_function_id(callee_path);
            }

            // Check if callee returns a struct (needs sret). Generic calls
            // have no symbol under the un-mangled name; use the concrete
            // (mangled) instantiation return type recorded by the semantic pass.
            bool is_sret_call = false;
            uint32_t sret_ptr = 0;
            {
                const ast::Type* ret_type = nullptr;
                if (is_generic_call) {
                    auto it = ctx.generic_return_types.find(mangled);
                    if (it != ctx.generic_return_types.end()) ret_type = it->second;
                } else if (auto* fn_sym = resolve_qualified(ctx, callee_path)) {
                    auto* fs = std::get_if<quant::symb_t::FuncSymbol>(&fn_sym->data);
                    if (fs) ret_type = fs->return_type;
                }
                if (ret_type && ret_type->kind == ast::TypeKind::Struct) {
                        is_sret_call = true;
                        int sz = type_size(ret_type, &ctx);
                        if (current_func) current_func->extra_stack += sz;
                        sret_ptr = new_reg();
                        emit(IRAlloca{ sret_ptr, static_cast<uint32_t>(current_func ? current_func->extra_stack : 0) });
                        args.insert(args.begin(), sret_ptr);
                    }
            }

            const uint32_t dst = new_reg();
            emit(IRCall{
                dst,
                func_id,
                args,
                is_sret_call
            });

            if (is_sret_call) {
                return sret_ptr;
            }
            return dst;
        },

        [&](const ast::NamespaceExpr& node) -> uint32_t {
            auto tmp = ast::Expr{ node };
            auto path = support::flatten_path(&tmp);
            auto* sym = resolve_qualified(ctx, path);
            if (sym) {
                if (auto* vs = std::get_if<symb_t::VarSymbol>(&sym->data)) {
                    if (vs->const_value.has_value()) {
                        const uint32_t dst = new_reg();
                        emit(IRLoadConst{ dst, static_cast<int32_t>(*vs->const_value) });
                        return dst;
                    }
                }
            }
            ctx.errors.add("Namespace expressions are not supported in IR generation yet"); return 0;
        },

        [&](const ast::CastExpr& node) -> uint32_t {
            const uint32_t src = gen_expr(*node.value);
            const uint32_t dst = new_reg();

            ast::TypeKind src_kind = ast::TypeKind::Void;
            if (node.value->resolved_type) {
                src_kind = node.value->resolved_type->kind;
            }

            emit(IRCast{
                dst,
                src,
                src_kind,
                node.target->kind,
                node.kind
            });
            return dst;
        },

        [&](const ast::StructInitExpr& node) -> uint32_t {
            const ast::Type* struct_type = expr.resolved_type;
            if (!struct_type || struct_type->kind != ast::TypeKind::Struct) {
                ctx.errors.add("StructInitExpr: invalid or missing struct type"); return 0;
            }

            int sz = type_size(struct_type, &ctx);
            if (sz <= 0) {
                ctx.errors.add("StructInitExpr: zero-size struct"); return 0;
            }

            // Allocate stack space for the struct
            if (current_func) current_func->extra_stack += sz;
            const uint32_t ptr = new_reg();
            emit(IRAlloca{ ptr, static_cast<uint32_t>(current_func ? current_func->extra_stack : 0) });

            // Initialize each field by position
            auto* sym = lookup_struct(ctx, struct_type->struct_name);
            const std::vector<const ast::Type*>* field_types = nullptr;
            if (sym) {
                if (auto* ss = std::get_if<quant::symb_t::StructSymbol>(&sym->data)) {
                    field_types = &ss->field_types;
                }
            }
            for (size_t i = 0; i < node.args.size(); ++i) {
                const ast::Type* field_type = (field_types && i < field_types->size()) ? (*field_types)[i] : nullptr;
                const uint32_t val = gen_expr_as(*node.args[i], field_type);
                quant::symb_t::StructSymbol* ss_target =
                    sym ? std::get_if<quant::symb_t::StructSymbol>(&sym->data) : nullptr;
                const uint32_t foff = struct_base_offset(ss_target, i, ctx);
                if (field_type && field_type->kind == ast::TypeKind::Struct) {
                    const auto flat = struct_flat_offsets(field_type, ctx);
                    for (uint32_t sub : flat) {
                        const uint32_t src_val = new_reg();
                        emit(IRGetField{src_val, val, sub});
                        emit(IRSetField{ptr, src_val, static_cast<uint32_t>(foff + sub)});
                    }
                } else {
                    emit(IRSetField{ ptr, val, foff });
                }
            }

            return ptr;
        },

        [&](const ast::SizeofExpr& node) -> uint32_t {
            if (node.type->kind == TypeKind::Struct && !node.type->type_args.empty()) {
                ctx.types.try_instantiate(node.type->struct_name, node.type->type_args);
            }
            int sz = ctx.types.type_size(node.type);
            if (sz <= 0) {
                ctx.errors.add("sizeof: zero or unknown size"); return 0;
            }
            const uint32_t dst = new_reg();
            emit(IRLoadConst{ dst, static_cast<int64_t>(sz) });
            return dst;
        },

        [&](const ast::TypeExpr&) -> uint32_t {
            ctx.errors.add("Type used as value in IR generation"); return 0;
        },

        [&](const ast::IndexExpr& node) -> uint32_t {
            const uint32_t base = gen_expr(*node.base);
            const uint32_t idx = gen_expr(*node.index);
            const uint32_t dst = new_reg();

            const ast::Type* base_type = node.base->resolved_type;
            // Auto-deref references
            if (base_type && base_type->kind == ast::TypeKind::Reference && base_type->pointed) {
                base_type = base_type->pointed;
            }
            if (!base_type || base_type->kind != ast::TypeKind::Pointer || !base_type->pointed) {
                ctx.errors.add("Invalid pointer index in IR gen"); return 0;
            }

            const ast::Type* elem_type = base_type->pointed;
            uint32_t elem_size = type_size(elem_type, &ctx);

            if (elem_type && elem_type->kind == ast::TypeKind::Struct) {
                const uint32_t sz = new_reg();
                emit(IRLoadConst{sz, static_cast<int64_t>(elem_size)});
                const uint32_t scaled = new_reg();
                emit(IRBinary{IRBinaryOp::Mul, scaled, idx, sz, ast::TypeKind::U64});
                const uint32_t addr = new_reg();
                emit(IRBinary{IRBinaryOp::Add, addr, base, scaled, ast::TypeKind::U64});
                return addr;
            }

            emit(IRLoadElement{dst, base, idx, elem_size});
            return dst;
        }
    }, expr.kind);
}

Reg IRGenerator::gen_expr_as(const ast::Expr& expr, const ast::Type* target) {
    const uint32_t v = gen_expr(expr);
    if (!target) return v;

    const ast::Type* src = expr.resolved_type;
    if (!src || src->kind == target->kind) return v;

    const bool src_num = is_numeric_kind(src->kind);
    const bool dst_num = is_numeric_kind(target->kind);
    if (!src_num || !dst_num) return v;

    const uint32_t c = new_reg();
    emit(IRCast{ c, v, src->kind, target->kind, ast::CastKind::ValueCast });
    return c;
}

} // namespace quant::codegen
