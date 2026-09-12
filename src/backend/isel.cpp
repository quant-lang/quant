#include <cstring>

#include "quant/backend/isel.h"
#include "utils/logger.h"

namespace quant::codegen {

namespace {

template<class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

template<class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

int64_t double_bits(double d) {
    int64_t bits = 0;
    std::memcpy(&bits, &d, sizeof(bits));
    return bits;
}

int32_t float_bits(float f) {
    int32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

int cast_type_size(ast::TypeKind kind) {
    switch (kind) {
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
        case ast::TypeKind::U64:
        case ast::TypeKind::Pointer:
        case ast::TypeKind::Reference:
        case ast::TypeKind::NullPtr: return 8;
        default: return 0;
    }
}

bool is_signed_int(ast::TypeKind kind) {
    switch (kind) {
        case ast::TypeKind::I8: case ast::TypeKind::I16:
        case ast::TypeKind::I32: case ast::TypeKind::I64:
            return true;
        default: return false;
    }
}

bool is_integer(ast::TypeKind kind) {
    switch (kind) {
        case ast::TypeKind::I8: case ast::TypeKind::I16:
        case ast::TypeKind::I32: case ast::TypeKind::I64:
        case ast::TypeKind::U8: case ast::TypeKind::U16:
        case ast::TypeKind::U32: case ast::TypeKind::U64:
            return true;
        default: return false;
    }
}

bool is_float(ast::TypeKind kind) {
    return kind == ast::TypeKind::F32 || kind == ast::TypeKind::F64;
}

} // namespace

std::string ISel::asm_mangle(std::string name) {
    for (char& ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (!(std::isalnum(c) || ch == '_')) {
            ch = '_';
        }
    }
    return name;
}

std::string ISel::function_name(const IRFunction& fn) {
    if (!fn.export_name.empty()) {
        return fn.export_name;
    }
    return "qk_" + asm_mangle(fn.name);
}

std::string ISel::abi_name(const IRFunction& fn) {
    if (fn.is_extern) {
        return "qk_" + asm_mangle(fn.name);
    }
    return function_name(fn);
}

std::string ISel::string_label(uint32_t id) {
    return "str_" + std::to_string(id);
}

std::string ISel::global_label(uint32_t id) {
    return "gbl_" + std::to_string(id);
}

std::size_t ISel::align16(std::size_t n) {
    return (n + 15u) & ~std::size_t(15u);
}

const IRFunction* ISel::find_entry(const IRProgram& program) {
    const IRFunction* fallback = nullptr;
    for (const auto& fn : program.functions) {
        if (fn.is_entry) {
            return &fn;
        }
        if (fn.name.ends_with("main")) {
            fallback = &fn;
        }
    }
    return fallback;
}

x86::Mem ISel::local_slot(Local l) {
    return x86::mem_base(x86::RBP, -static_cast<int64_t>((static_cast<std::size_t>(l) + 1u) * 8u));
}

x86::Mem ISel::temp_slot(Reg r, const IRFunction& fn) {
    const std::size_t base = static_cast<std::size_t>(fn.local_count);
    return x86::mem_base(x86::RBP, -static_cast<int64_t>((base + static_cast<std::size_t>(r) + 1u) * 8u));
}

uint32_t ISel::ensure_symbol(const std::string& name, mc::SymBind bind, mc::SymType type,
                             bool undefined, uint32_t section, uint64_t value, uint64_t size,
                             const std::string& import_dll, const std::string& import_name) {
    auto it = symbol_index.find(name);
    if (it != symbol_index.end()) {
        if (!undefined) {
            auto& s = obj.symbols[it->second];
            s.bind = bind;
            s.type = type;
            s.undefined = false;
            s.section = section;
            s.value = value;
            s.size = size;
        }
        return it->second;
    }
    mc::Symbol s;
    s.name = name;
    s.bind = bind;
    s.type = type;
    s.undefined = undefined;
    s.section = section;
    s.value = value;
    s.size = size;
    if (!import_dll.empty()) {
        s.import_dll = import_dll;
        s.import_name = import_name.empty() ? name : import_name;
    }
    obj.symbols.push_back(s);
    symbol_index[name] = static_cast<uint32_t>(obj.symbols.size() - 1);
    return static_cast<uint32_t>(obj.symbols.size() - 1);
}

uint32_t ISel::add_runtime_symbol(const std::string& name) {
    return ensure_symbol(name, mc::SymBind::Global, mc::SymType::Func, true, 0, 0, 0);
}

uint32_t ISel::add_import_symbol(const std::string& dll, const std::string& name) {
    return ensure_symbol(name, mc::SymBind::Global, mc::SymType::Func, true, 0, 0, 0, dll, name);
}

uint32_t ISel::add_func_symbol(const IRFunction& fn) {
    if (fn.is_extern) {
        if (!fn.import_dll.empty()) {
            return ensure_symbol(abi_name(fn), mc::SymBind::Global, mc::SymType::Func,
                                 true, 0, 0, 0, fn.import_dll,
                                 fn.import_name.empty() ? fn.name : fn.import_name);
        }
        return ensure_symbol(abi_name(fn), mc::SymBind::Global, mc::SymType::Func, true, 0, 0, 0);
    }
    return ensure_symbol(function_name(fn), mc::SymBind::Global, mc::SymType::Func,
                         false, 0, text.code.size(), 0);
}

void ISel::emit_load(Reg r, const IRFunction& fn) {
    text.mov_r64_mem(x86::RAX, temp_slot(r, fn));
    text.push_r64(x86::RAX);
}

void ISel::emit_store(Reg r, const IRFunction& fn) {
    text.pop_r64(x86::RAX);
    text.mov_mem_r64(temp_slot(r, fn), x86::RAX);
}

void ISel::emit_binop(const IRBinary& x, const IRFunction& fn) {
    if (is_float(x.type_kind)) {
        const bool is64 = (x.type_kind == ast::TypeKind::F64);

        if (is64) {
            text.movsd_r64_mem(0, temp_slot(x.lhs, fn));
            text.movsd_r64_mem(1, temp_slot(x.rhs, fn));
        } else {
            text.movss_r64_mem(0, temp_slot(x.lhs, fn));
            text.movss_r64_mem(1, temp_slot(x.rhs, fn));
        }

        switch (x.op) {
            case IRBinaryOp::Add:
            case IRBinaryOp::Sub:
            case IRBinaryOp::Mul:
            case IRBinaryOp::Div: {
                x86::SseOp op;
                switch (x.op) {
                    case IRBinaryOp::Add: op = x86::SseOp::Add; break;
                    case IRBinaryOp::Sub: op = x86::SseOp::Sub; break;
                    case IRBinaryOp::Mul: op = x86::SseOp::Mul; break;
                    default:              op = x86::SseOp::Div; break;
                }
                if (is64) text.arith_sd(op, 0, 1);
                else      text.arith_ss(op, 0, 1);
                if (is64) text.movsd_mem_r64(temp_slot(x.dst, fn), 0);
                else      text.movss_mem_r64(temp_slot(x.dst, fn), 0);
                return;
            }
            case IRBinaryOp::Eq:
            case IRBinaryOp::NotEq:
            case IRBinaryOp::Lt:
            case IRBinaryOp::Lte:
            case IRBinaryOp::Gt:
            case IRBinaryOp::Gte:
                if (is64) text.comisd(1, 0);
                else      text.comiss(1, 0);
                switch (x.op) {
                    case IRBinaryOp::Eq:    text.setcc(x86::COND_E, x86::RAX); break;
                    case IRBinaryOp::NotEq: text.setcc(x86::COND_NE, x86::RAX); break;
                    case IRBinaryOp::Lt:    text.setcc(x86::COND_B, x86::RAX); break;
                    case IRBinaryOp::Lte:   text.setcc(x86::COND_BE, x86::RAX); break;
                    case IRBinaryOp::Gt:    text.setcc(x86::COND_A, x86::RAX); break;
                    case IRBinaryOp::Gte:   text.setcc(x86::COND_AE, x86::RAX); break;
                    default: break;
                }
                text.movzx_r64_r8(x86::RAX, x86::RAX);
                text.mov_mem_r64(temp_slot(x.dst, fn), x86::RAX);
                return;
            default:
                break;
        }
    }

    emit_load(x.lhs, fn);
    emit_load(x.rhs, fn);

    text.pop_r64(x86::RBX);
    text.pop_r64(x86::RAX);

    switch (x.op) {
        case IRBinaryOp::Add:
            text.add_r64_r64(x86::RAX, x86::RBX);
            break;
        case IRBinaryOp::Sub:
            text.sub_r64_r64(x86::RAX, x86::RBX);
            break;
        case IRBinaryOp::Mul:
            text.imul_r64_r64(x86::RAX, x86::RBX);
            break;
        case IRBinaryOp::Div:
            if (is_signed_int(x.type_kind)) {
                text.cqo();
                text.idiv_r64(x86::RBX);
            } else {
                text.xor_r64_r64(x86::RDX, x86::RDX);
                text.div_r64(x86::RBX);
            }
            break;
        case IRBinaryOp::BitAnd:
            text.and_r64_r64(x86::RAX, x86::RBX);
            break;
        case IRBinaryOp::BitOr:
            text.or_r64_r64(x86::RAX, x86::RBX);
            break;
        case IRBinaryOp::LogicAnd:
            text.test_r64_r64(x86::RAX, x86::RAX);
            text.setcc(x86::COND_NE, x86::RAX);
            text.movzx_r64_r8(x86::RAX, x86::RAX);
            text.test_r64_r64(x86::RBX, x86::RBX);
            text.setcc(x86::COND_NE, x86::RBX);
            text.movzx_r64_r8(x86::RBX, x86::RBX);
            text.and_r64_r64(x86::RAX, x86::RBX);
            break;
        case IRBinaryOp::LogicOr:
            text.test_r64_r64(x86::RAX, x86::RAX);
            text.setcc(x86::COND_NE, x86::RAX);
            text.movzx_r64_r8(x86::RAX, x86::RAX);
            text.test_r64_r64(x86::RBX, x86::RBX);
            text.setcc(x86::COND_NE, x86::RBX);
            text.movzx_r64_r8(x86::RBX, x86::RBX);
            text.or_r64_r64(x86::RAX, x86::RBX);
            break;
        case IRBinaryOp::Eq:
        case IRBinaryOp::NotEq:
        case IRBinaryOp::Lt:
        case IRBinaryOp::Lte:
        case IRBinaryOp::Gt:
        case IRBinaryOp::Gte:
            text.cmp_r64_r64(x86::RAX, x86::RBX);
            {
                // Unsigned kinds (u8..u64, bool) must use the unsigned
                // condition codes, otherwise values >= 2^63 compare as negative.
                const bool sg = is_signed_int(x.type_kind);
                switch (x.op) {
                    case IRBinaryOp::Eq:    text.setcc(x86::COND_E, x86::RAX); break;
                    case IRBinaryOp::NotEq: text.setcc(x86::COND_NE, x86::RAX); break;
                    case IRBinaryOp::Lt:    text.setcc(sg ? x86::COND_L  : x86::COND_B,  x86::RAX); break;
                    case IRBinaryOp::Lte:   text.setcc(sg ? x86::COND_LE : x86::COND_BE, x86::RAX); break;
                    case IRBinaryOp::Gt:    text.setcc(sg ? x86::COND_G  : x86::COND_A,  x86::RAX); break;
                    case IRBinaryOp::Gte:   text.setcc(sg ? x86::COND_GE : x86::COND_AE, x86::RAX); break;
                    default: break;
                }
            }
            text.movzx_r64_r8(x86::RAX, x86::RAX);
            break;
    }

    text.mov_mem_r64(temp_slot(x.dst, fn), x86::RAX);
}

void ISel::emit_call(const IRProgram& program, const IRFunction& fn, const IRCall& x) {
    if (x.func_id >= program.functions.size()) {
        utils::logger::crash("IRCall references invalid function id: " + std::to_string(x.func_id));
    }

    const auto& callee = program.functions[x.func_id];
    const std::size_t n = x.args.size();
    const bool win_cc = (target_os == mc::TargetOS::Windows);

    const std::size_t max_reg = win_cc ? 4 : 6;
    const std::size_t shadow = win_cc ? 32 : 0;
    const std::size_t stack_count = (n > max_reg) ? (n - max_reg) : 0;
    std::size_t frame = shadow + stack_count * 8;
    if (win_cc && frame % 16 != 0) frame += 8;

    if (frame > 0) {
        text.sub_rsp_imm32(static_cast<uint32_t>(frame));
    }

    // Stack args: on Windows the first stack arg (5th) sits at [rsp+0x20]
    // relative to RSP at the call (above the 0x20 shadow space), on SysV the
    // 7th sits at [rsp+0]. The 16-byte alignment padding must not shift it.
    const std::size_t arg_base = shadow;
    for (std::size_t i = n; i > max_reg; --i) {
        const std::size_t idx = i - 1;
        const std::size_t slot = idx - max_reg;
        text.mov_r64_mem(x86::RAX, temp_slot(x.args[idx], fn));
        text.mov_mem_r64(x86::mem_base(x86::RSP, static_cast<int64_t>(arg_base + slot * 8)), x86::RAX);
    }

    const std::string callee_name = abi_name(callee);
    if (symbol_index.count(callee_name) == 0) {
        if (callee.is_extern) {
            add_func_symbol(callee);
        } else {
            add_runtime_symbol(callee_name);
        }
    }

    // register args
    if (win_cc) {
        static const x86::R64 win_regs[] = {x86::RCX, x86::RDX, x86::R8, x86::R9};
        for (std::size_t i = 0; i < n && i < max_reg; ++i) {
            text.mov_r64_mem(win_regs[i], temp_slot(x.args[i], fn));
        }
    } else {
        static const x86::R64 linux_regs[] = {x86::RDI, x86::RSI, x86::RDX, x86::RCX, x86::R8, x86::R9};
        for (std::size_t i = 0; i < n && i < max_reg; ++i) {
            text.mov_r64_mem(linux_regs[i], temp_slot(x.args[i], fn));
        }
    }

    if (callee.is_extern && !callee.import_dll.empty()) {
        // Imported function: call through its IAT slot (indirect).
        text.call_mem_rip(callee_name);
    } else {
        text.call_rel32(callee_name);
    }

    if (frame > 0) {
        text.add_rsp_imm32(static_cast<uint32_t>(frame));
    }

    if (!x.sret) {
        text.mov_mem_r64(temp_slot(x.dst, fn), x86::RAX);
    }
}

void ISel::emit_region_begin(const IRRegionBegin& x) {
    // Region layout: [data:8] [offset:8] [capacity:8]
    text.mov_r64_mem(x86::RBX, local_slot(x.region_local));
    if (target_os == mc::TargetOS::Windows) {
        // VirtualAlloc(0, size, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE)
        text.mov_r64_imm(x86::RCX, 0);
        text.mov_r64_imm(x86::RDX, x.region_size);
        text.mov_r64_imm(x86::R8, 0x3000);
        text.mov_r64_imm(x86::R9, 0x04);
        text.sub_rsp_imm32(32);
        text.call_mem_rip("VirtualAlloc");
        text.add_rsp_imm32(32);
    } else {
        // mmap(0, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
        text.mov_r64_imm(x86::RDI, 0);
        text.mov_r64_imm(x86::RSI, x.region_size);
        text.mov_r64_imm(x86::RDX, 3);
        text.mov_r64_imm(x86::R10, 0x22);
        text.mov_r64_imm(x86::R8, -1);
        text.mov_r64_imm(x86::R9, 0);
        text.mov_r64_imm(x86::RAX, 9);
        text.syscall();
    }
    text.mov_mem_r64(x86::mem_base(x86::RBX, 0), x86::RAX);
    text.mov_r64_imm(x86::RAX, 0);
    text.mov_mem_r64(x86::mem_base(x86::RBX, 8), x86::RAX);
    text.mov_r64_imm(x86::RAX, x.region_size);
    text.mov_mem_r64(x86::mem_base(x86::RBX, 16), x86::RAX);
}

void ISel::emit_region_alloc(const IRRegionAlloc& x, const IRFunction& fn) {
    const uint32_t ok_label = region_alloc_label();

    text.mov_r64_mem(x86::RAX, local_slot(x.region_local));
    text.mov_r64_mem(x86::RBX, x86::mem_base(x86::RAX, 0));
    text.add_r64_mem(x86::RBX, x86::mem_base(x86::RAX, 8));
    text.mov_mem_r64(temp_slot(x.dst, fn), x86::RBX);

    text.mov_r64_mem(x86::RAX, temp_slot(x.size, fn));
    text.add_r64_imm8(x86::RAX, 15);
    text.and_r64_imm8(x86::RAX, -16);

    text.mov_r64_mem(x86::RBX, local_slot(x.region_local));
    text.add_mem_r64(x86::mem_base(x86::RBX, 8), x86::RAX);

    text.mov_r64_mem(x86::RAX, x86::mem_base(x86::RBX, 8));
    text.cmp_r64_mem(x86::RAX, x86::mem_base(x86::RBX, 16));
    text.jcc_rel32(x86::COND_BE);
    fixups.push_back({static_cast<uint32_t>(text.code.size() - 4), ok_label, true});
    if (target_os == mc::TargetOS::Windows) {
        // ExitProcess(1) - region overflow
        add_import_symbol("kernel32.dll", "ExitProcess");
        text.mov_r64_imm(x86::RCX, 1);
        text.sub_rsp_imm32(32);
        text.call_mem_rip("ExitProcess");
    } else {
        // exit(1) inline - no qk_exit dependency
        text.mov_r64_imm(x86::RDI, 1);
        text.mov_r64_imm(x86::RAX, 60);
        text.syscall();
    }
    region_label_pos[ok_label] = text.code.size();
}

void ISel::emit_region_end(const IRRegionEnd& x) {
    text.mov_r64_mem(x86::RAX, local_slot(x.region_local));
    text.mov_r64_mem(x86::RDI, x86::mem_base(x86::RAX, 0));
    text.mov_r64_mem(x86::RSI, x86::mem_base(x86::RAX, 16));
    if (target_os == mc::TargetOS::Windows) {
        // VirtualFree(ptr, 0, MEM_RELEASE)
        text.mov_r64_mem(x86::RCX, x86::mem_base(x86::RAX, 0));
        text.mov_r64_imm(x86::RDX, 0);
        text.mov_r64_imm(x86::R8, 0x8000);
        text.sub_rsp_imm32(32);
        text.call_mem_rip("VirtualFree");
        text.add_rsp_imm32(32);
    } else {
        // munmap(ptr, size)
        text.mov_r64_imm(x86::RAX, 11);
        text.syscall();
    }
}

void ISel::emit_inst(const IRProgram& program, const IRFunction& fn, const IRInst& inst) {
    std::visit(overloaded{
        [&](const IRLoadConst& x) {
            text.mov_r64_imm(x86::RAX, x.value);
            text.mov_mem_r64(temp_slot(x.dst, fn), x86::RAX);
        },
        [&](const IRLoadFloatConst& x) {
            if (x.kind == ast::TypeKind::F32) {
                text.mov_r32_imm(x86::RAX, float_bits(static_cast<float>(x.value)));
                text.mov_mem_r32(temp_slot(x.dst, fn), x86::RAX);
            } else {
                text.mov_r64_imm(x86::RAX, double_bits(x.value));
                text.mov_mem_r64(temp_slot(x.dst, fn), x86::RAX);
            }
        },
        [&](const IRLoadLocal& x) {
            text.mov_r64_mem(x86::RAX, local_slot(x.local));
            text.mov_mem_r64(temp_slot(x.dst, fn), x86::RAX);
        },
        [&](const IRStoreLocal& x) {
            text.mov_r64_mem(x86::RAX, temp_slot(x.src, fn));
            text.mov_mem_r64(local_slot(x.local), x86::RAX);
        },
        [&](const IRAddrOf& x) {
            text.lea_r64_mem(x86::RAX, local_slot(x.local));
            text.mov_mem_r64(temp_slot(x.dst, fn), x86::RAX);
        },
        [&](const IRBinary& x) {
            emit_binop(x, fn);
        },
        [&](const IRCall& x) {
            emit_call(program, fn, x);
        },
        [&](const IRReturn& x) {
            text.mov_r64_mem(x86::RAX, temp_slot(x.value, fn));
            text.leave();
            text.ret();
        },
        [&](const IRJump& x) {
            text.jmp_rel32();
            fixups.push_back({static_cast<uint32_t>(text.code.size() - 4), x.target, false});
        },
        [&](const IRBranch& x) {
            text.mov_r64_mem(x86::RAX, temp_slot(x.cond, fn));
            text.test_r64_r64(x86::RAX, x86::RAX);
            text.jcc_rel32(x86::COND_NE);
            fixups.push_back({static_cast<uint32_t>(text.code.size() - 4), x.then_label, false});
            text.jmp_rel32();
            fixups.push_back({static_cast<uint32_t>(text.code.size() - 4), x.else_label, false});
        },
        [&](const IRLabel& x) {
            label_pos[x.id] = text.code.size();
        },
        [&](const IRGetField& x) {
            text.mov_r64_mem(x86::RAX, temp_slot(x.base, fn));
            text.mov_r64_mem(x86::RAX, x86::mem_base(x86::RAX, static_cast<int64_t>(x.offset)));
            text.mov_mem_r64(temp_slot(x.dst, fn), x86::RAX);
        },
        [&](const IRSetField& x) {
            text.mov_r64_mem(x86::RAX, temp_slot(x.base, fn));
            text.mov_r64_mem(x86::RBX, temp_slot(x.value, fn));
            text.mov_mem_r64(x86::mem_base(x86::RAX, static_cast<int64_t>(x.offset)), x86::RBX);
        },
        [&](const IRLoadString& x) {
            if (x.string_id >= program.strings.size()) {
                utils::logger::crash("IRLoadString references invalid string id: " + std::to_string(x.string_id));
            }
            const auto& lit = program.strings[x.string_id];
            text.lea_r64_rip(x86::RAX, string_label(lit.id));
            text.mov_mem_r64(temp_slot(x.dst, fn), x86::RAX);
        },
        [&](const IRLoadElement& x) {
            text.mov_r64_mem(x86::RAX, temp_slot(x.base, fn));
            text.mov_r64_mem(x86::RCX, temp_slot(x.index, fn));
            const x86::Mem m = x86::mem_index(x86::RAX, x86::RCX, static_cast<uint8_t>(x.elem_size));
            if (x.elem_size == 1) {
                text.movzx_r32_mem8(x86::RAX, m);
            } else if (x.elem_size == 2) {
                text.movzx_r32_mem16(x86::RAX, m);
            } else if (x.elem_size <= 4) {
                text.mov_r32_mem(x86::RAX, m);
            } else {
                text.mov_r64_mem(x86::RAX, m);
            }
            text.mov_mem_r64(temp_slot(x.dst, fn), x86::RAX);
        },
        [&](const IRStoreElement& x) {
            text.mov_r64_mem(x86::RAX, temp_slot(x.base, fn));
            text.mov_r64_mem(x86::RCX, temp_slot(x.index, fn));
            text.mov_r64_mem(x86::RBX, temp_slot(x.value, fn));
            const x86::Mem m = x86::mem_index(x86::RAX, x86::RCX, static_cast<uint8_t>(x.elem_size));
            if (x.elem_size == 1) {
                text.mov_mem_r8(m, x86::RBX);
            } else if (x.elem_size == 2) {
                text.mov_mem_r16(m, x86::RBX);
            } else if (x.elem_size <= 4) {
                text.mov_mem_r32(m, x86::RBX);
            } else {
                text.mov_mem_r64(m, x86::RBX);
            }
        },
        [&](const IRCast& x) {
            const x86::Mem src = temp_slot(x.src, fn);
            const x86::Mem dst = temp_slot(x.dst, fn);

            if (x.kind == ast::CastKind::Bitcast) {
                text.mov_r64_mem(x86::RAX, src);
                text.mov_mem_r64(dst, x86::RAX);
                return;
            }

            const int src_sz = cast_type_size(x.src_kind);
            const int dst_sz = cast_type_size(x.target_kind);
            const bool src_int = is_integer(x.src_kind);
            const bool dst_int = is_integer(x.target_kind);
            const bool src_flt = is_float(x.src_kind);
            const bool dst_flt = is_float(x.target_kind);

            if (x.target_kind == ast::TypeKind::String) {
                const x86::R64 int_arg_reg =
                    (target_os == mc::TargetOS::Windows) ? x86::RCX : x86::RDI;
                if (is_integer(x.src_kind)) {
                    const int src_sz = cast_type_size(x.src_kind);
                    if (is_signed_int(x.src_kind)) {
                        if (src_sz == 4)      text.movsxd_r64_mem32(int_arg_reg, src);
                        else if (src_sz == 2) text.movsx_r64_mem16(int_arg_reg, src);
                        else if (src_sz == 1) text.movsx_r64_mem8(int_arg_reg, src);
                        else                  text.mov_r64_mem(int_arg_reg, src);
                    } else {
                        if (src_sz == 4)      text.mov_r32_mem(int_arg_reg, src);
                        else if (src_sz == 2) text.movzx_r32_mem16(int_arg_reg, src);
                        else if (src_sz == 1) text.movzx_r32_mem8(int_arg_reg, src);
                        else                  text.mov_r64_mem(int_arg_reg, src);
                    }
                    if (is_signed_int(x.src_kind)) {
                        add_runtime_symbol("qk_format_i64");
                        text.call_rel32("qk_format_i64");
                    } else {
                        add_runtime_symbol("qk_format_u64");
                        text.call_rel32("qk_format_u64");
                    }
                } else if (x.src_kind == ast::TypeKind::F32) {
                    text.movss_r64_mem(0, src);
                    text.cvt_ss_to_sd_rr(0, 0);
                    text.movsd_mem_r64(dst, 0);
                    text.mov_r64_mem(int_arg_reg, dst);
                    add_runtime_symbol("qk_format_f64");
                    text.call_rel32("qk_format_f64");
                } else {
                    text.mov_r64_mem(int_arg_reg, src);
                    add_runtime_symbol("qk_format_f64");
                    text.call_rel32("qk_format_f64");
                }
                text.mov_mem_r64(dst, x86::RAX);
                return;
            }

            if (src_int && dst_int) {
                if (src_sz == dst_sz) {
                    text.mov_r64_mem(x86::RAX, src);
                } else if (src_sz < dst_sz) {
                    if (is_signed_int(x.src_kind)) {
                        if (src_sz == 4) {
                            text.movsxd_r64_mem32(x86::RAX, src);
                        } else if (src_sz == 2) {
                            text.movsx_r64_mem16(x86::RAX, src);
                        } else {
                            text.movsx_r64_mem8(x86::RAX, src);
                        }
                    } else {
                        if (src_sz == 4) {
                            text.mov_r32_mem(x86::RAX, src);
                        } else if (src_sz == 2) {
                            text.movzx_r32_mem16(x86::RAX, src);
                        } else {
                            text.movzx_r32_mem8(x86::RAX, src);
                        }
                    }
                } else {
                    if (is_signed_int(x.target_kind)) {
                        if (dst_sz == 4) {
                            text.movsxd_r64_mem32(x86::RAX, src);
                        } else if (dst_sz == 2) {
                            text.movsx_r64_mem16(x86::RAX, src);
                        } else {
                            text.movsx_r64_mem8(x86::RAX, src);
                        }
                    } else {
                        if (dst_sz == 4) {
                            text.mov_r32_mem(x86::RAX, src);
                        } else if (dst_sz == 2) {
                            text.movzx_r32_mem16(x86::RAX, src);
                        } else {
                            text.movzx_r32_mem8(x86::RAX, src);
                        }
                    }
                }
                text.mov_mem_r64(dst, x86::RAX);

            } else if (src_int && dst_flt) {
                if (dst_sz == 4) {
                    if (src_sz == 4) {
                        text.cvt_i32_to_ss(0, src);
                    } else {
                        text.cvt_i64_to_ss(0, src);
                    }
                    text.movss_mem_r64(dst, 0);
                } else {
                    if (src_sz == 4) {
                        text.cvt_i32_to_sd(0, src);
                    } else {
                        text.cvt_i64_to_sd(0, src);
                    }
                    text.movsd_mem_r64(dst, 0);
                }

            } else if (src_flt && dst_int) {
                if (src_sz == 4) {
                    text.cvt_ss_to_i64(x86::RAX, src);
                } else {
                    text.cvt_sd_to_i64(x86::RAX, src);
                }
                text.mov_mem_r64(dst, x86::RAX);

            } else if (src_flt && dst_flt) {
                if (src_sz == 4 && dst_sz == 8) {
                    text.cvt_ss_to_sd(0, src);
                    text.movsd_mem_r64(dst, 0);
                } else if (src_sz == 8 && dst_sz == 4) {
                    text.cvt_sd_to_ss(0, src);
                    text.movss_mem_r64(dst, 0);
                } else {
                    text.mov_r64_mem(x86::RAX, src);
                    text.mov_mem_r64(dst, x86::RAX);
                }

            } else {
                utils::logger::crash("Unsupported ValueCast in codegen");
            }
        },
        [&](const IRRegionBegin& x) {
            emit_region_begin(x);
        },
        [&](const IRRegionAlloc& x) {
            emit_region_alloc(x, fn);
        },
        [&](const IRRegionEnd& x) {
            emit_region_end(x);
        },
        [&](const IRAlloca& x) {
            const std::size_t base = (static_cast<std::size_t>(fn.local_count) +
                                      static_cast<std::size_t>(fn.temp_count)) * 8u;
            const std::size_t offset = base + static_cast<std::size_t>(x.offset);
            text.lea_r64_mem(x86::RAX, x86::mem_base(x86::RBP, -static_cast<int64_t>(offset)));
            text.mov_mem_r64(temp_slot(x.dst, fn), x86::RAX);
        },
        [&](const IRLoadGlobal& x) {
            text.mov_r64_mem_rip(x86::RAX, global_label(x.global_id));
            text.mov_mem_r64(temp_slot(x.dst, fn), x86::RAX);
        },
        [&](const IRStoreGlobal& x) {
            text.mov_r64_mem(x86::RAX, temp_slot(x.src, fn));
            text.mov_mem_r64_rip(x86::RAX, global_label(x.global_id));
        },
        [&](const IRLoadGlobalAddr& x) {
            text.lea_r64_rip(x86::RAX, global_label(x.global_id));
            text.mov_mem_r64(temp_slot(x.dst, fn), x86::RAX);
        },
    }, inst);
}

void ISel::emit_prologue(const IRFunction& fn) {
    const std::size_t stack_size = align16(
        (static_cast<std::size_t>(fn.local_count) +
         static_cast<std::size_t>(fn.temp_count)) * 8u +
        static_cast<std::size_t>(fn.extra_stack));

    text.push_r64(x86::RBP);
    text.mov_r64_r64(x86::RBP, x86::RSP);

    if (stack_size > 0) {
        text.sub_rsp_imm32(static_cast<uint32_t>(stack_size));
    }

    if (target_os == mc::TargetOS::Windows) {
        static const x86::R64 regs[] = {x86::RCX, x86::RDX, x86::R8, x86::R9};
        for (uint32_t i = 0; i < fn.arg_count; ++i) {
            if (i < 4) {
                text.mov_mem_r64(local_slot(i), regs[i]);
            } else {
                // 5th arg arrives at [entry_rsp + 0x28] == [rbp + 0x30]
                text.mov_r64_mem(x86::RAX, x86::mem_base(x86::RBP, static_cast<int64_t>(48u + (i - 4u) * 8u)));
                text.mov_mem_r64(local_slot(i), x86::RAX);
            }
        }
    } else {
        static const x86::R64 regs[] = {x86::RDI, x86::RSI, x86::RDX, x86::RCX, x86::R8, x86::R9};
        for (uint32_t i = 0; i < fn.arg_count; ++i) {
            if (i < 6) {
                text.mov_mem_r64(local_slot(i), regs[i]);
            } else {
                text.mov_r64_mem(x86::RAX, x86::mem_base(x86::RBP, static_cast<int64_t>(16u + (i - 6u) * 8u)));
                text.mov_mem_r64(local_slot(i), x86::RAX);
            }
        }
    }
}

void ISel::emit_func(const IRProgram& program, const IRFunction& fn) {
    add_func_symbol(fn);

    emit_prologue(fn);

    for (const auto& inst : fn.body) {
        emit_inst(program, fn, inst);
    }
}

void ISel::emit_syscall_stub(const IRFunction& fn) {
    ensure_symbol(abi_name(fn), mc::SymBind::Global, mc::SymType::Func, false, 0, text.code.size(), 0);
    // System V args arrive in rdi,rsi,rdx,rcx,r8,r9; a raw syscall wants
    // rdi,rsi,rdx,r10,r8,r9 and the number in rax.
    text.mov_r64_r64(x86::R10, x86::RCX);
    text.mov_r64_imm(x86::RAX, fn.syscall_number);
    text.syscall();
    text.ret();
}

void ISel::emit_start(const IRProgram& program) {
    ensure_symbol("_start", mc::SymBind::Global, mc::SymType::Func, false, 0, text.code.size(), 0);
    const IRFunction* entry = find_entry(program);
    if (!entry) {
        utils::logger::crash("No entry point found");
    }
    if (target_os == mc::TargetOS::Windows) {
        text.sub_rsp_imm32(8);
        if (symbol_index.count("qk_io_init") != 0) {
            text.call_rel32("qk_io_init");
        }
        text.call_rel32(function_name(*entry));

        text.mov_r64_r64(x86::RCX, x86::RAX);
        text.sub_rsp_imm32(32);
        text.call_mem_rip("ExitProcess");
    } else {
        text.call_rel32(function_name(*entry));
        text.mov_r64_r64(x86::RDI, x86::RAX);
        text.mov_r64_imm(x86::RAX, 60);
        text.syscall();
    }
}

void ISel::emit_strings(const IRProgram& program) {
    for (const auto& s : program.strings) {
        ensure_symbol(string_label(s.id), mc::SymBind::Local, mc::SymType::Object,
                      false, 1, obj.data.size(), s.value.size() + 1);
        obj.data.insert(obj.data.end(), s.value.begin(), s.value.end());
        obj.data.push_back(0);
    }
}

void ISel::emit_globals(const IRProgram& program) {
    for (uint32_t i = 0; i < program.globals.size(); ++i) {
        const auto& g = program.globals[i];
        if (g.is_extern) continue;
        while (obj.data.size() % 8u != 0) {
            obj.data.push_back(0);
        }
        ensure_symbol(global_label(i), mc::SymBind::Local, mc::SymType::Object,
                      false, 1, obj.data.size(), g.size);
        for (uint32_t j = 0; j < g.size; ++j) {
            obj.data.push_back(0);
        }
    }
}

void ISel::patch_fixups() {
    for (const auto& f : fixups) {
        const uint32_t target = f.region ? region_label_pos.at(f.key) : label_pos.at(f.key);
        const int32_t disp = static_cast<int32_t>(target - (f.field_offset + 4));
        for (int i = 0; i < 4; ++i) {
            text.code[f.field_offset + static_cast<uint32_t>(i)] =
                static_cast<uint8_t>((disp >> (8 * i)) & 0xFF);
        }
    }
}

void ISel::generate(const IRProgram& program) {
    obj.text.clear();
    obj.data.clear();
    obj.relocs.clear();
    obj.symbols.clear();
    symbol_index.clear();
    label_pos.clear();
    region_label_pos.clear();
    fixups.clear();
    text.code.clear();
    text.relocs.clear();
    next_region_label = 0;

    for (const auto& fn : program.functions) {
        if (fn.is_extern) {
            if (target_os == mc::TargetOS::Windows) {
                // Raw syscalls are not available on Windows: externs become
                // imports (or plain undefined symbols). @syscall is unsupported.
                add_func_symbol(fn);
            } else {
                if (fn.syscall_number >= 0) {
                    emit_syscall_stub(fn);
                } else {
                    add_func_symbol(fn);
                }
            }
            continue;
        }
        emit_func(program, fn);
    }

    if (target_os == mc::TargetOS::Windows) {
        add_import_symbol("kernel32.dll", "VirtualAlloc");
        add_import_symbol("kernel32.dll", "VirtualFree");
        add_import_symbol("kernel32.dll", "ExitProcess");
    }

    if(ctx.emit_start) emit_start(program);
    emit_strings(program);
    emit_globals(program);
    patch_fixups();

    obj.text = std::move(text.code);
    obj.relocs = std::move(text.relocs);
}

} // namespace quant::codegen
