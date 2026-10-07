#pragma once
#include "quant/backend/target.h"

namespace quant::codegen::linux_policy {
    inline constexpr mc::ObjectFormat object_format() { return mc::ObjectFormat::ELF; }
    inline constexpr std::string_view stdlib_root() { return ""; }
    inline constexpr bool load_format_runtime() { return true; }
    inline constexpr bool validate_syscall(uint64_t) { return true; }
    inline std::string syscall_surface() { return {}; }

    inline std::string static_stdlib_name(std::string_view name) {
        return "libqu-" + std::string(name) + ".a";
    }
} // namespace quant::codegen::linux_policy
