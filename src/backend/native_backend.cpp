#include "quant/backend/native_backend.h"

#include "quant/backend/mc.h"
#include "quant/support/compiler_context.h"

#ifdef QUANT_HAS_ELF
#include "quant/backend/elf_writer.h"
#endif
#ifdef QUANT_HAS_PE
#include "quant/backend/pe_writer.h"
#endif
#ifdef QUANT_HAS_X86_64
#include "quant/backend/isel.h"
#endif
#ifdef QUANT_HAS_AARCH64
#include "quant/backend/aarch64_isel.h"
#endif

namespace quant::codegen {

namespace {

[[noreturn]] void backend_not_compiled(const char* name) {
    utils::logger::crash(std::string(name) +
                         " code generator is not compiled into this binary "
                         "(enable it via -DQUANT_BACKENDS=...)");
}

} // namespace

std::vector<uint8_t> NativeBackend::generate(const IRProgram& program, CompilerContext& ctx,
                                              const Target& target) {
    mc::Object obj;
    if (target.arch() == mc::TargetArch::AARCH64) {
#ifdef QUANT_HAS_AARCH64
        AArch64ISel isel(target);
        isel.should_emit_start = ctx.emit_start;
        isel.generate(program);
        obj = std::move(isel.obj);
#else
        backend_not_compiled("AArch64");
#endif
    } else {
#ifdef QUANT_HAS_X86_64
        ISel isel(ctx, target);
        isel.generate(program);
        obj = std::move(isel.obj);
#else
        backend_not_compiled("x86-64");
#endif
    }
    if (target.object_format() == mc::ObjectFormat::PE) {
#ifdef QUANT_HAS_PE
        return pe::write(obj, target.image_base());
#else
        backend_not_compiled("PE writer");
#endif
    }
#ifdef QUANT_HAS_ELF
    return elf::write(obj, target.arch());
#else
    backend_not_compiled("ELF writer");
#endif
}

} // namespace quant::codegen
