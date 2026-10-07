#pragma once

#include <cstdint>
#include <vector>

#include "quant/ir/ir.h"
#include "quant/backend/mc.h"
#include "quant/support/compiler_context.h"

namespace quant::codegen {

// Full native pipeline: IR -> instruction selection -> machine code -> ELF object.
struct NativeBackend {
    std::vector<uint8_t> generate(const IRProgram& program, CompilerContext& ctx,
                                  const Target& target);
};

} // namespace quant::codegen
