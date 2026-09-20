#include "utils/options.h"
#include "utils/logger.h"

using namespace utils::logger;

namespace utils::options{

    namespace {

        // Apply a resolved registry entry to the options.
        void apply_target(Options& opts, const quant::codegen::mc::TargetInfo& t) {
            opts.target_name = t.name;
            opts.target_arch = t.arch;
            opts.target_os = t.os;
        }

        [[noreturn]] void fail_disabled(const std::string& name) {
            throw std::runtime_error(
                "Target '" + name + "' is known but not enabled in this compiler build.\n"
                "Enabled backends: " + quant::codegen::mc::enabled_targets_csv() + "\n"
                "Reconfigure with: cmake -B build -DQUANT_BACKENDS=\"...;" + name + "\"");
        }

    } // namespace

	Options parse_args(int argc, char** argv) {
        	Options opts;

        	std::unordered_map<std::string, Flag> flag_map = {
            	{"--emit-ir", Flag::EmitIR},
            	{"--emit-asm", Flag::EmitAsm},
            	{"--no-compile", Flag::NoCompile},
            	{"--time", Flag::Time},
                {"-c", Flag::CompileOnly},
                {"--static-lib", Flag::StaticLib}
        	};

        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];

            if (arg == "-o") {
                if (i + 1 >= argc) {
                    throw std::runtime_error("Option '-o' requires an output file");
                }
                opts.output_file = argv[++i];
                opts.has_output = true;
                continue;
            }
            if(arg == "--ar"){
                if (i + 1 >= argc) {
                    throw std::runtime_error("Option '--ar' requires an ar(GNU ar, LLVM etc.)");
                }
                opts.ar_name = argv[++i];
                continue;
            }
            if (arg == "--ld") {
                if (i + 1 >= argc) {
                    throw std::runtime_error("Option '--ld' requires a linker (ld, lld, mold, or path)");
                }
                opts.linker_name = argv[++i];
                continue;
            }

            if (arg == "--from-ast") {
                if (i + 1 >= argc) {
                    throw std::runtime_error("Option '--from-ast' requires a path to a QAST binary file");
                }
                opts.from_ast = argv[++i];
                continue;
            }

            if (arg == "-O0") { opts.opt_level = 0; continue; }
            if (arg == "-O1" || arg == "-O" || arg == "-Og") { opts.opt_level = 1; continue; }
            if (arg == "-O2" || arg == "-Os") { opts.opt_level = 2; continue; }
            if (arg == "-O3") { opts.opt_level = 3; continue; }

            if (arg == "--target") {
                if (i + 1 >= argc) {
                    throw std::runtime_error(
                        "Option '--target' requires a value. Known targets: "
                        + [&] {
                            std::string out;
                            for (const auto& t : quant::codegen::mc::known_targets()) {
                                if (!out.empty()) out += ", ";
                                out += t.name;
                            }
                            return out;
                        }());
                }
                std::string value = argv[++i];
                const auto* t = quant::codegen::mc::resolve_target(value);
                if (t == nullptr) {
                    std::string known;
                    for (const auto& k : quant::codegen::mc::known_targets()) {
                        if (!known.empty()) known += ", ";
                        known += k.name;
                    }
                    throw std::runtime_error("Unknown target: '" + value
                        + "'. Supported targets: " + known);
                }
                if (!t->enabled) {
                    fail_disabled(t->name);
                }
                apply_target(opts, *t);
                continue;
            }

            auto it = flag_map.find(arg);
            if (it != flag_map.end()) {
                switch (it->second) {
                    case Flag::EmitIR: opts.emit_ir = true; break;
                    case Flag::EmitAsm: opts.emit_asm = true; break;
                    case Flag::NoCompile: opts.no_compile = true; break;
                    case Flag::Time: opts.time = true; break;
                    case Flag::CompileOnly: opts.compile_only = true; break;
                    case Flag::StaticLib: opts.static_lib = true; break;
                }
            } else {
                if (!opts.input_file.empty()) {
                    fatal("Multiple input files are not supported: '"
                        + opts.input_file + "' and '" + arg + "'");
                }
                opts.input_file = arg;
            }
        }

        if (opts.input_file.empty() && opts.from_ast.empty()) {
            throw std::runtime_error("No input file provided");
        }

        // No explicit --target: use the host's native backend. Never fall
        // back to a different backend silently.
        if (opts.target_name.empty()) {
            const std::string def = quant::codegen::mc::host_default_target_name();
            const auto* t = quant::codegen::mc::resolve_target(def);
            if (t == nullptr || !t->enabled) {
                fail_disabled(def + " (default)");
            }
            apply_target(opts, *t);
        }

        return opts;
    }
}
