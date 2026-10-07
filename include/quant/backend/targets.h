#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "quant/backend/mc.h"
#include "quant/backend/target.h"

namespace quant::codegen::mc {

// One entry of the target registry: a canonical CLI name plus its
// (arch, OS) pair and whether this compiler build can select it.
struct TargetInfo {
    std::string_view name;   // canonical spelling, e.g. "x86_64-linux"
    TargetArch arch;
    TargetOS os;
    const quant::codegen::Target* target = nullptr;

    // True when this build compiled the target's code generator in; a null
    // factory in the registry marks a known-but-disabled target.
    bool enabled() const { return target != nullptr; }
};

// All targets known to the compiler (enabled or not), in registry order.
// The enabled set is fixed at CMake configure time via QUANT_BACKENDS.
std::span<const TargetInfo> known_targets();

// Resolve a CLI spelling (canonical name or legacy alias like "x86_64",
// "arm64") to a registry entry. Returns nullptr for unknown spellings.
// A resolved-but-disabled entry is returned as-is so callers can tell
// "unknown target" apart from "known but not enabled".
const TargetInfo* resolve_target(const std::string& spelling);

// Canonical name of the target used when no --target is passed:
// the host-native backend ("x86_64-linux" on POSIX hosts, "x86_64-windows"
// on Windows), or the QUANT_DEFAULT_TARGET override chosen at CMake time.
std::string host_default_target_name();

// Comma-separated target names for diagnostics. only_enabled restricts the
// list to backends compiled into this build.
std::string target_names_csv(bool only_enabled = false);

} // namespace quant::codegen::mc
