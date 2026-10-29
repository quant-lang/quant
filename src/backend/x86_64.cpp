#include "quant/backend/x86_64.h"

namespace quant::codegen::x86 {

namespace {

struct EncMem {
    uint8_t mod = 0;
    uint8_t rm = 0;
    bool sib = false;
    uint8_t sib_byte = 0;
    int32_t disp = 0;
};

// Encode a memory operand into mod/rm (+optional SIB + displacement).
EncMem enc_mem(const Mem& m) {
    EncMem em;
    const uint8_t base3 = static_cast<uint8_t>(m.base) & 7u;
    const bool has_index = m.has_index;
    const bool need_sib = has_index || base3 == 4u; // RSP/R12 require SIB

    const int64_t disp = m.disp;
    if (disp == 0 && base3 != 5u) {
        em.mod = 0;
        em.disp = 0;
    } else if (disp >= -128 && disp <= 127) {
        em.mod = 1;
        em.disp = static_cast<int8_t>(disp);
    } else {
        em.mod = 2;
        em.disp = static_cast<int32_t>(disp);
    }
    // [rbp]/[rbp+index] with disp==0 is not encodable in mod 00 (that would be
    // an absolute disp32 / SIB base==5). Force a disp8 form.
    if (em.mod == 0 && base3 == 5u) {
        em.mod = 1;
        em.disp = 0;
    }

    if (need_sib) {
        uint8_t scale_bits = 0;
        switch (m.scale) {
            case 1: scale_bits = 0; break;
            case 2: scale_bits = 1; break;
            case 4: scale_bits = 2; break;
            default: scale_bits = 3; break;
        }
        const uint8_t idx = has_index ? (static_cast<uint8_t>(m.index) & 7u) : 4u;
        em.sib_byte = static_cast<uint8_t>((scale_bits << 6) | (idx << 3) | base3);
        em.rm = 4;
        em.sib = true;
    } else {
        em.rm = base3;
    }
    return em;
}

void emit_modrm_mem(Emitter& e, uint8_t opcode, bool rex_w, uint8_t reg, const Mem& m) {
    const EncMem em = enc_mem(m);
    uint8_t rex = 0x40 | (rex_w ? 0x08 : 0x00);
    if (reg >= 8)  rex |= 0x04;
    if (m.has_index && m.index >= R8) rex |= 0x02;
    if (m.base >= R8)                 rex |= 0x01;
    if (rex != 0x40) e.u8(rex);
    e.u8(opcode);
    e.u8(static_cast<uint8_t>((em.mod << 6) | ((reg & 7u) << 3) | em.rm));
    if (em.sib) e.u8(em.sib_byte);
    if (em.mod == 1) {
        e.u8(static_cast<uint8_t>(em.disp));
    } else if (em.mod == 2) {
        e.u32(static_cast<uint32_t>(em.disp));
    }
}

void emit_modrm_reg(Emitter& e, uint8_t opcode, bool rex_w, uint8_t reg, uint8_t rm) {
    uint8_t rex = 0x40 | (rex_w ? 0x08 : 0x00);
    if (reg >= 8) rex |= 0x04;
    if (rm >= 8)  rex |= 0x01;
    if (rex != 0x40) e.u8(rex);
    e.u8(opcode);
    e.u8(static_cast<uint8_t>((3u << 6) | ((reg & 7u) << 3) | (rm & 7u)));
}

// Two-byte opcode (0F xx) with register/memory operand.
void emit_modrm_mem2(Emitter& e, uint8_t opcode2, bool rex_w, uint8_t reg, const Mem& m) {
    const EncMem em = enc_mem(m);
    uint8_t rex = 0x40 | (rex_w ? 0x08 : 0x00);
    if (reg >= 8)  rex |= 0x04;
    if (m.has_index && m.index >= R8) rex |= 0x02;
    if (m.base >= R8)                 rex |= 0x01;
    if (rex != 0x40) e.u8(rex);
    e.u8(0x0F);
    e.u8(opcode2);
    e.u8(static_cast<uint8_t>((em.mod << 6) | ((reg & 7u) << 3) | em.rm));
    if (em.sib) e.u8(em.sib_byte);
    if (em.mod == 1) {
        e.u8(static_cast<uint8_t>(em.disp));
    } else if (em.mod == 2) {
        e.u32(static_cast<uint32_t>(em.disp));
    }
}

// Two-byte opcode (0F xx) with register/register operand.
void emit_modrm_reg2(Emitter& e, uint8_t opcode2, bool rex_w, uint8_t reg, uint8_t rm) {
    uint8_t rex = 0x40 | (rex_w ? 0x08 : 0x00);
    if (reg >= 8) rex |= 0x04;
    if (rm >= 8)  rex |= 0x01;
    if (rex != 0x40) e.u8(rex);
    e.u8(0x0F);
    e.u8(opcode2);
    e.u8(static_cast<uint8_t>((3u << 6) | ((reg & 7u) << 3) | (rm & 7u)));
}

} // namespace

void Emitter::u8(uint8_t v) { code.push_back(v); }
void Emitter::u32(uint32_t v) {
    for (int i = 0; i < 4; ++i) code.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
void Emitter::u64(uint64_t v) {
    for (int i = 0; i < 8; ++i) code.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

void Emitter::reloc(const std::string& sym, mc::RelType type, int64_t addend) {
    relocs.push_back({static_cast<uint32_t>(code.size()), sym, type, addend});
}

// mov

void Emitter::mov_r64_imm(R64 r, int64_t imm) {
    uint8_t rex = 0x48 | (r >= 8 ? 0x01 : 0x00);
    if (rex != 0x40) u8(rex);
    u8(static_cast<uint8_t>(0xB8 + (static_cast<uint8_t>(r) & 7u)));
    u64(static_cast<uint64_t>(imm));
}
void Emitter::mov_r64_r64(R64 dst, R64 src) { emit_modrm_reg(*this, 0x89, true, src, dst); }
void Emitter::mov_r32_imm(R64 r, int32_t imm) {
    if (r >= 8) u8(0x41);
    u8(static_cast<uint8_t>(0xB8 + (static_cast<uint8_t>(r) & 7u)));
    u32(static_cast<uint32_t>(imm));
}
void Emitter::mov_r64_mem(R64 r, const Mem& m) { emit_modrm_mem(*this, 0x8B, true, r, m); }
void Emitter::mov_r32_mem(R64 r, const Mem& m) { emit_modrm_mem(*this, 0x8B, false, r, m); }
void Emitter::mov_mem_r64(const Mem& m, R64 r) { emit_modrm_mem(*this, 0x89, true, r, m); }
void Emitter::mov_mem_r32(const Mem& m, R64 r) { emit_modrm_mem(*this, 0x89, false, r, m); }
void Emitter::mov_mem_r16(const Mem& m, R64 r) { u8(0x66); emit_modrm_mem(*this, 0x89, false, r, m); }
void Emitter::mov_mem_r8(const Mem& m, R64 r) { emit_modrm_mem(*this, 0x88, false, r, m); }
void Emitter::lea_r64_mem(R64 r, const Mem& m) { emit_modrm_mem(*this, 0x8D, true, r, m); }
void Emitter::lea_r64_rip(R64 r, const std::string& sym) {
    uint8_t rex = 0x48 | (r >= 8 ? 0x04 : 0x00);
    if (rex != 0x40) u8(rex);
    u8(0x8D);
    u8(static_cast<uint8_t>((0u << 6) | ((static_cast<uint8_t>(r) & 7u) << 3) | 5u));
    reloc(sym, mc::RelType::X86_64_PC32, -4);
    u32(0);
}
void Emitter::mov_r64_mem_rip(R64 r, const std::string& sym) {
    uint8_t rex = 0x48 | (r >= 8 ? 0x04 : 0x00);
    if (rex != 0x40) u8(rex);
    u8(0x8B);
    u8(static_cast<uint8_t>((0u << 6) | ((static_cast<uint8_t>(r) & 7u) << 3) | 5u));
    reloc(sym, mc::RelType::X86_64_PC32, -4);
    u32(0);
}
void Emitter::mov_mem_r64_rip(R64 r, const std::string& sym) {
    uint8_t rex = 0x48 | (r >= 8 ? 0x04 : 0x00);
    if (rex != 0x40) u8(rex);
    u8(0x89);
    u8(static_cast<uint8_t>((0u << 6) | ((static_cast<uint8_t>(r) & 7u) << 3) | 5u));
    reloc(sym, mc::RelType::X86_64_PC32, -4);
    u32(0);
}

// extends

void Emitter::movzx_r64_mem8(R64 r, const Mem& m)  { emit_modrm_mem2(*this, 0xB6, true,  r, m); }
void Emitter::movzx_r32_mem8(R64 r, const Mem& m)  { emit_modrm_mem2(*this, 0xB6, false, r, m); }
void Emitter::movzx_r64_mem16(R64 r, const Mem& m) { emit_modrm_mem2(*this, 0xB7, true,  r, m); }
void Emitter::movzx_r32_mem16(R64 r, const Mem& m) { emit_modrm_mem2(*this, 0xB7, false, r, m); }
void Emitter::movsx_r64_mem8(R64 r, const Mem& m)  { emit_modrm_mem2(*this, 0xBE, true,  r, m); }
void Emitter::movsx_r64_mem16(R64 r, const Mem& m) { emit_modrm_mem2(*this, 0xBF, true,  r, m); }
void Emitter::movsxd_r64_mem32(R64 r, const Mem& m) { emit_modrm_mem(*this, 0x63, true, r, m); }
void Emitter::movzx_r64_r8(R64 r, R64 src) {
    uint8_t rex = 0x48;
    if (r >= 8) rex |= 0x04;
    if (src >= 8) rex |= 0x01;
    if (rex != 0x40) u8(rex);
    u8(0x0F);
    u8(0xB6);
    u8(static_cast<uint8_t>((3u << 6) | ((static_cast<uint8_t>(src) & 7u) << 3) | (static_cast<uint8_t>(r) & 7u)));
}

// stack

void Emitter::push_r64(R64 r) {
    if (r >= 8) u8(0x41);
    u8(static_cast<uint8_t>(0x50 + (static_cast<uint8_t>(r) & 7u)));
}
void Emitter::pop_r64(R64 r) {
    if (r >= 8) u8(0x41);
    u8(static_cast<uint8_t>(0x58 + (static_cast<uint8_t>(r) & 7u)));
}
void Emitter::add_rsp_imm32(uint32_t imm) {
    u8(0x48); u8(0x81); u8(0xC4); u32(imm);
}
void Emitter::sub_rsp_imm32(uint32_t imm) {
    u8(0x48); u8(0x81); u8(0xEC); u32(imm);
}

// integer alu

void Emitter::add_r64_r64(R64 dst, R64 src) { emit_modrm_reg(*this, 0x01, true, src, dst); }
void Emitter::sub_r64_r64(R64 dst, R64 src) { emit_modrm_reg(*this, 0x29, true, src, dst); }
void Emitter::and_r64_r64(R64 dst, R64 src) { emit_modrm_reg(*this, 0x21, true, src, dst); }
void Emitter::or_r64_r64(R64 dst, R64 src)  { emit_modrm_reg(*this, 0x09, true, src, dst); }
void Emitter::xor_r64_r64(R64 dst, R64 src) { emit_modrm_reg(*this, 0x31, true, src, dst); }
void Emitter::imul_r64_r64(R64 dst, R64 src) {
    uint8_t rex = 0x48;
    if (dst >= 8) rex |= 0x01;
    if (src >= 8) rex |= 0x04;
    if (rex != 0x40) u8(rex);
    u8(0x0F);
    u8(0xAF);
    u8(static_cast<uint8_t>((3u << 6) | ((static_cast<uint8_t>(dst) & 7u) << 3) | (static_cast<uint8_t>(src) & 7u)));
}
void Emitter::add_r64_mem(R64 dst, const Mem& m) { emit_modrm_mem(*this, 0x03, true, dst, m); }
void Emitter::add_mem_r64(const Mem& m, R64 src) { emit_modrm_mem(*this, 0x01, true, src, m); }
void Emitter::add_r64_imm8(R64 r, int8_t imm) { emit_modrm_reg(*this, 0x83, true, 0, r); u8(static_cast<uint8_t>(imm)); }
void Emitter::and_r64_imm8(R64 r, int8_t imm) { emit_modrm_reg(*this, 0x83, true, 4, r); u8(static_cast<uint8_t>(imm)); }
void Emitter::cmp_r64_r64(R64 a, R64 b) { emit_modrm_reg(*this, 0x39, true, b, a); }
void Emitter::cmp_r64_mem(R64 a, const Mem& m) { emit_modrm_mem(*this, 0x3B, true, a, m); }
void Emitter::test_r64_r64(R64 a, R64 b) { emit_modrm_reg(*this, 0x85, true, b, a); }
void Emitter::cqo() { u8(0x48); u8(0x99); }
void Emitter::idiv_r64(R64 r) { emit_modrm_reg(*this, 0xF7, true, 7, r); }
void Emitter::div_r64(R64 r) { emit_modrm_reg(*this, 0xF7, true, 6, r); }

// control flow

void Emitter::setcc(Cond c, R64 byte_reg) {
    uint8_t rex = 0;
    if (byte_reg >= 8) rex |= 0x01;
    if (rex != 0) u8(0x40 | rex);
    u8(0x0F);
    u8(static_cast<uint8_t>(0x90 + static_cast<uint8_t>(c)));
    u8(static_cast<uint8_t>((3u << 6) | (static_cast<uint8_t>(byte_reg) & 7u)));
}
void Emitter::jcc_rel32(Cond c) {
    u8(0x0F);
    u8(static_cast<uint8_t>(0x80 + static_cast<uint8_t>(c)));
    u32(0);
}
void Emitter::jmp_rel32() { u8(0xE9); u32(0); }
void Emitter::call_rel32(const std::string& sym) {
    u8(0xE8);
    reloc(sym, mc::RelType::X86_64_PLT32, -4);
    u32(0);
}
void Emitter::call_mem_rip(const std::string& sym) {
    u8(0xFF);
    u8(0x15);
    reloc(sym, mc::RelType::X86_64_PC32, -4);
    u32(0);
}
// shifts: REX.W D3 /digit  (count in CL)
void Emitter::shl_r64_cl(R64 r) { emit_modrm_reg(*this, 0xD3, true, 4, r); }
void Emitter::shr_r64_cl(R64 r) { emit_modrm_reg(*this, 0xD3, true, 5, r); }
void Emitter::sar_r64_cl(R64 r) { emit_modrm_reg(*this, 0xD3, true, 7, r); }

// shifts: REX.W C1 /digit ib  (immediate count)
void Emitter::shl_r64_imm8(R64 r, uint8_t imm) { emit_modrm_reg(*this, 0xC1, true, 4, r); u8(imm); }
void Emitter::shr_r64_imm8(R64 r, uint8_t imm) { emit_modrm_reg(*this, 0xC1, true, 5, r); u8(imm); }
void Emitter::sar_r64_imm8(R64 r, uint8_t imm) { emit_modrm_reg(*this, 0xC1, true, 7, r); u8(imm); }

// sse

void Emitter::movsd_r64_mem(uint8_t xmm, const Mem& m) { u8(0xF2); emit_modrm_mem2(*this, 0x10, false, xmm, m); }
void Emitter::movss_r64_mem(uint8_t xmm, const Mem& m) { u8(0xF3); emit_modrm_mem2(*this, 0x10, false, xmm, m); }
void Emitter::movsd_mem_r64(const Mem& m, uint8_t xmm) { u8(0xF2); emit_modrm_mem2(*this, 0x11, false, xmm, m); }
void Emitter::movss_mem_r64(const Mem& m, uint8_t xmm) { u8(0xF3); emit_modrm_mem2(*this, 0x11, false, xmm, m); }
void Emitter::arith_sd(SseOp op, uint8_t dst, uint8_t src) { u8(0xF2); emit_modrm_reg2(*this, static_cast<uint8_t>(op), false, dst, src); }
void Emitter::arith_ss(SseOp op, uint8_t dst, uint8_t src) { u8(0xF3); emit_modrm_reg2(*this, static_cast<uint8_t>(op), false, dst, src); }
void Emitter::comisd(uint8_t a, uint8_t b) { u8(0x66); emit_modrm_reg2(*this, 0x2F, false, b, a); }
void Emitter::comiss(uint8_t a, uint8_t b) { emit_modrm_reg2(*this, 0x2F, false, b, a); }

void Emitter::cvt_i64_to_sd(uint8_t xmm, const Mem& m) { u8(0xF2); emit_modrm_mem2(*this, 0x2A, true, xmm, m); }
void Emitter::cvt_i32_to_sd(uint8_t xmm, const Mem& m) { u8(0xF2); emit_modrm_mem2(*this, 0x2A, false, xmm, m); }
void Emitter::cvt_i64_to_ss(uint8_t xmm, const Mem& m) { u8(0xF3); emit_modrm_mem2(*this, 0x2A, true, xmm, m); }
void Emitter::cvt_i32_to_ss(uint8_t xmm, const Mem& m) { u8(0xF3); emit_modrm_mem2(*this, 0x2A, false, xmm, m); }
void Emitter::cvt_sd_to_i64(R64 dst, const Mem& m) { u8(0xF2); emit_modrm_mem2(*this, 0x2C, true, dst, m); }
void Emitter::cvt_ss_to_i64(R64 dst, const Mem& m) { u8(0xF3); emit_modrm_mem2(*this, 0x2C, true, dst, m); }
void Emitter::cvt_ss_to_sd(uint8_t xmm, const Mem& m) { u8(0xF3); emit_modrm_mem2(*this, 0x5A, false, xmm, m); }
void Emitter::cvt_sd_to_ss(uint8_t xmm, const Mem& m) { u8(0xF2); emit_modrm_mem2(*this, 0x5A, false, xmm, m); }
void Emitter::cvt_ss_to_sd_rr(uint8_t dst, uint8_t src) { u8(0xF3); emit_modrm_reg2(*this, 0x5A, false, dst, src); }

// misc

void Emitter::leave() { u8(0xC9); }
void Emitter::ret() { u8(0xC3); }
void Emitter::syscall() { u8(0x0F); u8(0x05); }

} // namespace quant::codegen::x86
