#include <cstring>

#include "quant/backend/fasmcodegen.h"
namespace quant::codegen{
namespace {

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

    template<class... Ts>
    struct overloaded : Ts... {
        using Ts::operator()...;
    };

    template<class... Ts>
    overloaded(Ts...) -> overloaded<Ts...>;

    std::string asm_mangle(std::string name) {
        for (char& ch : name) {
            const unsigned char c = static_cast<unsigned char>(ch);
            if (!(std::isalnum(c) || ch == '_')) {
                ch = '_';
            }
        }
        return name;
    }

    std::string label_name(uint32_t func_id, uint32_t label_id) {
        return "L_" + std::to_string(func_id) + "_" + std::to_string(label_id);
    }

    std::string function_name(const IRFunction& fn) {
        if (!fn.export_name.empty()) {
            return fn.export_name;
        }
        return "qk_" + asm_mangle(fn.name);
    }
    std::string abi_name(const IRFunction& fn) {
        if (fn.is_extern) {
            return "qk_" + asm_mangle(fn.name);
        }
        return function_name(fn);
    }

    std::size_t align16(std::size_t n) {
        return (n + 15u) & ~std::size_t(15u);
    }

    const IRFunction* find_entry(const IRProgram& program) {
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
      std::string string_label(uint32_t id) {
        return "str_" + std::to_string(id);
    }

    std::string global_label(uint32_t id) {
        return "gbl_" + std::to_string(id);
    }

    std::string db_bytes(const std::string& s) {
        std::string out = "    db ";
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (i != 0) out += ", ";
            out += std::to_string(static_cast<unsigned>(
                static_cast<unsigned char>(s[i])
            ));
        }
        if (!s.empty()) out += ", ";
        out += "0";
        return out;
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

        std::string mem_size(int bytes) {
        switch (bytes) {
            case 1: return "byte";
            case 2: return "word";
            case 4: return "dword";
            case 8: return "qword";
            default: return "qword";
        }
    }

    std::string reg_from_size(int bytes) {
        switch (bytes) {
            case 1: return "bl";
            case 2: return "bx";
            case 4: return "ebx";
            case 8: return "rbx";
            default: return "rbx";
        }
    }

} // namespace

// FasmCodeGenerator
    std::string FasmCodeGenerator::local_slot(Local l) {
            return "[rbp - " +
                std::to_string((static_cast<std::size_t>(l) + 1u) * 8u) +
                "]";
        }

    std::string FasmCodeGenerator::temp_slot(Reg r, const IRFunction& fn) {
            const std::size_t base = static_cast<std::size_t>(fn.local_count);
            return "[rbp - " +
                std::to_string((base + static_cast<std::size_t>(r) + 1u) * 8u) +
                "]";
    }

    uint32_t FasmCodeGenerator::region_alloc_label() {
        return next_region_label++;
    }
    void FasmCodeGenerator::emit_line(const std::string& s){
        out << s << '\n';
    }
    void FasmCodeGenerator::emit_load(Reg r, const IRFunction& fn) {
        emit_line("    mov rax, qword " + temp_slot(r, fn));
        emit_line("    push rax");
    }
    void FasmCodeGenerator::emit_store(Reg r, const IRFunction& fn) {
            emit_line("    pop rax");
            emit_line("    mov qword " + temp_slot(r, fn) + ", rax");
    }
    void FasmCodeGenerator::emit_binop(const IRBinary& x, const IRFunction& fn) {
        if (is_float(x.type_kind)) {
            const std::string sz = (x.type_kind == ast::TypeKind::F64) ? "sd" : "ss";
            const std::string ms = (x.type_kind == ast::TypeKind::F64) ? "qword" : "dword";

            emit_line("    mov" + sz + " xmm0, " + ms + " " + temp_slot(x.lhs, fn));
            emit_line("    mov" + sz + " xmm1, " + ms + " " + temp_slot(x.rhs, fn));

            switch (x.op) {
                case IRBinaryOp::Add:
                    emit_line("    add" + sz + " xmm0, xmm1");
                    emit_line("    mov" + sz + " " + ms + " " + temp_slot(x.dst, fn) + ", xmm0");
                    return;
                case IRBinaryOp::Sub:
                    emit_line("    sub" + sz + " xmm0, xmm1");
                    emit_line("    mov" + sz + " " + ms + " " + temp_slot(x.dst, fn) + ", xmm0");
                    return;
                case IRBinaryOp::Mul:
                    emit_line("    mul" + sz + " xmm0, xmm1");
                    emit_line("    mov" + sz + " " + ms + " " + temp_slot(x.dst, fn) + ", xmm0");
                    return;
                case IRBinaryOp::Div:
                    emit_line("    div" + sz + " xmm0, xmm1");
                    emit_line("    mov" + sz + " " + ms + " " + temp_slot(x.dst, fn) + ", xmm0");
                    return;
                case IRBinaryOp::Eq:
                case IRBinaryOp::NotEq:
                case IRBinaryOp::Lt:
                case IRBinaryOp::Lte:
                case IRBinaryOp::Gt:
                case IRBinaryOp::Gte:
                    emit_line("    comi" + sz + " xmm0, xmm1");
                    switch (x.op) {
                        case IRBinaryOp::Eq:    emit_line("    sete al"); break;
                        case IRBinaryOp::NotEq: emit_line("    setne al"); break;
                        case IRBinaryOp::Lt:    emit_line("    setb al"); break;
                        case IRBinaryOp::Lte:   emit_line("    setbe al"); break;
                        case IRBinaryOp::Gt:    emit_line("    seta al"); break;
                        case IRBinaryOp::Gte:   emit_line("    setae al"); break;
                    }
                    emit_line("    movzx rax, al");
                    emit_line("    mov qword " + temp_slot(x.dst, fn) + ", rax");
                    return;
            }
        }

        emit_load(x.lhs, fn);
        emit_load(x.rhs, fn);

        emit_line("    pop rbx");
        emit_line("    pop rax");

        switch (x.op) {
             case IRBinaryOp::Add:
                emit_line("    add rax, rbx");
                break;
            case IRBinaryOp::Sub:
                emit_line("    sub rax, rbx");
                break;
            case IRBinaryOp::Mul:
                emit_line("    imul rax, rbx");
                break;
            case IRBinaryOp::Div:
                if (is_signed_int(x.type_kind)) {
                    emit_line("    cqo");
                    emit_line("    idiv rbx");
                } else {
                    emit_line("    xor rdx, rdx");
                    emit_line("    div rbx");
                }
                break;
            case IRBinaryOp::BitAnd:
                emit_line("    and rax, rbx");
                break;
            case IRBinaryOp::BitOr:
                emit_line("    or rax, rbx");
                break;
            case IRBinaryOp::LogicAnd:
                emit_line("    test rax, rax");
                emit_line("    setne al");
                emit_line("    movzx rax, al");
                emit_line("    test rbx, rbx");
                emit_line("    setne bl");
                emit_line("    movzx rbx, bl");
                emit_line("    and rax, rbx");
                break;
            case IRBinaryOp::LogicOr:
                emit_line("    test rax, rax");
                emit_line("    setne al");
                emit_line("    movzx rax, al");
                emit_line("    test rbx, rbx");
                emit_line("    setne bl");
                emit_line("    movzx rbx, bl");
                emit_line("    or rax, rbx");
                break;
            case IRBinaryOp::Eq:
            case IRBinaryOp::NotEq:
            case IRBinaryOp::Lt:
            case IRBinaryOp::Lte:
            case IRBinaryOp::Gt:
            case IRBinaryOp::Gte:
                emit_line("    cmp rax, rbx");
                switch (x.op) {
                    case IRBinaryOp::Eq:    emit_line("    sete al"); break;
                    case IRBinaryOp::NotEq:  emit_line("    setne al"); break;
                    case IRBinaryOp::Lt:     emit_line("    setl al"); break;
                    case IRBinaryOp::Lte:    emit_line("    setle al"); break;
                    case IRBinaryOp::Gt:     emit_line("    setg al"); break;
                    case IRBinaryOp::Gte:    emit_line("    setge al"); break;
                    default: break;
            }
            emit_line("    movzx rax, al");
            break;
        }

        emit_line("    mov qword " + temp_slot(x.dst, fn) + ", rax");
    }
    void FasmCodeGenerator::emit_call(const IRProgram& program, const IRFunction& fn, const IRCall& x) {
        if (x.func_id >= program.functions.size()) {
            crash("IRCall references invalid function id: " + std::to_string(x.func_id));
        }

        const auto& callee = program.functions[x.func_id];
        const std::size_t n = x.args.size();
        const bool win_cc = (target_os == mc::TargetOS::Windows);

        const std::size_t max_reg = win_cc ? 4 : 6;
        const std::size_t shadow = win_cc ? 32 : 0;
        const std::size_t stack_count = (n > max_reg) ? (n - max_reg) : 0;
        const std::size_t frame = shadow + stack_count * 8;

        if (frame > 0) {
            emit_line("    sub rsp, " + std::to_string(frame));
        }

        // stack args (above shadow space or at [rsp])
        for (std::size_t i = n; i > max_reg; --i) {
            const std::size_t idx = i - 1;
            const std::size_t slot = idx - max_reg;
            emit_line("    mov rax, qword " + temp_slot(x.args[idx], fn));
            emit_line("    mov qword [rsp + " + std::to_string(shadow + slot * 8) + "], rax");
        }

        // register args
        if (win_cc) {
            static const char* const win_regs[] = {"rcx", "rdx", "r8", "r9"};
            for (std::size_t i = 0; i < n && i < max_reg; ++i) {
                emit_line("    mov " + std::string(win_regs[i]) + ", qword " + temp_slot(x.args[i], fn));
            }
        } else {
            static const char* const linux_regs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
            for (std::size_t i = 0; i < n && i < max_reg; ++i) {
                emit_line("    mov " + std::string(linux_regs[i]) + ", qword " + temp_slot(x.args[i], fn));
            }
        }

        emit_line("    call " + abi_name(callee));

        if (frame > 0) {
            emit_line("    add rsp, " + std::to_string(frame));
        }

        if (!x.sret) {
            emit_line("    mov qword " + temp_slot(x.dst, fn) + ", rax");
        }
    }
    void FasmCodeGenerator::emit_inst(const IRProgram& program, const IRFunction& fn, const IRInst& inst) {
        std::visit(overloaded{
            [&](const IRLoadConst& x) {
                emit_line("    mov rax, " + std::to_string(x.value));
                emit_line("    mov qword " + temp_slot(x.dst, fn) + ", rax");
            },

            [&](const IRLoadFloatConst& x) {
                if (x.kind == ast::TypeKind::F32) {
                    emit_line("    mov eax, " + std::to_string(float_bits(static_cast<float>(x.value))));
                    emit_line("    mov dword " + temp_slot(x.dst, fn) + ", eax");
                } else {
                    emit_line("    mov rax, " + std::to_string(double_bits(x.value)));
                    emit_line("    mov qword " + temp_slot(x.dst, fn) + ", rax");
                }
            },

            [&](const IRLoadLocal& x) {
                emit_line("    mov rax, qword " + local_slot(x.local));
                emit_line("    mov qword " + temp_slot(x.dst, fn) + ", rax");
            },

            [&](const IRStoreLocal& x) {
                emit_line("    mov rax, qword " + temp_slot(x.src, fn));
                emit_line("    mov qword " + local_slot(x.local) + ", rax");
            },

            [&](const IRAddrOf& x) {
                emit_line("    lea rax, qword " + local_slot(x.local));
                emit_line("    mov qword " + temp_slot(x.dst, fn) + ", rax");
            },

            [&](const IRBinary& x) {
                emit_binop(x, fn);
            },

            [&](const IRCall& x) {
                emit_call(program, fn, x);
            },

            [&](const IRReturn& x) {
                emit_line("    mov rax, qword " + temp_slot(x.value, fn));
                emit_line("    leave");
                emit_line("    ret");
            },

            [&](const IRJump& x) {
                emit_line("    jmp " + label_name(fn.id, x.target));
            },

            [&](const IRBranch& x) {
                emit_line("    mov rax, qword " + temp_slot(x.cond, fn));
                emit_line("    cmp rax, 0");
                emit_line("    jne " + label_name(fn.id, x.then_label));
                emit_line("    jmp " + label_name(fn.id, x.else_label));
            },

            [&](const IRLabel& x) {
                emit_line(label_name(fn.id, x.id) + ":");
            },

            [&](const IRGetField& x) {
                emit_line("    mov rax, qword " + temp_slot(x.base, fn));
                emit_line("    mov rax, qword [rax + " + std::to_string(x.offset) + "]");
                emit_line("    mov qword " + temp_slot(x.dst, fn) + ", rax");
            },

            [&](const IRSetField& x) {
                emit_line("    mov rax, qword " + temp_slot(x.base, fn));
                emit_line("    mov rbx, qword " + temp_slot(x.value, fn));
                emit_line("    mov qword [rax + " + std::to_string(x.offset) + "], rbx");
            },
            [&](const IRLoadString& x) {
                if (x.string_id >= program.strings.size()) {
                    crash("IRLoadString references invalid string id: " + std::to_string(x.string_id));
                }

                const auto& lit = program.strings[x.string_id];
                emit_line("    lea rax, [" + string_label(lit.id) + "]");
                emit_line("    mov qword " + temp_slot(x.dst, fn) + ", rax");
            },

            [&](const IRLoadElement& x) {
                const std::string ms = mem_size(x.elem_size);
                emit_line("    mov rax, qword " + temp_slot(x.base, fn));
                emit_line("    mov rcx, qword " + temp_slot(x.index, fn));
                if (x.elem_size <= 4) {
                    std::string src = ms + " [rax + rcx * " + std::to_string(x.elem_size) + "]";
                    if (x.elem_size < 4) {
                        emit_line("    movzx eax, " + src);
                    } else {
                        emit_line("    mov eax, " + src);
                    }
                } else {
                    emit_line("    mov rax, " + ms + " [rax + rcx * " + std::to_string(x.elem_size) + "]");
                }
                emit_line("    mov qword " + temp_slot(x.dst, fn) + ", rax");
            },

            [&](const IRStoreElement& x) {
                const std::string ms = mem_size(x.elem_size);
                emit_line("    mov rax, qword " + temp_slot(x.base, fn));
                emit_line("    mov rcx, qword " + temp_slot(x.index, fn));
                emit_line("    mov rbx, qword " + temp_slot(x.value, fn));
                emit_line("    mov " + ms + " [rax + rcx * " + std::to_string(x.elem_size) + "], " + reg_from_size(x.elem_size));
            },

            [&](const IRCast& x) {
                const std::string src = temp_slot(x.src, fn);
                const std::string dst = temp_slot(x.dst, fn);

                if (x.kind == ast::CastKind::Bitcast) {
                    emit_line("    mov rax, qword " + src);
                    emit_line("    mov qword " + dst + ", rax");
                    return;
                }

                const int src_sz = cast_type_size(x.src_kind);
                const int dst_sz = cast_type_size(x.target_kind);
                const bool src_int = is_integer(x.src_kind);
                const bool dst_int = is_integer(x.target_kind);
                const bool src_flt = is_float(x.src_kind);
                const bool dst_flt = is_float(x.target_kind);

                if (x.target_kind == ast::TypeKind::String) {
                    if (is_integer(x.src_kind)) {
                        const int src_sz = cast_type_size(x.src_kind);
                        const std::string ms = (src_sz == 4) ? "dword" : (src_sz == 2) ? "word" : "byte";
                        if (is_signed_int(x.src_kind)) {
                            if (src_sz == 4)      emit_line("    movsxd rdi, dword " + src);
                            else if (src_sz == 2) emit_line("    movsx rdi, word " + src);
                            else if (src_sz == 1) emit_line("    movsx rdi, byte " + src);
                            else                  emit_line("    mov rdi, qword " + src);
                        } else {
                            if (src_sz == 4)      emit_line("    mov rdi, " + ms + " " + src);
                            else if (src_sz == 2) emit_line("    movzx rdi, word " + src);
                            else if (src_sz == 1) emit_line("    movzx rdi, byte " + src);
                            else                  emit_line("    mov rdi, qword " + src);
                        }
                        if (is_signed_int(x.src_kind)) {
                            emit_line("    call qk_format_i64");
                        } else {
                            emit_line("    call qk_format_u64");
                        }
                    } else if (x.src_kind == ast::TypeKind::F32) {
                        emit_line("    movss xmm0, dword " + src);
                        emit_line("    cvtss2sd xmm0, xmm0");
                        emit_line("    movsd qword " + dst + ", xmm0");
                        emit_line("    mov rdi, qword " + dst);
                        emit_line("    call qk_format_f64");
                    } else {
                        emit_line("    mov rdi, qword " + src);
                        emit_line("    call qk_format_f64");
                    }
                    emit_line("    mov qword " + dst + ", rax");
                    return;
                }

                if (src_int && dst_int) {
                    if (src_sz == dst_sz) {
                        emit_line("    mov rax, qword " + src);
                    } else if (src_sz < dst_sz) {
                        if (is_signed_int(x.src_kind)) {
                            if (src_sz == 4) {
                                emit_line("    movsxd rax, dword " + src);
                            } else {
                                emit_line("    movsx rax, " + mem_size(src_sz) + " " + src);
                            }
                        } else {
                            if (src_sz == 4) {
                                emit_line("    mov eax, dword " + src);
                            } else {
                                emit_line("    movzx rax, " + mem_size(src_sz) + " " + src);
                            }
                        }
                    } else {
                        if (is_signed_int(x.target_kind)) {
                            if (dst_sz == 4) {
                                emit_line("    movsxd rax, dword " + src);
                            } else {
                                emit_line("    movsx rax, " + mem_size(dst_sz) + " " + src);
                            }
                        } else {
                            if (dst_sz == 4) {
                                emit_line("    mov eax, dword " + src);
                            } else {
                                emit_line("    movzx rax, " + mem_size(dst_sz) + " " + src);
                            }
                        }
                    }
                    emit_line("    mov qword " + dst + ", rax");

                } else if (src_int && dst_flt) {
                    if (dst_sz == 4) {
                        if (src_sz == 4) {
                            emit_line("    cvtsi2ss xmm0, dword " + src);
                        } else {
                            emit_line("    cvtsi2ss xmm0, qword " + src);
                        }
                        emit_line("    movss dword " + dst + ", xmm0");
                    } else {
                        if (src_sz == 4) {
                            emit_line("    cvtsi2sd xmm0, dword " + src);
                        } else {
                            emit_line("    cvtsi2sd xmm0, qword " + src);
                        }
                        emit_line("    movsd qword " + dst + ", xmm0");
                    }

                } else if (src_flt && dst_int) {
                    if (src_sz == 4) {
                        emit_line("    cvtss2si rax, dword " + src);
                    } else {
                        emit_line("    cvtsd2si rax, qword " + src);
                    }
                    emit_line("    mov qword " + dst + ", rax");

                } else if (src_flt && dst_flt) {
                    if (src_sz == 4 && dst_sz == 8) {
                        emit_line("    cvtss2sd xmm0, dword " + src);
                        emit_line("    movsd qword " + dst + ", xmm0");
                    } else if (src_sz == 8 && dst_sz == 4) {
                        emit_line("    cvtsd2ss xmm0, qword " + src);
                        emit_line("    movss dword " + dst + ", xmm0");
                    } else {
                        emit_line("    mov rax, qword " + src);
                        emit_line("    mov qword " + dst + ", rax");
                    }

                } else {
                    crash("Unsupported ValueCast in codegen");
                }
            },

            [&](const IRRegionBegin& x) {
                emit_region_begin(x, fn);
            },
            [&](const IRRegionAlloc& x) {
                emit_region_alloc(x, fn);
            },
            [&](const IRRegionEnd& x) {
                emit_region_end(x, fn);
            },
            [&](const IRAlloca& x) {
                // x.offset is the cumulative extra_stack at time of allocation
                // Place allocation below all local and temp slots
                const std::size_t base = (static_cast<std::size_t>(fn.local_count) +
                                          static_cast<std::size_t>(fn.temp_count)) * 8u;
                const std::size_t offset = base + static_cast<std::size_t>(x.offset);
                emit_line("    lea rax, [rbp - " + std::to_string(offset) + "]");
                emit_line("    mov qword " + temp_slot(x.dst, fn) + ", rax");
            },
            [&](const IRLoadGlobal& x) {
                emit_line("    mov rax, qword [" + global_label(x.global_id) + "]");
                emit_line("    mov qword " + temp_slot(x.dst, fn) + ", rax");
            },
            [&](const IRStoreGlobal& x) {
                emit_line("    mov rax, qword " + temp_slot(x.src, fn));
                emit_line("    mov qword [" + global_label(x.global_id) + "], rax");
            },
            [&](const IRLoadGlobalAddr& x) {
                emit_line("    lea rax, [" + global_label(x.global_id) + "]");
                emit_line("    mov qword " + temp_slot(x.dst, fn) + ", rax");
            },
        }, inst);
    }

    void FasmCodeGenerator::emit_region_begin(const IRRegionBegin& x, const IRFunction& fn) {
        (void)fn;
        emit_line("    mov rbx, qword " + local_slot(x.region_local));
        // mmap(0, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
        emit_line("    mov rdi, 0");
        emit_line("    mov rsi, " + std::to_string(x.region_size));
        emit_line("    mov rdx, 3");
        emit_line("    mov r10, 0x22");
        emit_line("    mov r8, -1");
        emit_line("    mov r9, 0");
        emit_line("    mov rax, 9");
        emit_line("    syscall");
        emit_line("    mov [rbx + 0], rax");
        emit_line("    mov qword [rbx + 8], 0");
        emit_line("    mov rax, " + std::to_string(x.region_size));
        emit_line("    mov [rbx + 16], rax");
    }

    void FasmCodeGenerator::emit_region_alloc(const IRRegionAlloc& x, const IRFunction& fn) {
        const uint32_t ok_label = region_alloc_label();

        emit_line("    mov rax, qword " + local_slot(x.region_local));
        emit_line("    mov rbx, [rax + 0]");
        emit_line("    add rbx, [rax + 8]");
        emit_line("    mov qword " + temp_slot(x.dst, fn) + ", rbx");

        emit_line("    mov rax, qword " + temp_slot(x.size, fn));
        emit_line("    add rax, 15");
        emit_line("    and rax, -16");

        emit_line("    mov rbx, qword " + local_slot(x.region_local));
        emit_line("    add [rbx + 8], rax");

        emit_line("    mov rax, [rbx + 8]");
        emit_line("    cmp rax, [rbx + 16]");
        emit_line("    jbe .region_ok_" + std::to_string(ok_label));
        emit_line("    mov rdi, 1");
        emit_line("    mov rax, 60");
        emit_line("    syscall");
        emit_line(".region_ok_" + std::to_string(ok_label) + ":");
    }

    void FasmCodeGenerator::emit_region_end(const IRRegionEnd& x, const IRFunction& fn) {
        (void)fn;
        emit_line("    mov rax, qword " + local_slot(x.region_local));
        emit_line("    mov rdi, [rax + 0]");
        emit_line("    mov rsi, [rax + 16]");
        // munmap(ptr, size)
        emit_line("    mov rax, 11");
        emit_line("    syscall");
    }
    std::string FasmCodeGenerator::generate(const IRProgram& program) {
        out.str("");
        out.clear();
        const bool win = (target_os == mc::TargetOS::Windows);

        if (win) {
            emit_line("format PE64 console");
            emit_line("entry start");
            emit_line();
            emit_line("section '.text' code readable executable");
        } else {
            emit_line("format ELF64");
            emit_line("section '.text' executable");
        }
        if (!win) {
            for (const auto& fn : program.functions) {
                if (fn.is_extern && fn.syscall_number < 0) {
                    emit_line("extrn " + abi_name(fn));
                }
            }
            emit_line("public _start");
        }
        for (const auto& fn : program.functions) {
            if (fn.is_extern) {
                if (fn.syscall_number >= 0) {
                    emit_line(abi_name(fn) + ":");
                    emit_line("    mov r10, rcx");
                    emit_line("    mov rax, " + std::to_string(fn.syscall_number));
                    emit_line("    syscall");
                    emit_line("    ret");
                    emit_line();
                }
                continue;
            }
            const std::size_t stack_size =
                align16(
                    (static_cast<std::size_t>(fn.local_count) +
                     static_cast<std::size_t>(fn.temp_count)) * 8u +
                     static_cast<std::size_t>(fn.extra_stack)
                );

            emit_line(function_name(fn) + ":");
            emit_line("    push rbp");
            emit_line("    mov rbp, rsp");

            if (stack_size > 0) {
                emit_line("    sub rsp, " + std::to_string(stack_size));
            }

            for (uint32_t i = 0; i < fn.arg_count; ++i) {
                if (win) {
                    if (i < 4) {
                        static const char* regs[] = {"rcx", "rdx", "r8", "r9"};
                        emit_line("    mov qword " + local_slot(i) + ", " + regs[i]);
                    } else {
                        emit_line("    mov rax, qword [rbp + " + std::to_string(16u + (i - 4u) * 8u) + "]");
                        emit_line("    mov qword " + local_slot(i) + ", rax");
                    }
                } else {
                    if (i < 6) {
                        static const char* regs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
                        emit_line("    mov qword " + local_slot(i) + ", " + regs[i]);
                    } else {
                        emit_line("    mov rax, qword [rbp + " + std::to_string(16u + (i - 6u) * 8u) + "]");
                        emit_line("    mov qword " + local_slot(i) + ", rax");
                    }
                }
            }

            for (const auto& inst : fn.body) {
                emit_inst(program, fn, inst);
            }

            emit_line();
        }

        // const IRFunction* main_fn = find_main_function(program);
        // if (!main_fn) {
        //     crash("No main function found");
        // }
        // Removed because the linker is checking all the things now

        if (win) {
        emit_line("start:");
        emit_line("    call qk_io_init");
        emit_line("    call " + function_name(*find_entry(program)));
        emit_line("    mov rcx, rax");
        emit_line("    call [ExitProcess]");

        emit_line();
        emit_line("section '.idata' import data readable writeable");
        emit_line("idata:");
        emit_line("    dd 0,0,0,rva kernel_name,rva kernel_table");
        emit_line("    dd 0,0,0,0,0");
        emit_line("kernel_table:");
        emit_line("ExitProcess dq rva _ExitProcess");
        emit_line("CreateFileA dq rva _CreateFileA");
        emit_line("CloseHandle dq rva _CloseHandle");
        emit_line("SetFilePointerEx dq rva _SetFilePointerEx");
        emit_line("FlushFileBuffers dq rva _FlushFileBuffers");
        emit_line("WriteFile dq rva _WriteFile");
        emit_line("ReadFile dq rva _ReadFile");
        emit_line("GetLastError dq rva _GetLastError");
        emit_line("GetStdHandle dq rva _GetStdHandle");
        emit_line("VirtualAlloc dq rva _VirtualAlloc");
        emit_line("VirtualFree dq rva _VirtualFree");
        emit_line("    dq 0");
        emit_line("_ExitProcess:");
        emit_line("    dw 0");
        emit_line("    db 'ExitProcess',0");
        emit_line("_CreateFileA:");
        emit_line("    dw 0");
        emit_line("    db 'CreateFileA',0");
        emit_line("_CloseHandle:");
        emit_line("    dw 0");
        emit_line("    db 'CloseHandle',0");
        emit_line("_SetFilePointerEx:");
        emit_line("    dw 0");
        emit_line("    db 'SetFilePointerEx',0");
        emit_line("_FlushFileBuffers:");
        emit_line("    dw 0");
        emit_line("    db 'FlushFileBuffers',0");
        emit_line("_WriteFile:");
        emit_line("    dw 0");
        emit_line("    db 'WriteFile',0");
        emit_line("_ReadFile:");
        emit_line("    dw 0");
        emit_line("    db 'ReadFile',0");
        emit_line("_GetLastError:");
        emit_line("    dw 0");
        emit_line("    db 'GetLastError',0");
        emit_line("_GetStdHandle:");
        emit_line("    dw 0");
        emit_line("    db 'GetStdHandle',0");
        emit_line("_VirtualAlloc:");
        emit_line("    dw 0");
        emit_line("    db 'VirtualAlloc',0");
        emit_line("_VirtualFree:");
        emit_line("    dw 0");
        emit_line("    db 'VirtualFree',0");
        emit_line("kernel_name db 'KERNEL32.DLL',0");
        } else {
        emit_line("_start:");
        emit_line("    call " + function_name(*find_entry(program)));
        emit_line("    mov rdi, rax");
        emit_line("    mov rax, 60");
        emit_line("    syscall");
        }
        // String literals are read-only; globals are writable and go to .data.
        if (!program.strings.empty()) {
            emit_line("section '.rodata' data readable");
            for (const auto& s : program.strings) {
                emit_line(string_label(s.id) + ":");
                emit_line(db_bytes(s.value));
            }
        }
        bool has_globals = false;
        for (const auto& g : program.globals) {
            if (!g.is_extern) { has_globals = true; break; }
        }
        if (has_globals) {
            emit_line(win ? "section '.data' data readable writeable"
                          : "section '.data' writeable");
        }
        for (uint32_t i = 0; i < program.globals.size(); ++i) {
            const auto& g = program.globals[i];
            if (g.is_extern) continue;
            emit_line(global_label(i) + ":");
            emit_line("    rb " + std::to_string(g.size));
        }
        return out.str();
    }
} // namespace quant::codegen
