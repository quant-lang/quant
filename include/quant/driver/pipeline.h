#pragma once

#include <memory>
#include <string>
#include <vector>

#include "quant/support/compiler_context.h"
#include "quant/modules/module.h"
#include "quant/ir/ir_gen.h"

namespace quant::driver {

struct PipelineConfig {
    std::string input_file;
    std::string input_source;
    std::string input_name = "<stdin>";
    std::string input_path; // optional disk path for module resolution
    const codegen::Target* target = nullptr;
    bool static_lib = false;
    int opt_level = 2;
    bool load_format_runtime = true;
    bool validate_linker = true;
    bool quiet_errors = false;
};

// Shared compiler pipeline used by the qu CLI and tooling (qud/lsp)
class Pipeline {
    public:
        explicit Pipeline(PipelineConfig cfg);

        CompilerContext& ctx() { return ctx_; }
        const CompilerContext& ctx() const { return ctx_; }
        modules::ModuleManager& modules() { return *mm_; }
        codegen::IRGenerator* irgen() { return irgen_.get(); }

        // Load entry + imports and build the module graph.
        bool load();

        // Run middle-end passes: attribute lowering, semantic analysis, dynamic
        // lowering, and linker validation. Returns false if diagnostics were emitted.
        bool analyze();

        // Generate and optimize IR. Requires a successful analyze()
        bool emit_ir();

    private:
        void init_context();
        bool unpack_embedded_libqu();

        PipelineConfig cfg_;
        CompilerContext ctx_;
        std::unique_ptr<modules::ModuleManager> mm_;
        std::unique_ptr<codegen::IRGenerator> irgen_;
        bool loaded_ = false;
        bool analyzed_ = false;
};

} // namespace quant::driver
