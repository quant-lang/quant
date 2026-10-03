#include <filesystem>
#include <iostream>
#include <chrono>
#include <fstream>
#include <cstdlib>

#ifndef QUANT_USE_LIBQU
#define QUANT_USE_LIBQU 0
#endif

#if QUANT_USE_LIBQU
#include <cstdint>
#include <sstream>
#include <unistd.h>
#include "quant_libqu_embedded.h"
#endif

#include "quant/frontend/lexer.h"
#include "quant/frontend/parser.h"
#include "quant/semantic/semantic.h"
#include "quant/support/compiler_context.h"

#include "utils/options.h"
#include "utils/logger.h"

#include "quant/ir/ir_gen.h"
#include "quant/ir/opt.h"
#include "quant/backend/native_backend.h"

#include "quant/modules/module.h"
#include "quant/linker/linker.h"

#if QUANT_USE_LIBQU
namespace {

// Content-addressed file name: builds embedding the same stdlib share one
// archive in the temp dir, and a changed stdlib gets a different name.
std::string libqu_cache_name(const quant::embedded_libqu::Archive& archive) {
    std::uint64_t hash = 1469598103934665603ull; // FNV-1a
    for (std::uint64_t i = 0; i < archive.size; ++i) {
        hash ^= archive.data[i];
        hash *= 1099511628211ull;
    }
    std::ostringstream name;
    name << "libqu-" << archive.target << "-" << std::hex << hash << ".a";
    return name.str();
}

// Drop the embedded archive into the temp dir so ld can read it. Returns the
// path in `out_path`.
bool unpack_libqu(const quant::embedded_libqu::Archive& archive, std::filesystem::path& out_path, std::string& error) {
    out_path = std::filesystem::temp_directory_path() / libqu_cache_name(archive);

    std::error_code ec;
    const std::uintmax_t expected = archive.size;
    if (std::filesystem::exists(out_path, ec) &&
        std::filesystem::file_size(out_path, ec) == expected) {
        return true;
    }

    // Write to a private name first: parallel qu processes then either see the
    // finished archive or rename an identical one over it, never a partial one.
    std::filesystem::path partial = out_path;
    partial += "." + std::to_string(::getpid()) + ".tmp";

    std::ofstream file(partial, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "cannot create " + partial.string();
        return false;
    }
    file.write(reinterpret_cast<const char*>(archive.data),
               static_cast<std::streamsize>(archive.size));
    if (!file) {
        error = "cannot write " + partial.string();
        return false;
    }
    file.close();

    std::filesystem::rename(partial, out_path, ec);
    if (ec) {
        error = "cannot install " + out_path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

} // namespace
#endif // QUANT_USE_LIBQU

int main(int argc, char **argv)
{
    try{
        using namespace std::chrono;

        auto opts = utils::options::parse_args(argc, argv);

        // Target selection is fully resolved by parse_args against the
        // backends enabled at CMake configure time (QUANT_BACKENDS).

        auto start = high_resolution_clock::now();

        quant::CompilerContext ctx;
        ctx.target_os = opts.target_os;
        ctx.emit_start = !opts.static_lib;

        {
            ctx.root_path = utils::io::get_executable_directory();
            // If binary is in build/ subdirectory, use parent (source root)
            auto parent = ctx.root_path.parent_path();
            if (std::filesystem::exists(parent / "std" / "io" / "io.qu")) {
                ctx.root_path = parent;
            }
        }

        // QUANT_USE_LIBQU: the prebuilt stdlib archive is part of this binary.
        // Unpack it and link against it instead of regenerating stdlib code
        // from the embedded sources on every run.
#if QUANT_USE_LIBQU
        if (opts.target_os == quant::codegen::mc::TargetOS::Linux && !opts.static_lib) {
            for (const auto& archive : quant::embedded_libqu::archives()) {
                if (archive.target != opts.target_name) continue;

                std::string error;
                if (!unpack_libqu(archive, ctx.static_std_path, error)) {
                    utils::logger::error("failed to unpack embedded stdlib: " + error);
                    return 1;
                }
                ctx.use_static_std = true;
                break;
            }
        }
#endif

        // Builds without an embedded archive fall back to a pre-compiled
        // stdlib (.a) shipped next to the binary: lib/qu-<arch>-<os>.a, then
        // the same name in the parent directory (build layout).
        if (!ctx.use_static_std &&
            opts.target_os == quant::codegen::mc::TargetOS::Linux && !opts.static_lib) {
            const char* arch_str = (opts.target_arch == quant::codegen::mc::TargetArch::AARCH64)
                ? "aarch64" : "x86_64";
            auto lib_name = std::string("libqu-") + arch_str + "-linux.a";
            auto lib_path = ctx.root_path / "lib" / lib_name;
            if (!std::filesystem::exists(lib_path)) {
                lib_path = ctx.root_path.parent_path() / "lib" / lib_name;
            }
            if (std::filesystem::exists(lib_path)) {
                ctx.use_static_std = true;
                ctx.static_std_path = lib_path;
            }
        }

        quant::modules::ModuleManager mm(ctx);
        quant::linker::Linker linker(mm, ctx);

        quant::modules::Module* entry = nullptr;

        if (opts.input_file.empty()) {
            utils::logger::error("No input file provided");
            return 1;
        }

        if (!std::filesystem::path(opts.input_file).has_extension()) {
            opts.input_file += ".qu";
        }

        entry = mm.load_entry(opts.input_file);

        if (!entry) {
            utils::logger::error("Failed to load entry module");
            return 1;
        }

        // Always compile the pure-Quant format runtime (used by `as str` casts).
        // It ships embedded in the binary; fall back to the source tree in dev builds.
        // Skipped on ZeroPoint: format depends on std::heap, and the ZeroPoint
        // ABI has no allocation syscalls yet (kernel malloc is TODO), so the
        // runtime is unusable there and its mmap syscalls are invalid.
        if (opts.target_os != quant::codegen::mc::TargetOS::ZeroPoint &&
            mm.load_embedded("std::format") == nullptr) {
            auto format_path = ctx.root_path / "std" / "format" / "format.qu";
            if (std::filesystem::exists(format_path)) {
                mm.load_module(format_path);
            }
        }

        mm.build_graph(entry);
        if (ctx.errors.has_errors()) return 1;

        // Semantic analysis
        for (auto* mod : mm.ordered_modules()) {
            quant::sm::SemanticAnalyzer sem(
                ctx,
                mod->namespace_path
            );

            sem.analyze(mod->ast, mod);
            if (ctx.errors.has_errors()) break;
            mod->analyzed = true;
        }
        if (ctx.errors.has_errors()) return 1;

        // Windows only, because Linux has compile only flag(-c) and checks @entry before ld cmd;
        if (opts.target_os == quant::codegen::mc::TargetOS::Windows) {
            // Linker validation
            linker.validate();
            if (ctx.errors.has_errors()) return 1;
        }

        // IRGen
        quant::codegen::IRGenerator irgen(ctx);
        irgen.gen_program(mm.ordered_modules());
        if (ctx.errors.has_errors()) return 1;

        // IR optimization (backend-agnostic, runs for every target).
        quant::codegen::optimize(
            irgen.program,
            static_cast<quant::codegen::OptLevel>(opts.opt_level),
            opts.static_lib
        );

        if (opts.emit_ir) {
             irgen.program.dump();
        }
        if(opts.no_compile){
            return 0;
        }
        // Build
        std::filesystem::path exe_path = "out";

        if(opts.has_output) exe_path = opts.output_file;

        // Create the output directory if it does not exist yet; otherwise the
        // ofstream below would silently fail and produce no executable.
        if (exe_path.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(exe_path.parent_path(), ec);
        }

        auto write_output = [](const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
            std::ofstream file(path, std::ios::binary);
            file.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
            if (!file) {
                utils::logger::error("failed to write output file: " + path.string());
                return false;
            }
            return true;
        };

        if (opts.target_os == quant::codegen::mc::TargetOS::Windows) {

            if (!exe_path.has_extension()) {
                exe_path += ".exe";
            }

            // Native backend: IR -> instruction selection -> machine code -> PE executable
            {
                quant::codegen::NativeBackend nativeBackend;
                auto pe_bytes = nativeBackend.generate(irgen.program, ctx, opts.target_arch, opts.target_os);
                if (!write_output(exe_path, pe_bytes)) return 1;
            }
        } else if (opts.static_lib) {
            if (!exe_path.has_extension()) {
                exe_path += ".a";
            }
            std::filesystem::path obj_path = exe_path.parent_path() / (exe_path.stem().string() + ".o");

            {
                quant::codegen::NativeBackend nativeBackend;
                auto elf_bytes = nativeBackend.generate(irgen.program, ctx, opts.target_arch, opts.target_os);
                if (!write_output(obj_path, elf_bytes)) return 1;
            }

            std::string ar_bin = opts.ar_name.empty() ? "ar" : opts.ar_name;
            // Rebuild the archive from scratch: ar rcs only ADDS/replaces a
            // member, so a stale archive would accumulate duplicate members.
            std::filesystem::remove(exe_path);
            std::string archive_cmd = ar_bin + " rcs " + exe_path.string() + " " + obj_path.string();
            if (std::system(archive_cmd.c_str()) != 0) {
                utils::logger::error("ar failed: " + archive_cmd + "\n");
                return 1;
            }
            std::filesystem::remove(obj_path);
        } else {
            std::filesystem::path obj_path = exe_path;
            if (opts.compile_only) {
                if (!obj_path.has_extension()) {
                    obj_path += ".o";
                }
            } else {
                obj_path = obj_path.parent_path() / ("." + obj_path.stem().string() + ".o");
            }

            {
                quant::codegen::NativeBackend nativeBackend;
                auto elf_bytes = nativeBackend.generate(irgen.program, ctx, opts.target_arch, opts.target_os);
                if (!write_output(obj_path, elf_bytes)) return 1;
            }

            if(!opts.compile_only){
                linker.validate();
                if (ctx.errors.has_errors()) return 1;

                std::string ld_name;
                if (!opts.linker_name.empty()) {
                    ld_name = opts.linker_name;
                    if (ld_name == "lld") ld_name = "ld.lld";
                } else {
                    ld_name = (opts.target_arch == quant::codegen::mc::TargetArch::AARCH64)
                        ? "ld.lld" : "ld";
                }
                std::string link_cmd = ld_name + " --gc-sections -o " + exe_path.string() + " " + obj_path.string();
                if (ctx.use_static_std) {
                    link_cmd += " " + ctx.static_std_path.string();
                }
                if (opts.target_os == quant::codegen::mc::TargetOS::ZeroPoint) {
                    link_cmd += " -pie --image-base=0x40000000";
                }
                if (std::system(link_cmd.c_str()) != 0) {
                    utils::logger::error("link failed\n");
                    return 1;
                }
                std::filesystem::remove(obj_path);
            }
        }

        auto end = std::chrono::high_resolution_clock::now();

        if (opts.time) {
            std::chrono::duration<double, std::milli> duration = end - start;
            std::cout << "\nCompilation took: " << duration.count() << " ms\n";
        }

        return 0;
    }
    catch (const std::exception& e) {
        utils::logger::error(std::string(e.what()));
        return 1;
    }
}
