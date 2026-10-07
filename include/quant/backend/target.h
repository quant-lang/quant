#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include "quant/backend/mc.h"

namespace quant::codegen {

class CodeEmitter;

struct LinkSpec {
    std::string executable_ext;
    std::string default_linker;
    std::string extra_flags;
    bool needs_external_linker = true;
    bool needs_pre_link_validation = false;
};

class Target {
public:
    virtual ~Target() = default;
    virtual std::string_view name() const = 0;
    virtual mc::TargetArch   arch() const = 0;
    virtual mc::TargetOS     os() const = 0;
    virtual mc::TargetAbi    abi() const = 0;
    virtual mc::ObjectFormat object_format() const = 0;
    virtual uint64_t         image_base() const = 0;
    virtual void     emit_entry(CodeEmitter& out, const std::string& entry_name) const = 0;
    virtual void     emit_exit(CodeEmitter& out, int value) const = 0;
    virtual void     emit_syscall(CodeEmitter& out, uint64_t ir_number) const = 0;
    virtual uint64_t map_syscall(uint64_t ir_number) const = 0;
    virtual bool     supports_syscalls() const = 0;
    virtual void     emit_mem_reserve(CodeEmitter&, uint64_t size) const = 0;
    virtual void     emit_mem_release(CodeEmitter&) const = 0;
    virtual void     declare_runtime_imports(CodeEmitter&) const = 0;
    virtual bool     entry_emitted_first() const = 0;
    virtual std::string_view stdlib_root() const = 0;
    virtual bool  load_format_runtime() const = 0;
    virtual bool  validate_syscall(uint64_t n) const = 0;
    // Preserve the ABI display name used in existing diagnostics.
    virtual std::string syscall_abi_name() const { return std::string(name()); }
    virtual std::string syscall_surface() const = 0;
    virtual LinkSpec    link_spec() const = 0;
    virtual std::string static_stdlib_name() const = 0;
};

}
