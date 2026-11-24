#include <filesystem>
#include <iostream>
#include <chrono>
#include <fstream>
#include <cstdlib>

#include "quant/driver/pipeline.h"
#include "quant/linker/linker.h"

#include "utils/options.h"
#include "utils/logger.h"

#include "backend/native_backend.h"

int main(int argc, char **argv)
{
    try{
        using namespace std::chrono;

        auto opts = utils::options::parse_args(argc, argv);
        auto start = high_resolution_clock::now();

        quant::driver::PipelineConfig pcfg;
        pcfg.input_file = opts.input_file;
        pcfg.target = opts.target;
        pcfg.static_lib = opts.static_lib;
        pcfg.opt_level = opts.opt_level;
        pcfg.load_format_runtime = opts.target->load_format_runtime();
        pcfg.validate_linker = opts.target->link_spec().needs_pre_link_validation;
        pcfg.quiet_errors = false;

        quant::driver::Pipeline pipeline(pcfg);
        if (!pipeline.load()) return 1;
        if (!pipeline.analyze()) return 1;

        if (!pipeline.emit_ir()) return 1;
        auto* irgen = pipeline.irgen();
        auto& ctx = pipeline.ctx();
        const auto link_spec = opts.target->link_spec();

        if (opts.emit_ir) {
            irgen->program.dump();
        }
        if (opts.no_compile) {
            return 0;
        }
        // Build
        std::filesystem::path exe_path = "out";
        if (opts.has_output) exe_path = opts.output_file;

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

        if (!link_spec.needs_external_linker) {
            if (!exe_path.has_extension()) {
                exe_path += link_spec.executable_ext;
            }
            quant::codegen::NativeBackend nativeBackend;
            auto pe_bytes = nativeBackend.generate(irgen->program, ctx.emit_start, *opts.target);
            if (!write_output(exe_path, pe_bytes)) return 1;
        } else if (opts.static_lib) {
            if (!exe_path.has_extension()) {
                exe_path += ".a";
            }
            std::filesystem::path obj_path = exe_path.parent_path() / (exe_path.stem().string() + ".o");
            {
                quant::codegen::NativeBackend nativeBackend;
                auto elf_bytes = nativeBackend.generate(irgen->program, ctx.emit_start, *opts.target);
                if (!write_output(obj_path, elf_bytes)) return 1;
            }

            std::string ar_bin = opts.ar_name.empty() ? "ar" : opts.ar_name;
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
                if (!obj_path.has_extension()) obj_path += ".o";
            } else {
                obj_path = obj_path.parent_path() / ("." + obj_path.stem().string() + ".o");
            }

            {
                quant::codegen::NativeBackend nativeBackend;
                auto elf_bytes = nativeBackend.generate(irgen->program, ctx.emit_start, *opts.target);
                if (!write_output(obj_path, elf_bytes)) return 1;
            }

            if (!opts.compile_only) {
                quant::linker::Linker linker(pipeline.modules(), ctx);
                linker.validate();
                if (ctx.errors.has_errors()) return 1;

                std::string ld_name;
                if (!opts.linker_name.empty()) {
                    ld_name = opts.linker_name;
                    if (ld_name == "lld") ld_name = "ld.lld";
                } else {
                    ld_name = link_spec.default_linker;
                }
                std::string link_cmd = ld_name + " --gc-sections -o \"" + exe_path.string()
                    + "\" \"" + obj_path.string() + "\"";
                if (ctx.use_static_std) {
                    link_cmd += " \"" + ctx.static_std_path.string() + "\"";
                }
                if (!link_spec.extra_flags.empty()) {
                    link_cmd += " " + link_spec.extra_flags;
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
