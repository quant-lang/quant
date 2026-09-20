#include <filesystem>
#include <iostream>
#include <chrono>
#include <fstream>
#include <cstdlib>
#include <cstdio>

#include "quant/frontend/lexer.h"
#include "quant/frontend/parser.h"
#include "quant/frontend/ast_deserialize.h"
#include "quant/semantic/semantic.h"
#include "quant/support/compiler_context.h"

#include "utils/options.h"
#include "utils/logger.h"

#include "quant/ir/ir_gen.h"
#include "quant/ir/opt.h"
#include "quant/backend/fasmcodegen.h"
#include "quant/backend/native_backend.h"

#include "quant/modules/module.h"
#include "quant/linker/linker.h"

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

        // Auto-detect pre-compiled static stdlib (.a) for Linux targets.
        // Searches for lib/qu-<arch>-<os>.a next to the compiler binary,
        // then in the parent directory (covers build layout where root_path
        // stays at build/bin/ but .a lives in build/lib/).
        if (opts.target_os == quant::codegen::mc::TargetOS::Linux && !opts.static_lib) {
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

        if (!opts.from_ast.empty()) {
            // Deserialize QAST binary and register as entry module
            quant::ast::AstDeserializer deser(ctx);
            auto ast = deser.deserialize_file(opts.from_ast);

            // Extract module name from ModuleDecl
            std::string module_name;
            for (auto* stmt : ast) {
                if (auto* md = std::get_if<quant::ast::ModuleDecl>(&stmt->kind)) {
                    module_name = md->name;
                    break;
                }
            }
            if (module_name.empty()) {
                module_name = std::filesystem::path(opts.from_ast).stem().string();
            }

            // Register the deserialized module
            entry = mm.register_ast(module_name, {}, std::move(ast));
        } else {
            if (opts.input_file.empty()) {
                utils::logger::error("No input file provided");
                return 1;
            }

            if (!std::filesystem::path(opts.input_file).has_extension()) {
                opts.input_file += ".qu";
            }

            // Try self-hosted frontend: look for ./out next to the source
            // or next to the compiler binary, then pipe source through it.
            auto src_path = std::filesystem::absolute(opts.input_file);
            std::filesystem::path fe_bin;
#ifdef QUANT_SELF_HOSTED_ENABLED
#ifdef QUANT_SELF_HOSTED_BIN
            // Compile-time path from CMake
            if (std::filesystem::exists(QUANT_SELF_HOSTED_BIN)) {
                fe_bin = QUANT_SELF_HOSTED_BIN;
            }
#endif
            // Runtime search: next to source, then in source tree
            if (fe_bin.empty()) {
                auto c = src_path.parent_path() / "out";
                if (std::filesystem::exists(c)) fe_bin = c;
            }
            if (fe_bin.empty()) {
                auto c = ctx.root_path / "src" / "self-hosted" / "out";
                if (std::filesystem::exists(c)) fe_bin = c;
            }
            if (fe_bin.empty()) {
                auto c = ctx.root_path.parent_path() / "src" / "self-hosted" / "out";
                if (std::filesystem::exists(c)) fe_bin = c;
            }
#endif

            if (!fe_bin.empty()) {
                // Pipe source through self-hosted frontend
                std::string cmd = fe_bin.string() + " < " + src_path.string();
                FILE* pipe = popen(cmd.c_str(), "r");
                if (!pipe) {
                    throw std::runtime_error("failed to spawn self-hosted frontend: " + fe_bin.string());
                }

                // Wrap in a streamsbuf so we can use std::istream
                struct PipeBuf : std::streambuf {
                    FILE* f;
                    PipeBuf(FILE* f) : f(f) {}
                    int underflow() override {
                        char buf[4096];
                        size_t n = std::fread(buf, 1, sizeof(buf), f);
                        if (n == 0) return traits_type::eof();
                        setg(buf, buf, buf + n);
                        return traits_type::to_int_type(*gptr());
                    }
                } pbuf(pipe);
                std::istream pipe_stream(&pbuf);

                quant::ast::AstDeserializer deser(ctx);
                deser.source_file = src_path.string();
                auto ast = deser.deserialize(pipe_stream);

                int rc = pclose(pipe);
                if (rc != 0) {
                    throw std::runtime_error("self-hosted frontend failed (exit " + std::to_string(rc) + ")");
                }

                // Extract module name from ModuleDecl
                std::string module_name;
                for (auto* stmt : ast) {
                    if (auto* md = std::get_if<quant::ast::ModuleDecl>(&stmt->kind)) {
                        module_name = md->name;
                        break;
                    }
                }
                if (module_name.empty()) {
                    module_name = src_path.stem().string();
                }

                entry = mm.register_ast(module_name, src_path, std::move(ast));
            } else {
                entry = mm.load_entry(opts.input_file);
            }
        }

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
        // Codegen
        if (opts.emit_asm) {
            quant::codegen::FasmCodeGenerator fasmCodegen;
            fasmCodegen.target_os = opts.target_os;
            std::string asm_code = fasmCodegen.generate(irgen.program);
            utils::logger::info("asm:");
            utils::logger::info(asm_code);
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

        auto write_output = [](const std::filesystem::path& path,
                               const std::vector<uint8_t>& bytes) {
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
