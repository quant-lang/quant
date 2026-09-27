#pragma once

#include <iostream>
#include <string>
#include <unordered_map>
#include <stdexcept>

#include "logger.h"
#include "quant/backend/mc.h"
#include "quant/backend/targets.h"

using namespace utils::logger;

namespace utils::options {

    using quant::codegen::mc::TargetArch;
    using quant::codegen::mc::TargetOS;

    struct Options {
        bool emit_ir = false;
        bool emit_asm = false;
        bool no_compile = false;
        bool time = false;
        std::string input_file;
        std::string output_file;
        bool has_output = false;
        bool compile_only = false;
        bool static_lib = false;

        std::string ar_name;
        std::string linker_name;

        // Optimization level, -O0 .. -O3 (default -O2). -Og maps to -O1,
        // -Os maps to -O2.
        int opt_level = 2;

        // Resolved compilation target. Defaults to the host's native backend
        // if it is enabled in this build; otherwise --target is mandatory.
        std::string target_name;   // canonical, e.g. "aarch64-zeropoint"
        TargetArch target_arch = TargetArch::X86_64;
        TargetOS target_os = TargetOS::Linux;
    };

    enum class Flag {
        EmitIR,
        EmitAsm,
        NoCompile,
        Time,
        CompileOnly,
        StaticLib,
    };

    Options parse_args(int argc, char** argv);
}
