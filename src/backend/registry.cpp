#include "quant/backend/targets.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>

namespace quant::codegen {

// Factories of every known target, guarded by the same QUANT_TARGET_<key>
// definitions that select the target source files in CMake.
#ifdef QUANT_TARGET_x86_64_linux
const Target* make_x86_64_linux();
#endif
#ifdef QUANT_TARGET_aarch64_linux
const Target* make_aarch64_linux();
#endif
#ifdef QUANT_TARGET_x86_64_windows
const Target* make_x86_64_windows();
#endif
#ifdef QUANT_TARGET_aarch64_zeropoint
const Target* make_aarch64_zeropoint();
#endif

} // namespace quant::codegen

namespace quant::codegen::mc {

namespace {

// A registry row before factories are resolved: canonical CLI name plus
// (arch, OS) metadata and the guarded factory pointer. Disabled targets keep
// their metadata so CLI diagnostics can distinguish "known but not enabled"
// from "unknown target"; a null factory marks a target whose code generator
// was not compiled into this build
struct Entry {
    std::string_view name;
    TargetArch arch;
    TargetOS os;
    const quant::codegen::Target* (*factory)();
};

constexpr Entry kEntries[] = {
    {"x86_64-linux", TargetArch::X86_64, TargetOS::Linux,
#ifdef QUANT_TARGET_x86_64_linux
     &make_x86_64_linux
#else
     nullptr
#endif
    },
    {"aarch64-linux", TargetArch::AARCH64, TargetOS::Linux,
#ifdef QUANT_TARGET_aarch64_linux
     &make_aarch64_linux
#else
     nullptr
#endif
    },
    {"x86_64-windows", TargetArch::X86_64, TargetOS::Windows,
#ifdef QUANT_TARGET_x86_64_windows
     &make_x86_64_windows
#else
     nullptr
#endif
    },
    {"aarch64-zeropoint", TargetArch::AARCH64, TargetOS::ZeroPoint,
#ifdef QUANT_TARGET_aarch64_zeropoint
     &make_aarch64_zeropoint
#else
     nullptr
#endif
    },
};

// Registry rows with factories resolved exactly once. A null factory marks a
// target whose code generator was not compiled into this build; its metadata
// is kept so CLI diagnostics can distinguish disabled from unknown targets.
const std::array<TargetInfo, std::size(kEntries)>& registry() {
    static const std::array table = [] {
        std::array<TargetInfo, std::size(kEntries)> result{};
        for (std::size_t i = 0; i < std::size(kEntries); ++i) {
            const Entry& e = kEntries[i];
            result[i] = {e.name, e.arch, e.os, e.factory ? e.factory() : nullptr};
        }
        return result;
    }();
    return table;
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Legacy CLI spellings kept for compatibility. Bare x86 names are
// host-relative: they select the host's native OS flavor (this preserves
// the historical behavior where the executable format followed the host).
// Bare AArch64 names always mean Linux; availability is decided by the
// enabled-backend set, not by the spelling.
std::optional<std::string_view> alias_to_canonical(std::string_view lower) {
#ifdef _WIN32
    constexpr bool kWindowsHost = true;
#else
    constexpr bool kWindowsHost = false;
#endif
    if (lower == "x86_64" || lower == "x86-64" || lower == "amd64") {
        return std::string_view(kWindowsHost ? "x86_64-windows" : "x86_64-linux");
    }
    if (lower == "aarch64" || lower == "arm64")
        return std::string_view("aarch64-linux");
    if (lower == "arm64-zeropoint")
        return std::string_view("aarch64-zeropoint");
    return std::nullopt;
}

} // namespace

std::span<const TargetInfo> known_targets() {
    return registry();
}

const TargetInfo* resolve_target(const std::string& spelling) {
    std::string key = to_lower(spelling);
    if (const auto canon = alias_to_canonical(key)) key = *canon;
    const auto& table = registry();
    const auto it = std::find_if(table.begin(), table.end(),
                                 [&](const TargetInfo& t) { return t.name == key; });
    return it == table.end() ? nullptr : &*it;
}

std::string host_default_target_name() {
#ifdef QUANT_DEFAULT_TARGET
    // CMake-time override (validated to be one of QUANT_BACKENDS).
    return QUANT_DEFAULT_TARGET;
#elif defined(_WIN32)
    return "x86_64-windows";
#else
    return "x86_64-linux";
#endif
}

std::string target_names_csv(bool only_enabled) {
    std::string out;
    for (const auto& t : registry()) {
        if (only_enabled && !t.enabled()) continue;
        if (!out.empty()) out += ", ";
        out += t.name;
    }
    return out;
}

} // namespace quant::codegen::mc
