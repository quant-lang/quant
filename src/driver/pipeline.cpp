#include "quant/driver/pipeline.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

#if defined(_WIN32)
#include <process.h>
#define QUANT_GETPID _getpid
#else
#include <unistd.h>
#define QUANT_GETPID ::getpid
#endif

#include "quant/attributes/attr_lowering.h"
#include "quant/frontend/lexer.h"
#include "quant/frontend/parser.h"
#include "quant/ir/opt.h"
#include "quant/linker/linker.h"
#include "quant/semantic/semantic.h"
#include "utils/file_manager.h"
#include "utils/logger.h"

#ifndef QUANT_USE_LIBQU
#define QUANT_USE_LIBQU 0
#endif

#if QUANT_USE_LIBQU
#include "quant_libqu_embedded.h"
#endif

namespace quant::driver {
namespace {

#if QUANT_USE_LIBQU
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

bool unpack_libqu(const quant::embedded_libqu::Archive& archive, std::filesystem::path& out_path, std::string& error) {
    out_path = std::filesystem::temp_directory_path() / libqu_cache_name(archive);

    std::error_code ec;
    const std::uintmax_t expected = archive.size;
    if (std::filesystem::exists(out_path, ec) &&
        std::filesystem::file_size(out_path, ec) == expected) {
        return true;
    }

    std::filesystem::path partial = out_path;
    partial += "." + std::to_string(QUANT_GETPID()) + ".tmp";

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
#endif

} // namespace

Pipeline::Pipeline(PipelineConfig cfg) : cfg_(std::move(cfg)) {
    init_context();
    mm_ = std::make_unique<modules::ModuleManager>(ctx_);
}

void Pipeline::init_context() {
    ctx_.errors.set_quiet(cfg_.quiet_errors);
    ctx_.target = cfg_.target ? cfg_.target : [] {
        const auto* info = codegen::mc::resolve_target(codegen::mc::host_default_target_name());
        return info ? info->target : nullptr;
    }();
    ctx_.emit_start = !cfg_.static_lib;

    ctx_.root_path = utils::io::get_executable_directory();
    auto parent = ctx_.root_path.parent_path();
    if (std::filesystem::exists(parent / "std" / "io" / "io.qu")) {
        ctx_.root_path = parent;
    }

    unpack_embedded_libqu();
}

bool Pipeline::unpack_embedded_libqu() {
    if (!ctx_.target)
        return false;
    const auto static_stdlib = ctx_.target->static_stdlib_name();
    if (static_stdlib.empty() || cfg_.static_lib)
        return false;

#if QUANT_USE_LIBQU
    for (const auto& archive : quant::embedded_libqu::archives()) {
        if (archive.target != ctx_.target->name()) continue;

        std::string error;
        if (!unpack_libqu(archive, ctx_.static_std_path, error)) {
            ctx_.errors.add("failed to unpack embedded stdlib: " + error);
            return false;
        }
        ctx_.use_static_std = true;
        return true;
    }
#endif

    auto lib_path = ctx_.root_path / "lib" / static_stdlib;
    if (!std::filesystem::exists(lib_path)) {
        lib_path = ctx_.root_path.parent_path() / "lib" / static_stdlib;
    }
    if (std::filesystem::exists(lib_path)) {
        ctx_.use_static_std = true;
        ctx_.static_std_path = lib_path;
    }
    return true;
}

bool Pipeline::load() {
    if (loaded_)
        return !ctx_.errors.has_errors();

    modules::Module* entry = nullptr;
    if (!cfg_.input_source.empty()) {
        const std::string display = cfg_.input_name.empty() ? "<stdin>" : cfg_.input_name;
        std::filesystem::path disk = cfg_.input_path.empty() ? std::filesystem::path{} : std::filesystem::path(cfg_.input_path);
        entry = mm_->load_source(display, cfg_.input_source, disk);
    } else {
        if (cfg_.input_file.empty()) {
            ctx_.errors.add("No input file provided");
            loaded_ = true;
            return false;
        }
        std::string path = cfg_.input_file;
        if (!std::filesystem::path(path).has_extension())
            path += ".qu";
        entry = mm_->load_entry(path);
    }

    if (!entry) {
        if (!ctx_.errors.has_errors())
            ctx_.errors.add("Failed to load entry module");
        loaded_ = true;
        return false;
    }

    if (cfg_.load_format_runtime && ctx_.target && ctx_.target->load_format_runtime() &&
        mm_->load_embedded("std::format") == nullptr) {
        auto format_path = ctx_.root_path / "std" / "format" / "format.qu";
        if (std::filesystem::exists(format_path)) {
            mm_->load_module(format_path);
        }
    }

    mm_->build_graph(entry);
    loaded_ = true;
    return !ctx_.errors.has_errors();
}

bool Pipeline::analyze() {
    if (!loaded_ && !load())
        return false;
    if (analyzed_)
        return !ctx_.errors.has_errors();

    if (ctx_.errors.has_errors() && ctx_.errors.parse_errors() != ctx_.errors.count()) {
        analyzed_ = true;
        return false;
    }

    std::unordered_set<modules::Module*> broken;
    for (auto* mod : mm_->ordered_modules()) {
        bool dep_broken = false;
        for (auto* dep : mod->dependencies) {
            if (broken.count(dep)) {
                dep_broken = true;
                break;
            }
        }
        if (mod->had_parse_errors || dep_broken) {
            broken.insert(mod);
            continue;
        }

        quant::attrs::AttrEvaluator attr_eval(ctx_, mod->namespace_path);
        attr_eval.evaluate(mod->ast);

        quant::sm::SemanticAnalyzer sem(ctx_, mod->namespace_path);
        size_t errors_before = ctx_.errors.count();
        sem.analyze(mod->ast, mod);
        mod->analyzed = true;
        if (ctx_.errors.count() != errors_before) {
            broken.insert(mod);
        }
    }

    if (ctx_.errors.has_errors()) {
        analyzed_ = true;
        return false;
    }

    quant::attrs::lower_dynamics(ctx_, mm_->ordered_modules());

    if (cfg_.validate_linker && ctx_.target && ctx_.target->link_spec().needs_pre_link_validation) {
        quant::linker::Linker linker(*mm_, ctx_);
        linker.validate();
    }

    analyzed_ = true;
    return !ctx_.errors.has_errors();
}

bool Pipeline::emit_ir() {
    if (!analyzed_ && !analyze())
        return false;
    irgen_ = std::make_unique<codegen::IRGenerator>(ctx_);
    irgen_->gen_program(mm_->ordered_modules());
    if (ctx_.errors.has_errors())
        return false;

    codegen::optimize(irgen_->program, static_cast<codegen::OptLevel>(cfg_.opt_level), cfg_.static_lib);
    return !ctx_.errors.has_errors();
}

} // namespace quant::driver
