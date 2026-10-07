#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "quant/ir/ir.h"
#include "quant/backend/mc.h"
#include "quant/backend/target.h"
#include "quant/backend/code_emitter.h"
#include "quant/backend/aarch_64.h"

namespace quant::codegen {

// AArch64 instruction selector: IRProgram -> mc::Object (AArch64 machine code).
struct AArch64ISel {
    aarch64::Emitter text;
    mc::Object obj;

    const Target& target;
    mc::SymbolTable symbols{obj.symbols};
    CodeEmitter out{text, obj, symbols};

    explicit AArch64ISel(const Target& selected) : target(selected) {}

    bool should_emit_start = true;

    void generate(const IRProgram& program);

private:
    struct Fixup {
        uint32_t field_offset;
        uint32_t key;
        bool region;
    };

    std::unordered_map<uint32_t, uint32_t> label_pos;
    std::unordered_map<uint32_t, uint32_t> region_label_pos;
    std::vector<Fixup> fixups;
    uint32_t next_region_label = 0;



    int64_t local_offset(Local l, const IRFunction& fn);
    int64_t temp_offset(Reg r, const IRFunction& fn);


    uint32_t region_alloc_label();

    void emit_func(const IRProgram& program, const IRFunction& fn);
    void emit_prologue(const IRFunction& fn);
    void emit_epilogue(const IRFunction& fn);
    void emit_inst(const IRProgram& program, const IRFunction& fn, const IRInst& inst);
    void emit_call(const IRProgram& program, const IRFunction& fn, const IRCall& x);
    void emit_syscall_stub(const IRFunction& fn);
    void emit_region_begin(const IRRegionBegin& x);
    void emit_region_alloc(const IRRegionAlloc& x, const IRFunction& fn);
    void emit_region_end(const IRRegionEnd& x);
    void emit_start(const IRProgram& program);
    void emit_strings(const IRProgram& program);
    void emit_globals(const IRProgram& program);
    void patch_fixups();
};

} // namespace quant::codegen
