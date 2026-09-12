#include <cstring>
#include <variant>

#include "quant/backend/aarch64_isel.h"
#include "quant/backend/mc.h"
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

// Map IR comparison ops to AArch64 condition codes. Unsigned kinds
// (u8..u64, bool) use the unsigned codes so values >= 2^63 do not compare
// as negative.
aarch64::Cond cmp_op_to_cond(IRBinaryOp op, bool is_signed) {
    switch (op) {
        case IRBinaryOp::Eq:    return aarch64::COND_EQ;
        case IRBinaryOp::NotEq: return aarch64::COND_NE;
        case IRBinaryOp::Lt:    return is_signed ? aarch64::COND_LT : aarch64::COND_CC;
        case IRBinaryOp::Lte:   return is_signed ? aarch64::COND_LE : aarch64::COND_LS;
        case IRBinaryOp::Gt:    return is_signed ? aarch64::COND_GT : aarch64::COND_HI;
        case IRBinaryOp::Gte:   return is_signed ? aarch64::COND_GE : aarch64::COND_CS;
        default:                return aarch64::COND_AL;
    }
}

aarch64::Cond float_cmp_op_to_cond(IRBinaryOp op) {
    switch (op) {
        case IRBinaryOp::Eq:    return aarch64::COND_EQ;
        case IRBinaryOp::NotEq: return aarch64::COND_NE;
        case IRBinaryOp::Lt:    return aarch64::COND_LT;  // FCMP sets NZCV; A < B => C=0 => CC
        case IRBinaryOp::Lte:   return aarch64::COND_LE;  // A <= B => C=1 or Z=1
        case IRBinaryOp::Gt:    return aarch64::COND_GT;  // A > B => C=1 and Z=0
        case IRBinaryOp::Gte:   return aarch64::COND_GE;  // A >= B => C=1
        default:                return aarch64::COND_AL;
    }
}

} // namespace

// Naming

std::string AArch64ISel::asm_mangle(std::string name) {
    for (char& ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (!(std::isalnum(c) || ch == '_')) ch = '_';
    }
    return name;
}

std::string AArch64ISel::function_name(const IRFunction& fn) {
    if (!fn.export_name.empty()) return fn.export_name;
    return "qk_" + asm_mangle(fn.name);
}

std::string AArch64ISel::abi_name(const IRFunction& fn) {
    if (fn.is_extern) return "qk_" + asm_mangle(fn.name);
    return function_name(fn);
}

std::string AArch64ISel::string_label(uint32_t id) {
    return "str_" + std::to_string(id);
}

std::string AArch64ISel::global_label(uint32_t id) {
    return "gbl_" + std::to_string(id);
}

std::size_t AArch64ISel::align16(std::size_t n) {
    return (n + 15u) & ~std::size_t(15u);
}

const IRFunction* AArch64ISel::find_entry(const IRProgram& program) {
    const IRFunction* fallback = nullptr;
    for (const auto& fn : program.functions) {
        if (fn.is_entry) return &fn;
        if (fn.name.ends_with("main")) fallback = &fn;
    }
    return fallback;
}

// Stack frame layout (AAPCS64)
//
// Frame pointer (X29) points to:
//   [FP]       = saved X29
//   [FP+8]     = saved X30 (LR)
//   [FP-8]     = local 0
//   [FP-16]    = local 1
//   ...
//   [FP-(S+1)*8]      = temp 0
//   [FP-(S+T)*8]      = temp T-1
//   [FP-(S+T)*8-8]    = extra_stack area (struct allocas)
// SP = FP - frame_size (16-byte aligned)
//
// S = number of callee-saved registers we save (0 for now).

int64_t AArch64ISel::local_offset(Local l, const IRFunction& fn) {
    return static_cast<int64_t>((static_cast<std::size_t>(l) + 2u) * 8u);
}

int64_t AArch64ISel::temp_offset(Reg r, const IRFunction& fn) {
    const std::size_t base = static_cast<std::size_t>(fn.local_count);
    return static_cast<int64_t>((base + static_cast<std::size_t>(r) + 2u) * 8u);
}

// Symbols

uint32_t AArch64ISel::ensure_symbol(const std::string& name, mc::SymBind bind,
                                     mc::SymType type, bool undefined,
                                     uint32_t section, uint64_t value, uint64_t size) {
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
    obj.symbols.push_back(s);
    symbol_index[name] = static_cast<uint32_t>(obj.symbols.size() - 1);
    return static_cast<uint32_t>(obj.symbols.size() - 1);
}

uint32_t AArch64ISel::add_runtime_symbol(const std::string& name) {
    return ensure_symbol(name, mc::SymBind::Global, mc::SymType::Func, true);
}

uint32_t AArch64ISel::add_func_symbol(const IRFunction& fn) {
    if (fn.is_extern) {
        return ensure_symbol(abi_name(fn), mc::SymBind::Global, mc::SymType::Func, true);
    }
    return ensure_symbol(function_name(fn), mc::SymBind::Global, mc::SymType::Func,
                         false, 0, text.code.size(), 0);
}

uint32_t AArch64ISel::region_alloc_label() {
    return next_region_label++;
}

// Prologue / epilogue

void AArch64ISel::emit_prologue(const IRFunction& fn) {
    const std::size_t frame_size = align16(
        (static_cast<std::size_t>(fn.local_count) +
         static_cast<std::size_t>(fn.temp_count)) * 8u +
        static_cast<std::size_t>(fn.extra_stack) + 16u);

    // SUB SP, SP, #frame_size
    text.sub_sp(static_cast<uint16_t>(frame_size));
    // STP X29, X30, [SP]  (saved regs at [SP+0] and [SP+8])
    text.stp(aarch64::X29, aarch64::X30, aarch64::SP, 0);
    // MOV X29, SP  (FP = SP)
    text.add_imm(aarch64::X29, aarch64::SP, 0);

    // Spill register arguments into local slots.
    // AAPCS64: sret pointer in X8, user args in X0-X7.
    // arg[0] = sret (from X8 if sret, else from X0).
    // arg[1..] = X0-X6 (shifted by 1 if sret).
    static const aarch64::Reg arg_regs[] = {
        aarch64::X0, aarch64::X1, aarch64::X2, aarch64::X3,
        aarch64::X4, aarch64::X5, aarch64::X6, aarch64::X7
    };
    for (uint32_t i = 0; i < fn.arg_count; ++i) {
        if (i < 8) {
            if (fn.sret && i == 0) {
                // sret pointer arrives in X8
                text.str_imm(aarch64::X8, aarch64::X29, local_offset(i, fn));
            } else {
                const uint32_t reg_idx = fn.sret ? (i - 1) : i;
                text.str_imm(arg_regs[reg_idx], aarch64::X29, local_offset(i, fn));
            }
        } else {
            // Stack args: caller placed them above our frame.
            // After SUB SP, the old SP is at X29 + frame_size.
            const int64_t offset = static_cast<int64_t>(frame_size) +
                                   static_cast<int64_t>((i - 8u) * 8u);
            text.ldur(aarch64::X9, aarch64::X29, static_cast<int16_t>(offset));
            text.str_imm(aarch64::X9, aarch64::X29, local_offset(i, fn));
        }
    }
}

void AArch64ISel::emit_epilogue(const IRFunction& fn) {
    const std::size_t frame_size = align16(
        (static_cast<std::size_t>(fn.local_count) +
         static_cast<std::size_t>(fn.temp_count)) * 8u +
        static_cast<std::size_t>(fn.extra_stack) + 16u);

    // LDP X29, X30, [SP]
    text.ldp(aarch64::X29, aarch64::X30, aarch64::SP, 0);
    // ADD SP, SP, #frame_size
    text.add_sp_imm(aarch64::SP, static_cast<uint16_t>(frame_size));
    text.ret();
}

// Call

void AArch64ISel::emit_call(const IRProgram& program, const IRFunction& fn, const IRCall& x) {
    if (x.func_id >= program.functions.size()) {
        utils::logger::crash("IRCall references invalid function id: " + std::to_string(x.func_id));
    }

    const auto& callee = program.functions[x.func_id];
    const std::size_t n = x.args.size();

    // AAPCS64: X0-X7 for args, X8 for sret pointer.
    // For sret: X8 = hidden pointer, then user args start at X0.
    const std::size_t max_reg = x.sret ? 7u : 8u;  // if sret, X8 is taken
    const std::size_t stack_count = (n > max_reg) ? (n - max_reg) : 0;

    std::size_t frame = stack_count * 8;
    if (frame % 16 != 0) frame += 8;

    if (frame > 0) {
        text.sub_sp(static_cast<uint16_t>(frame));
    }

    // Stack args (right to left)
    for (std::size_t i = n; i > max_reg; --i) {
        const std::size_t idx = i - 1;
        const std::size_t slot = idx - max_reg;
        text.ldr_imm(aarch64::X9, aarch64::X29, temp_offset(x.args[idx], fn));
        text.str_imm(aarch64::X9, aarch64::SP, static_cast<uint16_t>(slot * 8));
    }

    // Register args
    static const aarch64::Reg arg_regs[] = {
        aarch64::X0, aarch64::X1, aarch64::X2, aarch64::X3,
        aarch64::X4, aarch64::X5, aarch64::X6, aarch64::X7
    };

    std::size_t reg_idx = 0;

    // sret pointer goes to X8 first
    if (x.sret) {
        text.ldr_imm(aarch64::X8, aarch64::X29, temp_offset(x.args[0], fn));
        reg_idx = 1;
    }

    // Remaining args in X0-X7 (or X0-X6 if sret)
    for (std::size_t i = reg_idx; i < n && (i - reg_idx) < max_reg; ++i) {
        const std::size_t ri = x.sret ? (i - 1) : i;
        if (ri < max_reg) {
            text.ldr_imm(arg_regs[ri], aarch64::X29, temp_offset(x.args[i], fn));
        }
    }

    const std::string callee_name = abi_name(callee);
    if (symbol_index.count(callee_name) == 0) {
        if (callee.is_extern) {
            add_func_symbol(callee);
        } else {
            add_runtime_symbol(callee_name);
        }
    }

    text.bl(callee_name);

    if (frame > 0) {
        text.add_sp_imm(aarch64::SP, static_cast<uint16_t>(frame));
    }

    if (!x.sret) {
        text.str_imm(aarch64::X0, aarch64::X29, temp_offset(x.dst, fn));
    }
}

// Syscall stub

// Map the syscall number embedded in IR (@syscall(N)) to the target OS.
//   Linux:     source numbers follow the x86-64 convention and are remapped
//              to AArch64 Linux numbers here.
//   ZeroPoint: numbers are ZeroPoint ABI numbers used as-is
//              (write=0, read=1, open=10, exit=20). The stub passes the
//              single buffer argument in X0; everything else (length,
//              descriptor, mode) is computed by the OS.
uint32_t AArch64ISel::map_syscall(uint32_t nr) const {
    if (target_os == mc::TargetOS::ZeroPoint) return nr;
    switch (nr) {
        case 0:   return 63;   // read
        case 1:   return 64;   // write
        case 2:   return 56;   // open
        case 3:   return 57;   // close
        case 8:   return 62;   // lseek
        case 9:   return 222;  // mmap
        case 11:  return 215;  // munmap
        case 39:  return 174;  // getpid
        case 57:  return 172;  // fork
        case 59:  return 221;  // execve
        case 60:  return 93;   // exit
        case 74:  return 82;   // fsync
        case 257: return 56;   // openat (close=57, openat on x86=257 -> aarch64=56)
        default:  return nr;   // pass-through for unknown
    }
}

void AArch64ISel::emit_syscall_stub(const IRFunction& fn) {
    ensure_symbol(abi_name(fn), mc::SymBind::Global, mc::SymType::Func,
                  false, 0, text.code.size(), 0);
    // Syscall: X8 = number, X0-X5 = args, SVC #0, result in X0.
    // No register shuffling needed (unlike x86-64 where RCX -> R10).
    text.mov_imm64(aarch64::X8, static_cast<uint64_t>(map_syscall(static_cast<uint32_t>(fn.syscall_number))));
    text.svc();
    text.ret();
}

// Region
//
// Region layout in the local slot (24 bytes, stored as 3 qwords via pointer):
//   [ptr+0] = data pointer (from mmap)
//   [ptr+8] = current offset (bump pointer)
//   [ptr+16] = capacity

void AArch64ISel::emit_region_begin(const IRRegionBegin& x) {
    // ZeroPoint has no mmap/munmap yet, so regions cannot be lowered.
    if (target_os == mc::TargetOS::ZeroPoint) {
        utils::logger::crash("region alloc is not supported on the ZeroPoint target");
    }
    // Load region struct pointer into X9.
    text.ldr_imm(aarch64::X9, aarch64::X29, local_offset(x.region_local, {}));

    // mmap(0, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
    // Syscall 222 on aarch64 Linux.
    text.mov_imm64(aarch64::X0, 0);
    text.mov_imm64(aarch64::X1, x.region_size);
    text.mov_imm64(aarch64::X2, 3);          // PROT_READ|PROT_WRITE
    text.mov_imm64(aarch64::X3, 0x22);       // MAP_PRIVATE|MAP_ANONYMOUS
    text.mov_imm64(aarch64::X4, static_cast<uint64_t>(-1));
    text.mov_imm64(aarch64::X5, 0);
    text.mov_imm64(aarch64::X8, 222);
    text.svc();

    // Store returned pointer at [ptr+0].
    text.str_imm(aarch64::X0, aarch64::X9, 0);
    // Zero offset at [ptr+8].
    text.str_imm(aarch64::XZR, aarch64::X9, 8);
    // Store capacity at [ptr+16].
    text.mov_imm64(aarch64::X10, x.region_size);
    text.str_imm(aarch64::X10, aarch64::X9, 16);
}

void AArch64ISel::emit_region_alloc(const IRRegionAlloc& x, const IRFunction& fn) {
    const uint32_t ok_label = region_alloc_label();

    // Load region struct pointer.
    text.ldr_imm(aarch64::X9, aarch64::X29, local_offset(x.region_local, {}));
    // X0 = [ptr+0]   (data base)
    text.ldur(aarch64::X0, aarch64::X9, 0);
    // X1 = [ptr+8]   (current offset)
    text.ldur(aarch64::X1, aarch64::X9, 8);
    // result = data + offset
    text.add_reg(aarch64::X10, aarch64::X0, aarch64::X1);
    text.str_imm(aarch64::X10, aarch64::X29, temp_offset(x.dst, fn));

    // Align size to 16 bytes: size = (size + 15) & ~15
    // Use LSR/LSL to clear low 4 bits after adding 15.
    text.ldr_imm(aarch64::X10, aarch64::X29, temp_offset(x.size, fn));
    text.add_imm(aarch64::X10, aarch64::X10, 15);
    text.lsr_imm(aarch64::X10, aarch64::X10, 4);
    text.lsl_imm(aarch64::X10, aarch64::X10, 4);

    // Bump: [ptr+8] += aligned_size
    text.ldur(aarch64::X11, aarch64::X9, 8);
    text.add_reg(aarch64::X11, aarch64::X11, aarch64::X10);
    text.str_imm(aarch64::X11, aarch64::X9, 8);

    // Overflow check: if ([ptr+8]) > [ptr+16], exit(1).
    text.ldur(aarch64::X0, aarch64::X9, 8);
    text.ldur(aarch64::X1, aarch64::X9, 16);
    text.cmp_reg(aarch64::X0, aarch64::X1);
    text.b_cond(aarch64::COND_LS, 0);  // if X0 <= X1, skip (LS = unsigned lower or same)
    fixups.push_back({static_cast<uint32_t>(text.code.size() - 4), ok_label, true});

    // exit(1) inline
    text.mov_imm64(aarch64::X0, 1);
    text.mov_imm64(aarch64::X8, 93);
    text.svc();

    region_label_pos[ok_label] = text.code.size();
}

void AArch64ISel::emit_region_end(const IRRegionEnd& x) {
    text.ldr_imm(aarch64::X9, aarch64::X29, local_offset(x.region_local, {}));
    // munmap(ptr, size) — syscall 215 on aarch64.
    text.ldur(aarch64::X0, aarch64::X9, 0);
    text.ldur(aarch64::X1, aarch64::X9, 16);
    text.mov_imm64(aarch64::X8, 215);
    text.svc();
}

// Per-instruction lowering

void AArch64ISel::emit_inst(const IRProgram& program, const IRFunction& fn, const IRInst& inst) {
    std::visit(overloaded{
        [&](const IRLoadConst& x) {
            text.mov_imm64(aarch64::X0, static_cast<uint64_t>(x.value));
            text.str_imm(aarch64::X0, aarch64::X29, temp_offset(x.dst, fn));
        },
        [&](const IRLoadFloatConst& x) {
            if (x.kind == ast::TypeKind::F32) {
                const int32_t bits = float_bits(static_cast<float>(x.value));
                text.mov_imm64(aarch64::X0, static_cast<uint64_t>(bits));
                text.fmov_d_reg(aarch64::X0, aarch64::D0);
                text.fcvt_s_d(aarch64::D0, aarch64::S0);
                text.str_s(aarch64::S0, aarch64::X29, temp_offset(x.dst, fn));
            } else {
                const int64_t bits = double_bits(x.value);
                text.mov_imm64(aarch64::X0, static_cast<uint64_t>(bits));
                text.fmov_d_reg(aarch64::X0, aarch64::D0);
                text.str_d(aarch64::D0, aarch64::X29, temp_offset(x.dst, fn));
            }
        },
        [&](const IRLoadString& x) {
            if (x.string_id >= program.strings.size()) {
                utils::logger::crash("IRLoadString references invalid string id");
            }
            const auto& lit = program.strings[x.string_id];
            const std::string label = string_label(lit.id);
            ensure_symbol(label, mc::SymBind::Local, mc::SymType::Object,
                          false, 1, obj.data.size(), lit.value.size() + 1);
            // ADRP X0, symbol@PAGE
            text.reloc(label, mc::RelType::AARCH64_ADR_PREL_PG_HI21, 0);
            text.emit32(0x90000000u);
            // ADD X0, X0, symbol@PAGEOFF
            text.reloc(label, mc::RelType::AARCH64_ADD_ABS_LO12_NC, 0);
            text.emit32(0x91000000u);
            text.str_imm(aarch64::X0, aarch64::X29, temp_offset(x.dst, fn));
        },
        [&](const IRLoadLocal& x) {
            text.ldr_imm(aarch64::X0, aarch64::X29, local_offset(x.local, fn));
            text.str_imm(aarch64::X0, aarch64::X29, temp_offset(x.dst, fn));
        },
        [&](const IRStoreLocal& x) {
            text.ldr_imm(aarch64::X0, aarch64::X29, temp_offset(x.src, fn));
            text.str_imm(aarch64::X0, aarch64::X29, local_offset(x.local, fn));
        },
        [&](const IRAddrOf& x) {
            // X0 = FP + local_offset  (local_offset is negative, so subtract abs)
            const int64_t off = local_offset(x.local, fn);
            text.sub_imm(aarch64::X0, aarch64::X29, static_cast<uint16_t>(-off));
            text.str_imm(aarch64::X0, aarch64::X29, temp_offset(x.dst, fn));
        },
        [&](const IRBinary& x) {
            const bool flt = is_float(x.type_kind);
            if (flt) {
                const bool is64 = (x.type_kind == ast::TypeKind::F64);

                if (is64) {
                    text.ldr_d(aarch64::D0, aarch64::X29, temp_offset(x.lhs, fn));
                    text.ldr_d(aarch64::D1, aarch64::X29, temp_offset(x.rhs, fn));
                } else {
                    text.ldr_s(aarch64::S0, aarch64::X29, temp_offset(x.lhs, fn));
                    text.ldr_s(aarch64::S1, aarch64::X29, temp_offset(x.rhs, fn));
                }

                switch (x.op) {
                    case IRBinaryOp::Add:
                        if (is64) text.fadd_d(aarch64::D0, aarch64::D0, aarch64::D1);
                        else      text.fadd_s(aarch64::S0, aarch64::S0, aarch64::S1);
                        break;
                    case IRBinaryOp::Sub:
                        if (is64) text.fsub_d(aarch64::D0, aarch64::D0, aarch64::D1);
                        else      text.fsub_s(aarch64::S0, aarch64::S0, aarch64::S1);
                        break;
                    case IRBinaryOp::Mul:
                        if (is64) text.fmul_d(aarch64::D0, aarch64::D0, aarch64::D1);
                        else      text.fmul_s(aarch64::S0, aarch64::S0, aarch64::S1);
                        break;
                    case IRBinaryOp::Div:
                        if (is64) text.fdiv_d(aarch64::D0, aarch64::D0, aarch64::D1);
                        else      text.fdiv_s(aarch64::S0, aarch64::S0, aarch64::S1);
                        break;
                    case IRBinaryOp::Eq: case IRBinaryOp::NotEq:
                    case IRBinaryOp::Lt: case IRBinaryOp::Lte:
                    case IRBinaryOp::Gt: case IRBinaryOp::Gte: {
                        if (is64) text.fcmp_d(aarch64::D0, aarch64::D1);
                        else      text.fcmp_s(aarch64::S0, aarch64::S1);
                        aarch64::Cond c = float_cmp_op_to_cond(x.op);
                        text.cset(aarch64::X0, c);
                        text.str_imm(aarch64::X0, aarch64::X29, temp_offset(x.dst, fn));
                        return;
                    }
                    default: break;
                }

                if (is64) {
                    text.str_d(aarch64::D0, aarch64::X29, temp_offset(x.dst, fn));
                } else {
                    text.str_s(aarch64::S0, aarch64::X29, temp_offset(x.dst, fn));
                }
                return;
            }

            // Integer binary op
            text.ldr_imm(aarch64::X0, aarch64::X29, temp_offset(x.lhs, fn));
            text.ldr_imm(aarch64::X1, aarch64::X29, temp_offset(x.rhs, fn));

            switch (x.op) {
                case IRBinaryOp::Add:
                    text.add_reg(aarch64::X0, aarch64::X0, aarch64::X1);
                    break;
                case IRBinaryOp::Sub:
                    text.sub_reg(aarch64::X0, aarch64::X0, aarch64::X1);
                    break;
                case IRBinaryOp::Mul:
                    text.mul(aarch64::X0, aarch64::X0, aarch64::X1);
                    break;
                case IRBinaryOp::Div:
                    if (is_signed_int(x.type_kind)) {
                        text.sdiv(aarch64::X0, aarch64::X0, aarch64::X1);
                    } else {
                        text.udiv(aarch64::X0, aarch64::X0, aarch64::X1);
                    }
                    break;
                case IRBinaryOp::BitAnd:
                    text.and_reg(aarch64::X0, aarch64::X0, aarch64::X1);
                    break;
                case IRBinaryOp::BitOr:
                    text.orr_reg(aarch64::X0, aarch64::X0, aarch64::X1);
                    break;
                case IRBinaryOp::LogicAnd:
                    // Normalize both operands to 0/1.
                    // X0 = (X0 != 0) ? 1 : 0
                    text.cmp_imm(aarch64::X0, 0);
                    text.cset(aarch64::X0, aarch64::COND_NE);
                    text.cmp_imm(aarch64::X1, 0);
                    text.cset(aarch64::X1, aarch64::COND_NE);
                    text.and_reg(aarch64::X0, aarch64::X0, aarch64::X1);
                    break;
                case IRBinaryOp::LogicOr:
                    text.cmp_imm(aarch64::X0, 0);
                    text.cset(aarch64::X0, aarch64::COND_NE);
                    text.cmp_imm(aarch64::X1, 0);
                    text.cset(aarch64::X1, aarch64::COND_NE);
                    text.orr_reg(aarch64::X0, aarch64::X0, aarch64::X1);
                    break;
                case IRBinaryOp::Eq: case IRBinaryOp::NotEq:
                case IRBinaryOp::Lt: case IRBinaryOp::Lte:
                case IRBinaryOp::Gt: case IRBinaryOp::Gte:
                    text.cmp_reg(aarch64::X0, aarch64::X1);
                    text.cset(aarch64::X0, cmp_op_to_cond(x.op, is_signed_int(x.type_kind)));
                    break;
            }
            text.str_imm(aarch64::X0, aarch64::X29, temp_offset(x.dst, fn));
        },
        [&](const IRCall& x) {
            emit_call(program, fn, x);
        },
        [&](const IRReturn& x) {
            text.ldr_imm(aarch64::X0, aarch64::X29, temp_offset(x.value, fn));
            emit_epilogue(fn);
        },
        [&](const IRJump& x) {
            text.b(0);  // placeholder
            fixups.push_back({static_cast<uint32_t>(text.code.size() - 4), x.target, false});
        },
        [&](const IRBranch& x) {
            text.ldr_imm(aarch64::X0, aarch64::X29, temp_offset(x.cond, fn));
            text.cmp_imm(aarch64::X0, 0);
            text.b_cond(aarch64::COND_NE, 0);  // if X0 != 0, goto then
            fixups.push_back({static_cast<uint32_t>(text.code.size() - 4), x.then_label, false});
            text.b(0);  // else goto else
            fixups.push_back({static_cast<uint32_t>(text.code.size() - 4), x.else_label, false});
        },
        [&](const IRLabel& x) {
            label_pos[x.id] = text.code.size();
        },
        [&](const IRGetField& x) {
            text.ldr_imm(aarch64::X0, aarch64::X29, temp_offset(x.base, fn));
            text.ldur(aarch64::X0, aarch64::X0, static_cast<int16_t>(x.offset));
            text.str_imm(aarch64::X0, aarch64::X29, temp_offset(x.dst, fn));
        },
        [&](const IRSetField& x) {
            text.ldr_imm(aarch64::X0, aarch64::X29, temp_offset(x.base, fn));
            text.ldr_imm(aarch64::X1, aarch64::X29, temp_offset(x.value, fn));
            text.str_imm(aarch64::X1, aarch64::X0, static_cast<int16_t>(x.offset));
        },
        [&](const IRLoadElement& x) {
            text.ldr_imm(aarch64::X0, aarch64::X29, temp_offset(x.base, fn));
            text.ldr_imm(aarch64::X1, aarch64::X29, temp_offset(x.index, fn));
            if (x.elem_size > 1) {
                text.mov_imm64(aarch64::X2, x.elem_size);
                text.mul(aarch64::X1, aarch64::X1, aarch64::X2);
            }
            text.add_reg(aarch64::X0, aarch64::X0, aarch64::X1);
            if (x.elem_size == 1) {
                text.ldrb(aarch64::X0, aarch64::X0, 0);
            } else if (x.elem_size == 2) {
                text.ldrh(aarch64::X0, aarch64::X0, 0);
            } else if (x.elem_size <= 4) {
                text.ldr32_imm(static_cast<aarch64::WReg>(aarch64::W0), aarch64::X0, 0);
            } else {
                text.ldur(aarch64::X0, aarch64::X0, 0);
            }
            text.str_imm(aarch64::X0, aarch64::X29, temp_offset(x.dst, fn));
        },
        [&](const IRStoreElement& x) {
            text.ldr_imm(aarch64::X0, aarch64::X29, temp_offset(x.base, fn));
            text.ldr_imm(aarch64::X1, aarch64::X29, temp_offset(x.index, fn));
            text.ldr_imm(aarch64::X2, aarch64::X29, temp_offset(x.value, fn));
            if (x.elem_size > 1) {
                text.mov_imm64(aarch64::X3, x.elem_size);
                text.mul(aarch64::X1, aarch64::X1, aarch64::X3);
            }
            text.add_reg(aarch64::X0, aarch64::X0, aarch64::X1);
            if (x.elem_size == 1) {
                text.strb(aarch64::X2, aarch64::X0, 0);
            } else if (x.elem_size == 2) {
                text.strh(aarch64::X2, aarch64::X0, 0);
            } else if (x.elem_size <= 4) {
                text.str32_imm(static_cast<aarch64::WReg>(aarch64::W2), aarch64::X0, 0);
            } else {
                text.str_imm(aarch64::X2, aarch64::X0, 0);
            }
        },
        [&](const IRCast& x) {
            if (x.kind == ast::CastKind::Bitcast) {
                text.ldr_imm(aarch64::X0, aarch64::X29, temp_offset(x.src, fn));
                text.str_imm(aarch64::X0, aarch64::X29, temp_offset(x.dst, fn));
                return;
            }

            const int src_sz = cast_type_size(x.src_kind);
            const int dst_sz = cast_type_size(x.target_kind);
            const bool src_int = is_integer(x.src_kind);
            const bool dst_int = is_integer(x.target_kind);
            const bool src_flt = is_float(x.src_kind);
            const bool dst_flt = is_float(x.target_kind);

            if (x.target_kind == ast::TypeKind::String) {
                // int/float -> string: call qk_format_*
                text.ldr_imm(aarch64::X0, aarch64::X29, temp_offset(x.src, fn));
                if (is_integer(x.src_kind)) {
                    if (is_signed_int(x.src_kind)) {
                        add_runtime_symbol("qk_format_i64");
                        text.bl("qk_format_i64");
                    } else {
                        add_runtime_symbol("qk_format_u64");
                        text.bl("qk_format_u64");
                    }
                } else if (x.src_kind == ast::TypeKind::F32) {
                    text.ldr_s(aarch64::S0, aarch64::X29, temp_offset(x.src, fn));
                    text.fcvt_d_s(aarch64::S0, aarch64::D0);
                    add_runtime_symbol("qk_format_f64");
                    text.bl("qk_format_f64");
                } else {
                    add_runtime_symbol("qk_format_f64");
                    text.bl("qk_format_f64");
                }
                text.str_imm(aarch64::X0, aarch64::X29, temp_offset(x.dst, fn));
                return;
            }

            text.ldr_imm(aarch64::X0, aarch64::X29, temp_offset(x.src, fn));

            if (src_int && dst_int) {
                if (src_sz == dst_sz) {
                    // same size, just copy
                } else if (src_sz < dst_sz) {
                    // widening
                    if (is_signed_int(x.src_kind)) {
                        if (src_sz == 1)      text.sxtb(aarch64::X0, aarch64::W0);
                        else if (src_sz == 2) text.sxth(aarch64::X0, aarch64::W0);
                        else if (src_sz == 4) text.sxtw(aarch64::X0, aarch64::W0);
                    } else {
                        if (src_sz == 1)      text.uxtb(aarch64::X0, aarch64::W0);
                        else if (src_sz == 2) text.uxth(aarch64::X0, aarch64::W0);
                        else if (src_sz == 4) text.ldr32_imm(aarch64::W0, aarch64::SP, 0);  // zero-extend by writing to W0
                    }
                } else {
                    // narrowing: truncate by writing to W register (auto zero-extends upper bits on AArch64)
                    if (dst_sz == 4) {
                        text.str32_imm(aarch64::W0, aarch64::SP, 0);
                        text.ldur(aarch64::X0, aarch64::SP, 0);
                    } else if (dst_sz == 2) {
                        text.uxth(aarch64::X0, aarch64::W0);
                    } else {
                        text.uxtb(aarch64::X0, aarch64::W0);
                    }
                }
                text.str_imm(aarch64::X0, aarch64::X29, temp_offset(x.dst, fn));

            } else if (src_int && dst_flt) {
                if (dst_sz == 4) {
                    text.scvtf_s(aarch64::X0, aarch64::S0);
                    text.str_s(aarch64::S0, aarch64::X29, temp_offset(x.dst, fn));
                } else {
                    text.scvtf_d(aarch64::X0, aarch64::D0);
                    text.str_d(aarch64::D0, aarch64::X29, temp_offset(x.dst, fn));
                }

            } else if (src_flt && dst_int) {
                if (src_sz == 4) {
                    text.ldr_s(aarch64::S0, aarch64::X29, temp_offset(x.src, fn));
                    text.fcvtzs_s(aarch64::S0, aarch64::X0);
                } else {
                    text.ldr_d(aarch64::D0, aarch64::X29, temp_offset(x.src, fn));
                    text.fcvtzs_d(aarch64::D0, aarch64::X0);
                }
                text.str_imm(aarch64::X0, aarch64::X29, temp_offset(x.dst, fn));

            } else if (src_flt && dst_flt) {
                if (src_sz == 4 && dst_sz == 8) {
                    text.ldr_s(aarch64::S0, aarch64::X29, temp_offset(x.src, fn));
                    text.fcvt_d_s(aarch64::S0, aarch64::D0);
                    text.str_d(aarch64::D0, aarch64::X29, temp_offset(x.dst, fn));
                } else if (src_sz == 8 && dst_sz == 4) {
                    text.ldr_d(aarch64::D0, aarch64::X29, temp_offset(x.src, fn));
                    text.fcvt_s_d(aarch64::D0, aarch64::S0);
                    text.str_s(aarch64::S0, aarch64::X29, temp_offset(x.dst, fn));
                } else {
                    text.ldr_imm(aarch64::X0, aarch64::X29, temp_offset(x.src, fn));
                    text.str_imm(aarch64::X0, aarch64::X29, temp_offset(x.dst, fn));
                }
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
            // x.offset == fn.extra_stack (end of alloca area on x86 where frame is inverted).
            // On AArch64 alloca grows upward from fp+16+base, so map to the start.
            const std::size_t off = 16 + base + static_cast<std::size_t>(fn.extra_stack) - static_cast<std::size_t>(x.offset);
            // X0 = FP + off
            text.add_imm(aarch64::X0, aarch64::X29, static_cast<uint16_t>(off));
            text.str_imm(aarch64::X0, aarch64::X29, temp_offset(x.dst, fn));
        },
        [&](const IRLoadGlobal& x) {
            const std::string label = global_label(x.global_id);
            // ADRP X0, symbol@PAGE
            text.reloc(label, mc::RelType::AARCH64_ADR_PREL_PG_HI21, 0);
            text.emit32(0x90000000u);
            // LDR X0, [X0, symbol@PAGEOFF]
            text.reloc(label, mc::RelType::AARCH64_LDST64_ABS_LO12_NC, 0);
            text.emit32(0xF9400000u);
            text.str_imm(aarch64::X0, aarch64::X29, temp_offset(x.dst, fn));
        },
        [&](const IRStoreGlobal& x) {
            const std::string label = global_label(x.global_id);
            text.ldr_imm(aarch64::X0, aarch64::X29, temp_offset(x.src, fn));
            // ADRP X1, symbol@PAGE
            text.reloc(label, mc::RelType::AARCH64_ADR_PREL_PG_HI21, 0);
            text.emit32(0x90000001u);
            // STR X0, [X1, symbol@PAGEOFF]
            text.reloc(label, mc::RelType::AARCH64_LDST64_ABS_LO12_NC, 0);
            text.emit32(0xF9000020u);
        },
        [&](const IRLoadGlobalAddr& x) {
            const std::string label = global_label(x.global_id);
            // ADRP X0, symbol@PAGE
            text.reloc(label, mc::RelType::AARCH64_ADR_PREL_PG_HI21, 0);
            text.emit32(0x90000000u);
            // ADD X0, X0, symbol@PAGEOFF
            text.reloc(label, mc::RelType::AARCH64_ADD_ABS_LO12_NC, 0);
            text.emit32(0x91000000u);
            text.str_imm(aarch64::X0, aarch64::X29, temp_offset(x.dst, fn));
        },
    }, inst);
}

// Main entry points

void AArch64ISel::emit_func(const IRProgram& program, const IRFunction& fn) {
    add_func_symbol(fn);
    emit_prologue(fn);
    for (const auto& inst : fn.body) {
        emit_inst(program, fn, inst);
    }
}

void AArch64ISel::emit_start(const IRProgram& program) {
    ensure_symbol("_start", mc::SymBind::Global, mc::SymType::Func, false, 0, text.code.size(), 0);
    const IRFunction* entry = find_entry(program);
    if (!entry) {
        utils::logger::crash("No entry point found");
    }
    // BL main
    text.bl(function_name(*entry));
    if (target_os == mc::TargetOS::ZeroPoint) {
        // ZeroPoint has no exit syscall yet (20 is reserved but unimplemented):
        // park the core after main returns. PIC-safe: PC-relative loop only.
        text.wfe();
        text.b(-4);  // branch to self (WFE)
    } else {
        // MOV X8, #93; SVC #0  (exit syscall)
        text.mov_imm64(aarch64::X8, 93);
        text.svc();
    }
}

void AArch64ISel::emit_strings(const IRProgram& program) {
    for (const auto& s : program.strings) {
        ensure_symbol(string_label(s.id), mc::SymBind::Local, mc::SymType::Object,
                      false, 1, obj.data.size(), s.value.size() + 1);
        obj.data.insert(obj.data.end(), s.value.begin(), s.value.end());
        obj.data.push_back(0);
    }
}

void AArch64ISel::emit_globals(const IRProgram& program) {
    for (uint32_t i = 0; i < program.globals.size(); ++i) {
        const auto& g = program.globals[i];
        if (g.is_extern) continue;
        while (obj.data.size() % 8u != 0) obj.data.push_back(0);
        ensure_symbol(global_label(i), mc::SymBind::Local, mc::SymType::Object,
                      false, 1, obj.data.size(), g.size);
        for (uint32_t j = 0; j < g.size; ++j) obj.data.push_back(0);
    }
}

void AArch64ISel::patch_fixups() {
    for (const auto& f : fixups) {
        const uint32_t target = f.region
            ? region_label_pos.at(f.key)
            : label_pos.at(f.key);
        const int32_t disp = static_cast<int32_t>(target - f.field_offset);

        // Read the instruction at field_offset to determine the type.
        uint32_t insn = 0;
        for (int i = 0; i < 4; ++i) {
            insn |= static_cast<uint32_t>(text.code[f.field_offset + i]) << (8 * i);
        }

        // Determine branch type from the instruction encoding:
        //   B  (unconditional): bits[31:26] = 000101
        //   BL:                bits[31:26] = 100101
        //   B.cond:            bits[31:24] = 01010100
        //   CBZ:               bits[31:24] = 10110100
        //   CBNZ:              bits[31:24] = 10110101
        const uint32_t opc = (insn >> 26) & 0x3F;
        const uint32_t top8 = (insn >> 24) & 0xFF;

        if (opc == 0x05 || opc == 0x25) {
            // B or BL: imm26 * 4 = offset => imm26 = disp >> 2
            const uint32_t imm26 = (static_cast<uint32_t>(disp) >> 2) & 0x03FFFFFFu;
            insn = (insn & 0xFC000000u) | imm26;
        } else if (top8 == 0x54 || top8 == 0xB4 || top8 == 0xB5) {
            // B.cond / CBZ / CBNZ: imm19 * 4 = offset => imm19 = disp >> 2
            const uint32_t imm19 = (static_cast<uint32_t>(disp) >> 2) & 0x7FFFFu;
            insn = (insn & 0xFF00001Fu) | (imm19 << 5);
        } else {
            // Unknown instruction for fixup.
            utils::logger::crash("patch_fixups: unrecognised instruction at offset " +
                                 std::to_string(f.field_offset));
        }

        for (int i = 0; i < 4; ++i) {
            text.code[f.field_offset + i] = static_cast<uint8_t>((insn >> (8 * i)) & 0xFF);
        }
    }
}

void AArch64ISel::generate(const IRProgram& program) {
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

    if(should_emit_start && target_os == mc::TargetOS::ZeroPoint) emit_start(program);

    for (const auto& fn : program.functions) {
        if (fn.is_extern) {
            if (fn.syscall_number >= 0) {
                emit_syscall_stub(fn);
            } else {
                add_func_symbol(fn);
            }
            continue;
        }
        emit_func(program, fn);
    }

    if(should_emit_start && target_os != mc::TargetOS::ZeroPoint ) emit_start(program);
    emit_strings(program);
    emit_globals(program);
    patch_fixups();

    obj.text = std::move(text.code);
    obj.relocs = std::move(text.relocs);
}

} // namespace quant::codegen
