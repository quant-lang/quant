#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "quant/backend/mc.h"

namespace quant::codegen::x86 {

enum R64 : uint8_t {
    RAX = 0, RCX = 1, RDX = 2, RBX = 3,
    RSP = 4, RBP = 5, RSI = 6, RDI = 7,
    R8 = 8, R9 = 9,
    R10 = 10,
};

struct Mem {
    R64 base = RAX;
    bool has_index = false;
    R64 index = RAX;
    uint8_t scale = 1;
    int64_t disp = 0;
};

inline Mem mem_base(R64 base, int64_t disp) {
    return Mem{base, false, RAX, 1, disp};
}
inline Mem mem_index(R64 base, R64 index, uint8_t scale, int64_t disp = 0) {
    return Mem{base, true, index, scale, disp};
}

// Condition codes (low nibble, shared by Jcc/Setcc)
enum Cond : uint8_t {
    COND_B  = 0x2,
    COND_BE = 0x6,
    COND_A  = 0x7,
    COND_AE = 0x3,
    COND_E  = 0x4,
    COND_NE = 0x5,
    COND_L  = 0xC,
    COND_LE = 0xE,
    COND_G  = 0xF,
    COND_GE = 0xD,
};

enum class SseOp : uint8_t {
    Add = 0x58,
    Sub = 0x5C,
    Mul = 0x59,
    Div = 0x5E,
};

class Emitter {
public:
    std::vector<uint8_t> code;
    std::vector<mc::Relocation> relocs;

    void u8(uint8_t v);
    void u32(uint32_t v);
    void u64(uint64_t v);

    void reloc(const std::string& sym, mc::RelType type, int64_t addend);

    // mov
    void mov_r64_imm(R64 r, int64_t imm);
    void mov_r64_r64(R64 dst, R64 src);
    void mov_r32_imm(R64 r, int32_t imm);
    void mov_r64_mem(R64 r, const Mem& m);
    void mov_r32_mem(R64 r, const Mem& m);
    void mov_mem_r64(const Mem& m, R64 r);
    void mov_mem_r32(const Mem& m, R64 r);
    void mov_mem_r16(const Mem& m, R64 r);
    void mov_mem_r8(const Mem& m, R64 r);
    void lea_r64_mem(R64 r, const Mem& m);
    // lea rax, [rip + symbol]
    void lea_r64_rip(R64 r, const std::string& sym);
    // mov rax, [rip + symbol]
    void mov_r64_mem_rip(R64 r, const std::string& sym);
    // mov [rip + symbol], rax
    void mov_mem_r64_rip(R64 r, const std::string& sym);

    // extends
    void movzx_r64_mem8(R64 r, const Mem& m);
    void movzx_r32_mem8(R64 r, const Mem& m);
    void movzx_r64_mem16(R64 r, const Mem& m);
    void movzx_r32_mem16(R64 r, const Mem& m);
    void movsx_r64_mem8(R64 r, const Mem& m);
    void movsx_r64_mem16(R64 r, const Mem& m);
    void movsxd_r64_mem32(R64 r, const Mem& m);
    void movzx_r64_r8(R64 r, R64 src);

    // stack
    void push_r64(R64 r);
    void pop_r64(R64 r);
    void add_rsp_imm32(uint32_t imm);
    void sub_rsp_imm32(uint32_t imm);

    // integer alu
    void add_r64_r64(R64 dst, R64 src);
    void sub_r64_r64(R64 dst, R64 src);
    void and_r64_r64(R64 dst, R64 src);
    void or_r64_r64(R64 dst, R64 src);
    void xor_r64_r64(R64 dst, R64 src);
    void imul_r64_r64(R64 dst, R64 src);
    void add_r64_mem(R64 dst, const Mem& m);
    void add_mem_r64(const Mem& m, R64 src);
    void add_r64_imm8(R64 r, int8_t imm);
    void and_r64_imm8(R64 r, int8_t imm);
    void cmp_r64_r64(R64 a, R64 b);
    void cmp_r64_mem(R64 a, const Mem& m);
    void test_r64_r64(R64 a, R64 b);
    void cqo();
    void idiv_r64(R64 r);
    void div_r64(R64 r);

    // shifts: REX.W D3 /digit  (count in CL)
    void shl_r64_cl(R64 r);
    void shr_r64_cl(R64 r);
    void sar_r64_cl(R64 r);

    // shifts: REX.W C1 /digit ib  (immediate count)
    void shl_r64_imm8(R64 r, uint8_t imm);
    void shr_r64_imm8(R64 r, uint8_t imm);
    void sar_r64_imm8(R64 r, uint8_t imm);

    // control flow
    void setcc(Cond c, R64 byte_reg);
    // rel32 targets are patched by the caller (labels) or via relocations (calls)
    void jcc_rel32(Cond c);
    void jmp_rel32();
    void call_rel32(const std::string& sym);
    // call qword [rip + sym] — indirect call through an import IAT slot
    void call_mem_rip(const std::string& sym);

    // sse (register operands are xmm indices 0-15, not R64)
    void movsd_r64_mem(uint8_t xmm, const Mem& m);
    void movss_r64_mem(uint8_t xmm, const Mem& m);
    void movsd_mem_r64(const Mem& m, uint8_t xmm);
    void movss_mem_r64(const Mem& m, uint8_t xmm);
    void arith_sd(SseOp op, uint8_t dst, uint8_t src);
    void arith_ss(SseOp op, uint8_t dst, uint8_t src);
    void comisd(uint8_t a, uint8_t b);
    void comiss(uint8_t a, uint8_t b);
    void cvt_i64_to_sd(uint8_t xmm, const Mem& m);
    void cvt_i32_to_sd(uint8_t xmm, const Mem& m);
    void cvt_i64_to_ss(uint8_t xmm, const Mem& m);
    void cvt_i32_to_ss(uint8_t xmm, const Mem& m);
    void cvt_sd_to_i64(R64 dst, const Mem& m);
    void cvt_ss_to_i64(R64 dst, const Mem& m);
    void cvt_ss_to_sd(uint8_t xmm, const Mem& m);
    void cvt_sd_to_ss(uint8_t xmm, const Mem& m);
    void cvt_ss_to_sd_rr(uint8_t dst, uint8_t src);

    // misc
    void leave();
    void ret();
    void syscall();
};

} // namespace quant::codegen::x86
