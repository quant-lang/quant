#pragma once

#include <cstdint>
#include <vector>

#include "quant/backend/mc.h"

namespace quant::codegen::pe {

// Serialize a machine-code object into a PE32+ executable (x86-64).
// Unlike the ELF writer (which produces a relocatable .o for `ld`), this
// resolves all relocations internally and emits a ready-to-run .exe with
// an import table for the DLLs referenced by the mc::Symbol import fields.
std::vector<uint8_t> write(const mc::Object& obj, uint64_t image_base);

} // namespace quant::codegen::pe
