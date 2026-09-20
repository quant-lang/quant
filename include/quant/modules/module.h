#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "quant/frontend/ast.h"
#include "quant/semantic/symbol_table.h"
#include "quant/support/alloc.h"
#include "utils/errors.h"

namespace quant {
struct CompilerContext;
}

namespace quant::modules {

namespace fs = std::filesystem;

struct Module {
    std::string name;                           // e.g. "std::io"
    fs::path path;                              // primary file
    std::vector<fs::path> file_paths;            // all files in this module

    std::vector<ast::Stmt*> ast;                // merged AST
    std::vector<ast::Attribute> attributes;
    std::vector<std::string> imports;
    std::vector<std::string> namespace_path;
    std::vector<Module*> dependencies;           // resolved dependencies
    symb_t::Namespace* ns = nullptr;

    std::unordered_map<std::string, SourceFile> source_files;

    bool analyzed = false;
};

class ModuleManager {
public:
    explicit ModuleManager(::quant::CompilerContext& ctx);

    Module* load_entry(const fs::path& path);
    Module* load_module(const fs::path& path);

    // Register a module with a pre-built AST (e.g. deserialized from QAST).
    // The module_name must already be set. populates imports from LoadStmts.
    Module* register_ast(const std::string& module_name,
                         const fs::path& disk_path,
                         std::vector<ast::Stmt*> ast);

    // Load a std:: module from the stdlib embedded into the binary.
    // Returns nullptr if the module is not found in the embedded table.
    Module* load_embedded(const std::string& imp);

    void build_graph(Module* entry);

    const std::vector<Module*>& ordered_modules() const;

private:
    std::filesystem::path entry_dir;
    Module* build_module(const std::string& file_key,
                         const std::string& display_path,
                         const std::string& source,
                         const fs::path& disk_path);
    Module* load_embedded_file(const std::string& rel_path, const std::string& source);
    Module* load_embedded_module(const std::string& imp);

    std::string extract_module_name(const std::vector<ast::Stmt*>& ast,
                                    std::vector<ast::Attribute>& out_attrs) const;
    void topo_sort();

    CompilerContext& ctx;
    // Key: module name (e.g. "std::io")
    std::unordered_map<std::string, Module*> modules;
    // Key: canonical file path — for dedup
    std::unordered_map<std::string, Module*> loaded_files;
    std::vector<Module*> ordered;
};

} // namespace quant::modules
