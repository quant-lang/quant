#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "quant/ir/ir.h"

namespace quant::codegen {

enum class OptLevel {
    O0 = 0,
    O1 = 1,
    O2 = 2,
    O3 = 3,
};

// Optional statistics collected by optimize(): per-pass wall time and the
// number of times each pass reported a change, plus IR size before/after.
struct OptStats {
    struct Pass {
        std::string name;
        uint64_t nanoseconds = 0;
        uint32_t invocations = 0;
        uint32_t changes = 0;
    };

    std::vector<Pass> passes;
    size_t insts_before = 0;
    size_t insts_after = 0;
    size_t functions_before = 0;
    size_t functions_after = 0;
    uint32_t loops_folded = 0;
    uint32_t branches_folded = 0;

    Pass& pass(const std::string& name);
};

// Passes can be disabled individually with the QUANT_SKIP environment
// variable (substring match), e.g. QUANT_SKIP="loop,dse". Pass names:
//   fold    constant folding / algebraic simplification / cast folding
//   branch  folding of branches on constant conditions (subset of fold)
//   locals  constant & copy propagation through local slots
//   cfg     jump threading, unreachable code and unused label removal
//   loop    compile-time evaluation of pure loops (O2+)
//   dead    dead temp elimination
//   dse     dead store elimination for locals
//   prune   whole-program removal of unused functions / strings / globals (O2+)
void optimize(IRProgram& program, OptLevel level, bool keep_all_functions = false,
              OptStats* stats = nullptr);

} // namespace quant::codegen
