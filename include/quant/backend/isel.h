#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "quant/backend/mc.h"
#include "quant/backend/target.h"
#include "quant/backend/code_emitter.h"
#include "quant/backend/x86_64.h"
#include "quant/ir/ir.h"
#include "quant/support/compiler_context.h"

namespace quant::codegen {

// IR -> x86-64 machine instructions (+ relocations), before the ELF writer.
struct ISel {
    mc::Object obj;
    x86::Emitter text;

    const Target& target;
    mc::SymbolTable symbols{obj.symbols};
    CodeEmitter out{text, obj, symbols};

    uint32_t next_region_label = 0;


    // label key -> text offset (IR labels and region labels are separate)
    std::unordered_map<uint32_t, uint32_t> label_pos;
    std::unordered_map<uint32_t, uint32_t> region_label_pos;

    struct Fixup {
        uint32_t field_offset;
        uint32_t key;
        bool region;
    };
    std::vector<Fixup> fixups;

    void generate(const IRProgram& program);

    ISel(CompilerContext& context, const Target& selected) : target(selected), ctx(context) {}

private:
    CompilerContext& ctx;

    x86::Mem local_slot(Local l);
    x86::Mem temp_slot(Reg r, const IRFunction& fn);


    void emit_func(const IRProgram& program, const IRFunction& fn);
    void emit_syscall_stub(const IRFunction& fn);
    void emit_prologue(const IRFunction& fn);
    void emit_inst(const IRProgram& program, const IRFunction& fn, const IRInst& inst);
    void emit_binop(const IRBinary& x, const IRFunction& fn);
    void emit_call(const IRProgram& program, const IRFunction& fn, const IRCall& x);
    void emit_load(Reg r, const IRFunction& fn);
    void emit_store(Reg r, const IRFunction& fn);
    void emit_region_begin(const IRRegionBegin& x);
    void emit_region_alloc(const IRRegionAlloc& x, const IRFunction& fn);
    void emit_region_end(const IRRegionEnd& x);
    void emit_shift(const IRShift& x, const IRFunction& fn);

    uint32_t region_alloc_label() { return next_region_label++; }

    void emit_start(const IRProgram& program);
    void emit_strings(const IRProgram& program);
    void emit_globals(const IRProgram& program);
    void patch_fixups();


};

} // namespace quant::codegen
