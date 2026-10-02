#include <iostream>
#include <variant>
#include "quant/ir/ir.h"

namespace quant::codegen {

namespace {

template<class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

template<class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

const char* type_name(ast::TypeKind kind) {
    switch (kind) {
        case ast::TypeKind::Void:      return "void";
        case ast::TypeKind::Bool:      return "bool";
        case ast::TypeKind::I8:        return "i8";
        case ast::TypeKind::I16:       return "i16";
        case ast::TypeKind::I32:       return "i32";
        case ast::TypeKind::I64:       return "i64";
        case ast::TypeKind::U8:        return "u8";
        case ast::TypeKind::U16:       return "u16";
        case ast::TypeKind::U32:       return "u32";
        case ast::TypeKind::U64:       return "u64";
        case ast::TypeKind::F32:       return "f32";
        case ast::TypeKind::F64:       return "f64";
        case ast::TypeKind::Struct:    return "struct";
        case ast::TypeKind::Pointer:   return "ptr";
        case ast::TypeKind::Reference: return "ref";
        case ast::TypeKind::NullPtr:   return "nullptr";
        case ast::TypeKind::Generic:   return "generic";
        default:                       return "?";
    }
}

const char* binop_name(IRBinaryOp op) {
    switch (op) {
        case IRBinaryOp::Add:      return "add";
        case IRBinaryOp::Sub:      return "sub";
        case IRBinaryOp::Mul:      return "mul";
        case IRBinaryOp::Div:      return "div";
        case IRBinaryOp::Eq:       return "eq";
        case IRBinaryOp::NotEq:    return "neq";
        case IRBinaryOp::Lt:       return "lt";
        case IRBinaryOp::Lte:      return "lte";
        case IRBinaryOp::Gt:       return "gt";
        case IRBinaryOp::Gte:      return "gte";
        case IRBinaryOp::BitAnd:   return "bitand";
        case IRBinaryOp::BitOr:    return "bitor";
        case IRBinaryOp::LogicAnd: return "and";
        case IRBinaryOp::LogicOr:  return "or";
        default:                   return "?";
    }
}

void dump_strings(const IRProgram& program) {
    std::cout << "strings:\n";
    if (program.strings.empty()) {
        std::cout << "  (none)\n";
    }
    for (const auto& s : program.strings) {
        std::cout << "  str_" << s.id << ": \"" << s.value << "\"\n";
    }
}

void dump_globals(const IRProgram& program) {
    std::cout << "globals:\n";
    if (program.globals.empty()) {
        std::cout << "  (none)\n";
    }
    for (uint32_t i = 0; i < program.globals.size(); ++i) {
        const auto& g = program.globals[i];
        std::cout << "  gbl_" << i << ": " << g.name
                  << " (size=" << g.size << ")\n";
    }
}

void dump_inst(const IRProgram& program, const IRInst& inst) {
    std::visit(overloaded{
        [&](const IRLoadConst& x) {
            std::cout << "const r" << x.dst << ", " << x.value;
        },
        [&](const IRLoadFloatConst& x) {
            std::cout << "float r" << x.dst << ", " << x.value
                      << " (" << (x.kind == ast::TypeKind::F32 ? "f32" : "f64") << ")";
        },
        [&](const IRLoadString& x) {
            std::cout << "loadstr r" << x.dst << ", str_" << x.string_id;
        },
        [&](const IRLoadLocal& x) {
            std::cout << "loadlocal r" << x.dst << ", L" << x.local;
        },
        [&](const IRStoreLocal& x) {
            std::cout << "storelocal L" << x.local << ", r" << x.src;
        },
        [&](const IRAddrOf& x) {
            std::cout << "addrof r" << x.dst << ", L" << x.local;
        },
        [&](const IRBinary& x) {
            std::cout << binop_name(x.op) << " r" << x.dst
                      << ", r" << x.lhs << ", r" << x.rhs
                      << " [" << type_name(x.type_kind) << "]";
        },
        [&](const IRCall& x) {
            const std::string name =
                (x.func_id < program.functions.size())
                    ? program.functions[x.func_id].name
                    : "<invalid>";
            std::cout << "call r" << x.dst << ", fn_" << x.func_id
                      << " (" << name << ") (";
            for (size_t i = 0; i < x.args.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << "r" << x.args[i];
            }
            std::cout << ")";
            if (x.sret) std::cout << " [sret]";
        },
        [&](const IRCast& x) {
            const char* kind = x.kind == ast::CastKind::Bitcast ? "bitcast"
                             : x.kind == ast::CastKind::Format  ? "format"
                                                               : "valuecast";
            std::cout << "cast r" << x.dst << ", r" << x.src
                      << " (" << type_name(x.src_kind)
                      << " -> " << type_name(x.target_kind)
                      << ", " << kind
                      << ")";
        },
        [&](const IRReturn& x) {
            std::cout << "return r" << x.value;
        },
        [&](const IRJump& x) {
            std::cout << "jump L" << x.target;
        },
        [&](const IRBranch& x) {
            std::cout << "branch r" << x.cond
                      << ", L" << x.then_label
                      << ", L" << x.else_label;
        },
        [&](const IRLabel& x) {
            std::cout << "L" << x.id << ":";
        },
        [&](const IRGetField& x) {
            std::cout << "getfield r" << x.dst << ", r" << x.base
                      << ", " << x.offset;
        },
        [&](const IRSetField& x) {
            std::cout << "setfield r" << x.base << ", r" << x.value
                      << ", " << x.offset;
        },
        [&](const IRLoadElement& x) {
            std::cout << "loadelem r" << x.dst << ", r" << x.base
                      << ", r" << x.index << ", " << x.elem_size;
        },
        [&](const IRStoreElement& x) {
            std::cout << "storeelem r" << x.base << ", r" << x.index
                      << ", r" << x.value << ", " << x.elem_size;
        },
        [&](const IRRegionBegin& x) {
            std::cout << "region_begin L" << x.region_local
                      << ", " << x.region_size;
        },
        [&](const IRRegionAlloc& x) {
            std::cout << "region_alloc r" << x.dst << ", r" << x.size
                      << ", L" << x.region_local;
        },
        [&](const IRRegionEnd& x) {
            std::cout << "region_end L" << x.region_local;
        },
        [&](const IRAlloca& x) {
            std::cout << "alloca r" << x.dst << ", " << x.offset;
        },
        [&](const IRLoadGlobal& x) {
            std::cout << "loadglobal r" << x.dst << ", gbl_" << x.global_id;
        },
        [&](const IRStoreGlobal& x) {
            std::cout << "storeglobal gbl_" << x.global_id << ", r" << x.src;
        },
        [&](const IRLoadGlobalAddr& x) {
            std::cout << "loadglobaladdr r" << x.dst << ", gbl_" << x.global_id;
        },
    }, inst);
}

void dump_function(const IRProgram& program, const IRFunction& fn) {
    std::cout << "fn_" << fn.id << ": " << fn.name << "\n";

    std::cout << "  args=" << fn.arg_count
              << ", locals=" << fn.local_count
              << ", temps=" << fn.temp_count
              << ", extra_stack=" << fn.extra_stack;

    if (fn.is_extern) {
        std::cout << ", extern";
        if (fn.syscall_number >= 0) {
            std::cout << ", syscall=" << fn.syscall_number;
        }
    }
    if (fn.is_entry) std::cout << ", entry";
    if (fn.sret) std::cout << ", sret";
    if (!fn.export_name.empty()) std::cout << ", export=\"" << fn.export_name << "\"";
    if (!fn.import_dll.empty()) std::cout << ", import=\"" << fn.import_dll << "\"";
    if (!fn.import_name.empty()) std::cout << ", import_name=\"" << fn.import_name << "\"";
    std::cout << "\n";

    if (fn.is_extern && fn.body.empty()) {
        return;
    }

    for (size_t i = 0; i < fn.body.size(); ++i) {
        std::cout << "    " << i << ": ";
        dump_inst(program, fn.body[i]);
        std::cout << "\n";
    }
}

} // namespace

void IRProgram::dump() const {
    std::cout << "=== IR dump ===\n\n";

    dump_strings(*this);
    std::cout << "\n";

    dump_globals(*this);
    std::cout << "\n";

    std::cout << "functions:\n";
    if (functions.empty()) {
        std::cout << "  (none)\n";
    }
    for (const auto& fn : functions) {
        dump_function(*this, fn);
    }
}

} // namespace quant::codegen
