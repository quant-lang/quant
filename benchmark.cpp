// benchmark.cpp - IR optimizer benchmark for the Quant compiler.
//
// Runs the front end once per input file (lexer -> parser -> semantic -> IR)
// and then times quant::codegen::optimize() at every requested level on
// fresh copies of the IR. Reports optimizer wall time, IR size before/after,
// the number of functions kept, folded loops/branches and, optionally, a
// per-pass breakdown.
//
//   quant_bench [options] file.qu [more.qu ...]
//     --levels 0,1,2,3   optimization levels to measure (default: all)
//     --repeat N         timing runs per level, the minimum is reported (default 5)
//     --func NAME        report the size of functions whose name contains NAME
//                        (default: "main")
//     --target NAME      compilation target (default: host)
//     --passes           print the per-pass breakdown for every level
//     --md               emit Markdown tables instead of aligned text
//
// Runtime speed of the produced executables is measured by
// scripts/bench_opt.ps1 / scripts/bench_opt.sh, which drive the real
// compiler binary.

#ifndef NOMINMAX
#define NOMINMAX   // utils/file_manager.h pulls in <windows.h>; keep std::max usable
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "quant/backend/targets.h"
#include "quant/frontend/lexer.h"
#include "quant/frontend/parser.h"
#include "quant/ir/ir_gen.h"
#include "quant/ir/opt.h"
#include "quant/modules/module.h"
#include "quant/semantic/semantic.h"
#include "quant/support/compiler_context.h"
#include "utils/file_manager.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::vector<int> levels{0, 1, 2, 3};
    int repeat = 5;
    std::string func = "main";
    std::string target;
    bool passes = false;
    bool markdown = false;
    std::vector<std::string> inputs;
};

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string format_ms(double ms) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(ms < 10 ? 3 : 1) << ms;
    return out.str();
}

bool parse_args(int argc, char** argv, Options& opts) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "error: " << name << " requires a value\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--levels") {
            const char* v = value("--levels");
            if (!v) return false;
            opts.levels.clear();
            std::stringstream ss(v);
            std::string item;
            while (std::getline(ss, item, ',')) {
                if (item.empty()) continue;
                const int level = std::atoi(item.c_str());
                if (level < 0 || level > 3) {
                    std::cerr << "error: bad level '" << item << "' (0..3)\n";
                    return false;
                }
                opts.levels.push_back(level);
            }
        } else if (arg == "--repeat") {
            const char* v = value("--repeat");
            if (!v) return false;
            opts.repeat = std::max(1, std::atoi(v));
        } else if (arg == "--func") {
            const char* v = value("--func");
            if (!v) return false;
            opts.func = v;
        } else if (arg == "--target") {
            const char* v = value("--target");
            if (!v) return false;
            opts.target = v;
        } else if (arg == "--passes") {
            opts.passes = true;
        } else if (arg == "--md") {
            opts.markdown = true;
        } else if (arg == "--help" || arg == "-h") {
            return false;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option " << arg << "\n";
            return false;
        } else {
            opts.inputs.push_back(arg);
        }
    }
    if (opts.inputs.empty()) {
        std::cerr << "error: no input files\n";
        return false;
    }
    return true;
}

void usage() {
    std::cerr <<
        "usage: quant_bench [--levels 0,1,2,3] [--repeat N] [--func NAME]\n"
        "                   [--target NAME] [--passes] [--md] file.qu [...]\n";
}

// Front end up to IR generation; mirrors src/main.cpp.
bool build_ir(const Options& opts, const std::filesystem::path& input,
              quant::CompilerContext& ctx, quant::codegen::IRProgram& out, double& frontend_ms) {
    using namespace quant;

    const std::string target_name = opts.target.empty()
        ? codegen::mc::host_default_target_name() : opts.target;
    const auto* target = codegen::mc::resolve_target(target_name);
    if (!target || !target->enabled) {
        std::cerr << "error: target '" << target_name << "' is not available in this build\n";
        return false;
    }

    const auto start = Clock::now();

    ctx.target_os = target->os;
    ctx.emit_start = true;
    ctx.root_path = utils::io::get_executable_directory();
    {
        const auto parent = ctx.root_path.parent_path();
        if (std::filesystem::exists(parent / "std" / "io" / "io.qu")) ctx.root_path = parent;
    }

    modules::ModuleManager mm(ctx);
    auto* entry = mm.load_entry(input);
    if (!entry || ctx.errors.has_errors()) return false;

    if (target->os != codegen::mc::TargetOS::ZeroPoint && mm.load_embedded("std::format") == nullptr) {
        const auto format_path = ctx.root_path / "std" / "format" / "format.qu";
        if (std::filesystem::exists(format_path)) mm.load_module(format_path);
    }

    mm.build_graph(entry);
    if (ctx.errors.has_errors()) return false;

    for (auto* mod : mm.ordered_modules()) {
        sm::SemanticAnalyzer sem(ctx, mod->namespace_path);
        sem.analyze(mod->ast, mod);
        if (ctx.errors.has_errors()) return false;
        mod->analyzed = true;
    }

    codegen::IRGenerator irgen(ctx);
    irgen.gen_program(mm.ordered_modules());
    if (ctx.errors.has_errors()) return false;

    out = std::move(irgen.program);
    frontend_ms = ms_since(start);
    return true;
}

struct LevelResult {
    int level = 0;
    double opt_ms = 0;
    quant::codegen::OptStats stats;
    size_t func_insts = 0;
    size_t func_matches = 0;
};

size_t matching_function_size(const quant::codegen::IRProgram& program,
                              const std::string& needle, size_t& matches) {
    size_t total = 0;
    matches = 0;
    for (const auto& fn : program.functions) {
        if (fn.name.find(needle) == std::string::npos) continue;
        total += fn.body.size();
        ++matches;
    }
    return total;
}

LevelResult measure_level(const quant::codegen::IRProgram& program, int level, const Options& opts) {
    using quant::codegen::OptLevel;
    using quant::codegen::OptStats;

    LevelResult result;
    result.level = level;
    result.opt_ms = 1e300;

    for (int run = 0; run < opts.repeat; ++run) {
        quant::codegen::IRProgram copy = program;
        OptStats stats;
        const auto start = Clock::now();
        quant::codegen::optimize(copy, static_cast<OptLevel>(level), false, &stats);
        const double ms = ms_since(start);
        if (ms < result.opt_ms) {
            result.opt_ms = ms;
            result.stats = std::move(stats);
            result.func_insts = matching_function_size(copy, opts.func, result.func_matches);
        }
    }
    return result;
}

struct Table {
    std::vector<std::string> header;
    std::vector<std::vector<std::string>> rows;

    void print(bool markdown) const {
        std::vector<size_t> width(header.size());
        for (size_t c = 0; c < header.size(); ++c) width[c] = header[c].size();
        for (const auto& row : rows) {
            for (size_t c = 0; c < row.size() && c < width.size(); ++c) {
                width[c] = std::max(width[c], row[c].size());
            }
        }

        const auto line = [&](const std::vector<std::string>& cells) {
            std::string out = markdown ? "| " : "  ";
            for (size_t c = 0; c < header.size(); ++c) {
                const std::string cell = c < cells.size() ? cells[c] : "";
                out += cell;
                out.append(width[c] - cell.size(), ' ');
                out += markdown ? " | " : "  ";
            }
            std::cout << out << "\n";
        };

        line(header);
        if (markdown) {
            std::string sep = "|";
            for (size_t c = 0; c < header.size(); ++c) {
                sep += std::string(width[c] + 2, '-') + "|";
            }
            std::cout << sep << "\n";
        } else {
            std::string sep = "  ";
            for (size_t c = 0; c < header.size(); ++c) sep += std::string(width[c], '-') + "  ";
            std::cout << sep << "\n";
        }
        for (const auto& row : rows) line(row);
    }
};

void report(const std::filesystem::path& input, double frontend_ms,
            const std::vector<LevelResult>& results, const Options& opts) {
    std::cout << (opts.markdown ? "### " : "== ") << input.generic_string()
              << (opts.markdown ? "" : " ==") << "\n\n";
    std::cout << (opts.markdown ? "Front end (lexer, parser, semantic, IR gen): "
                                : "front end: ")
              << format_ms(frontend_ms) << " ms" << (opts.markdown ? "  \n" : "\n");
    std::cout << (opts.markdown ? "Optimizer time is the minimum of " : "optimizer time: min of ")
              << opts.repeat << (opts.markdown ? " runs.\n\n" : " runs\n\n");

    Table table;
    table.header = {"level", "optimize, ms", "IR insts", "insts in '" + opts.func + "'",
                    "functions", "loops folded", "branches folded"};
    for (const auto& r : results) {
        const auto& s = r.stats;
        const auto pct = [&](size_t after, size_t before) {
            if (before == 0) return std::string("");
            std::ostringstream out;
            out << " (" << std::fixed << std::setprecision(0)
                << 100.0 * static_cast<double>(after) / static_cast<double>(before) << "%)";
            return out.str();
        };
        table.rows.push_back({
            "-O" + std::to_string(r.level),
            format_ms(r.opt_ms),
            std::to_string(s.insts_after) + pct(s.insts_after, s.insts_before),
            std::to_string(r.func_insts) + (r.func_matches > 1
                ? " (" + std::to_string(r.func_matches) + " fns)" : ""),
            std::to_string(s.functions_after) + " / " + std::to_string(s.functions_before),
            std::to_string(s.loops_folded),
            std::to_string(s.branches_folded),
        });
    }
    table.print(opts.markdown);
    std::cout << "\n";

    if (!opts.passes) return;
    for (const auto& r : results) {
        if (r.stats.passes.empty()) continue;
        std::cout << (opts.markdown ? "#### passes at -O" : "passes at -O") << r.level << "\n\n";
        Table passes;
        passes.header = {"pass", "time, ms", "invocations", "changed"};
        for (const auto& p : r.stats.passes) {
            passes.rows.push_back({
                p.name,
                format_ms(static_cast<double>(p.nanoseconds) / 1e6),
                std::to_string(p.invocations),
                std::to_string(p.changes),
            });
        }
        passes.print(opts.markdown);
        std::cout << "\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    Options opts;
    if (!parse_args(argc, argv, opts)) {
        usage();
        return 2;
    }

    int failures = 0;
    for (const auto& input_name : opts.inputs) {
        std::filesystem::path input = input_name;
        if (!input.has_extension()) input += ".qu";

        quant::CompilerContext ctx;
        quant::codegen::IRProgram program;
        double frontend_ms = 0;
        try {
            if (!build_ir(opts, input, ctx, program, frontend_ms)) {
                std::cerr << "error: front end failed for " << input.generic_string() << "\n";
                ++failures;
                continue;
            }
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            ++failures;
            continue;
        }

        std::vector<LevelResult> results;
        for (int level : opts.levels) results.push_back(measure_level(program, level, opts));
        report(input, frontend_ms, results, opts);
    }
    return failures == 0 ? 0 : 1;
}
