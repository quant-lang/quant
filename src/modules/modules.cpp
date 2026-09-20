#include "quant/modules/module.h"
#include "quant/support/compiler_context.h"
#include "quant/frontend/lexer.h"
#include "quant/frontend/parser.h"
#include "quant/support/symbol_path.h"
#include "quant_std_embedded.h"
#include "utils/file_manager.h"
#include "utils/logger.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <unordered_set>
#include <vector>

namespace quant::modules {

namespace fs = std::filesystem;

namespace {

std::vector<std::string> module_namespace_from_path(const fs::path& path, const fs::path& root) {
    fs::path canon = fs::weakly_canonical(path);
    fs::path rel = fs::relative(canon, root);
    rel.replace_extension();

    std::vector<std::string> out;
    for (const auto& part : rel) {
        out.push_back(part.string());
    }
    return out;
}

std::vector<std::string> collect_imports(const std::vector<ast::Stmt*>& ast) {
    std::vector<std::string> ret;
    for (auto* stmt : ast) {
        if (!stmt) continue;
        if (auto* load = std::get_if<ast::LoadStmt>(&stmt->kind)) {
            ret.push_back(load->module);
        }
    }
    return ret;
}

// Convert module name like "std::io" to a relative path "std/io.qu"
fs::path module_name_to_path(const std::string& name) {
    fs::path p;
    size_t start = 0;
    while (true) {
        size_t sep = name.find("::", start);
        if (sep == std::string::npos) {
            p /= name.substr(start);
            break;
        }
        p /= name.substr(start, sep - start);
        start = sep + 2;
    }
    p += ".qu";
    return p;
}

// Same as module_name_to_path but always uses forward slashes (embedded table keys).
std::string module_name_to_rel_path(const std::string& name) {
    std::string out;
    size_t start = 0;
    while (true) {
        size_t sep = name.find("::", start);
        if (sep == std::string::npos) {
            out += name.substr(start);
            break;
        }
        out += name.substr(start, sep - start);
        out += '/';
        start = sep + 2;
    }
    out += ".qu";
    return out;
}

} // namespace

ModuleManager::ModuleManager(CompilerContext& ctx_)
    : ctx(ctx_) {}

Module* ModuleManager::load_entry(const fs::path& path) {
    fs::path abs = fs::absolute(path);
    if (!fs::exists(abs)) {
        ctx.errors.add("entry file not found: " + abs.string());
        return nullptr;
    }
    entry_dir = fs::weakly_canonical(abs).parent_path();
    return load_module(abs);
}
Module* ModuleManager::register_ast(const std::string& module_name,
                                    const fs::path& disk_path,
                                    std::vector<ast::Stmt*> ast) {
    // Check if already loaded
    if (modules.count(module_name)) {
        return modules[module_name];
    }

    auto imports = collect_imports(ast);

    // Split "std::io" -> ["std", "io"]
    std::vector<std::string> namespace_path;
    {
        size_t start = 0;
        while (true) {
            size_t sep = module_name.find("::", start);
            if (sep == std::string::npos) {
                namespace_path.push_back(module_name.substr(start));
                break;
            }
            namespace_path.push_back(module_name.substr(start, sep - start));
            start = sep + 2;
        }
    }

    auto* mod = memory::make_default<Module>(ctx.module_arena);
    mod->name = module_name;
    mod->namespace_path = namespace_path;
    mod->path = disk_path;
    if (!disk_path.empty()) mod->file_paths.push_back(disk_path);
    mod->ast = std::move(ast);
    mod->imports = std::move(imports);
    mod->ns = ctx.symbols.create_namespace_path(namespace_path);

    modules.emplace(module_name, mod);

    // Register by file key for dedup
    std::string file_key = disk_path.empty()
        ? "<ast>:" + module_name
        : fs::weakly_canonical(disk_path).string();
    loaded_files.emplace(file_key, mod);

    return mod;
}

Module* ModuleManager::load_module(const fs::path& path) {
    fs::path canon = fs::weakly_canonical(path);
    std::string file_key = canon.string();

    // Already loaded this file → return its module
    if (auto it = loaded_files.find(file_key); it != loaded_files.end()) {
        return it->second;
    }

    std::string source = utils::io::read_file(canon);
    return build_module(file_key, canon.string(), source, canon);
}

Module* ModuleManager::load_embedded(const std::string& imp) {
    return load_embedded_module(imp);
}

Module* ModuleManager::load_embedded_file(const std::string& rel_path, const std::string& source) {
    const std::string file_key = "<embedded>:" + rel_path;
    if (auto it = loaded_files.find(file_key); it != loaded_files.end()) {
        return it->second;
    }
    return build_module(file_key, rel_path, source, {});
}

Module* ModuleManager::load_embedded_module(const std::string& imp) {
    const auto& table = embedded_std::std_modules();
    if (table.empty()) return nullptr;

    const std::string rest = imp.rfind("std::", 0) == 0 ? imp.substr(5) : imp;

    // Candidate embedded paths, most specific first: platform overrides,
    // then the shared std/ tree. Each is either a primary file or a directory.
    std::vector<std::pair<std::string, bool>> candidates;
    // Windows target: WinAPI-based stdlib override (e.g. std::io -> std/win/io).
    if (ctx.target_os == codegen::mc::TargetOS::Windows) {
        candidates.emplace_back("std/win/" + module_name_to_rel_path(rest), false);
        candidates.emplace_back("std/win/" + rest, true);
    }
    // ZeroPoint target: syscall-based stdlib override (e.g. std::io -> std/zp/io).
    if (ctx.target_os == codegen::mc::TargetOS::ZeroPoint) {
        candidates.emplace_back("std/zp/" + module_name_to_rel_path(rest), false);
        candidates.emplace_back("std/zp/" + rest, true);
    }
    candidates.emplace_back("std/" + module_name_to_rel_path(rest), false);
    candidates.emplace_back("std/" + rest, true);

    for (const auto& [prefix, is_dir] : candidates) {
        Module* found = nullptr;
        for (const auto& [key, src] : table) {
            const bool matches = is_dir
                ? key.rfind(prefix + "/", 0) == 0
                : key == prefix;
            if (!matches) continue;

            // Primary file: use it unconditionally (module name checked on load).
            if (!is_dir) return load_embedded_file(key, src);

            // Directory: load all files and keep the one declaring this module.
            auto* m = load_embedded_file(key, src);
            if (m->name == imp) found = m;
        }
        if (found) return found;
    }
    return nullptr;
}

Module* ModuleManager::build_module(const std::string& file_key,
                                    const std::string& display_path,
                                    const std::string& source,
                                    const fs::path& disk_path) {
    // Read and split into lines
    std::vector<std::string> lines;
    {
        size_t pos = 0;
        while (pos < source.size()) {
            size_t end = source.find('\n', pos);
            if (end == std::string::npos) {
                lines.push_back(source.substr(pos));
                break;
            }
            lines.push_back(source.substr(pos, end - pos));
            pos = end + 1;
        }
    }
    ctx.srcloc.file = display_path;
    ctx.srcloc.line = 1;
    ctx.srcloc.column = 1;

    ctx.errors.add_source(file_key, SourceFile{lines});

    lx::Lexer lex(std::string(source), ctx);
    ps::Parser parser(lex, ctx);
    auto ast = parser.parse();
    auto imports = collect_imports(ast);

    // Extract module name from declaration (or fallback to path)
    std::string module_name;
    std::vector<std::string> namespace_path;
    std::vector<ast::Attribute> mod_attrs;

    auto* mod_decl = [&]() -> ast::ModuleDecl* {
        for (auto* stmt : ast) {
            if (!stmt) continue;
            if (auto* d = std::get_if<ast::ModuleDecl>(&stmt->kind)) {
                return d;
            }
        }
        return nullptr;
    }();

    if (mod_decl) {
        module_name = mod_decl->name;
        mod_attrs = std::move(mod_decl->attributes);

        // Split "std::io" -> ["std", "io"]
        size_t start = 0;
        while (true) {
            size_t sep = module_name.find("::", start);
            if (sep == std::string::npos) {
                namespace_path.push_back(module_name.substr(start));
                break;
            }
            namespace_path.push_back(module_name.substr(start, sep - start));
            start = sep + 2;
        }
    } else if (!disk_path.empty()) {
        namespace_path = module_namespace_from_path(disk_path, ctx.root_path);
        module_name = support::join_namespace(namespace_path);
    } else {
        ctx.errors.add("embedded module has no module declaration: " + display_path);
        return nullptr;
    }

    // Find or create module by name
    Module* mod = nullptr;
    auto mod_it = modules.find(module_name);

    if (mod_it != modules.end()) {
        mod = mod_it->second;
        // Merge AST
        mod->ast.insert(mod->ast.end(), ast.begin(), ast.end());
        if (!disk_path.empty()) mod->file_paths.push_back(disk_path);
        // Merge imports (dedup)
        for (const auto& imp : imports) {
            if (std::find(mod->imports.begin(), mod->imports.end(), imp) == mod->imports.end()) {
                mod->imports.push_back(imp);
            }
        }
    } else {
        mod = memory::make_default<Module>(ctx.module_arena);
        mod->name = module_name;
        mod->namespace_path = namespace_path;
        mod->path = disk_path;
        mod->file_paths = disk_path.empty() ? std::vector<fs::path>{} : std::vector<fs::path>{ disk_path };
        mod->ast = std::move(ast);
        mod->imports = std::move(imports);
        mod->attributes = std::move(mod_attrs);
        mod->ns = ctx.symbols.create_namespace_path(namespace_path);
        modules.emplace(module_name, mod);
    }

    mod->source_files[file_key] = SourceFile{lines};

    loaded_files.emplace(file_key, mod);
    return mod;
}

void ModuleManager::topo_sort() {
    ordered.clear();

    std::unordered_set<Module*> visited;
    std::unordered_set<Module*> stack;

    std::function<void(Module*)> dfs = [&](Module* mod) {
        if (!mod) return;

        if (visited.find(mod) != visited.end()) return;

        if (stack.find(mod) != stack.end()) {
            ctx.errors.add("cyclic dependency detected at module: " + mod->name);
            return;
        }

        stack.insert(mod);

        for (Module* dep : mod->dependencies) {
            dfs(dep);
        }

        stack.erase(mod);
        visited.insert(mod);
        ordered.push_back(mod);
    };

    // Collect roots (sorted for determinism)
    std::vector<Module*> roots;
    roots.reserve(modules.size());
    for (auto& [_, mod] : modules) {
        if (mod) roots.push_back(mod);
    }
    std::sort(roots.begin(), roots.end(),
        [](Module* a, Module* b) { return a->name < b->name; });

    for (Module* mod : roots) {
        dfs(mod);
    }

}

void ModuleManager::build_graph(Module* entry) {
    if (!entry) {
        ctx.errors.add("entry module is null");
        return;
    }

    std::unordered_set<Module*> visited;

    std::function<void(Module*)> dfs = [&](Module* mod) {
        if (!mod) return;
        if (visited.find(mod) != visited.end()) return;
        visited.insert(mod);

        for (const auto& imp : mod->imports) {
            Module* dep = nullptr;

            // 1. The standard library is embedded into the compiler binary.
            if (imp.rfind("std::", 0) == 0) {
                dep = load_embedded_module(imp);
            }

            // 2. Filesystem fallback (user modules, dev std overrides).
            if (!dep) {
                // Windows native backend: std modules are implemented with @import
                // and live in <root>/std/win/ (e.g. "std::io" -> std/win/io/io.qu).
                if (ctx.target_os == codegen::mc::TargetOS::Windows &&
                    imp.rfind("std::", 0) == 0) {
                    fs::path rest = module_name_to_path(imp.substr(5)); // "io.qu"
                    fs::path win_root = ctx.root_path / "std" / "win";
                    fs::path primary = win_root / rest;
                    fs::path dir = win_root / rest.parent_path() / rest.stem();

                    if (fs::exists(primary)) {
                        dep = load_module(primary);
                    } else if (fs::exists(dir) && fs::is_directory(dir)) {
                        for (const auto& dirent : fs::directory_iterator(dir)) {
                            if (dirent.path().extension() != ".qu") continue;
                            auto* m = load_module(dirent.path());
                            if (m->name == imp) dep = m;
                        }
                    }
                }
                // ZeroPoint native backend: syscall-based stdlib override lives
                // in <root>/std/zp/ (e.g. "std::io" -> std/zp/io/io.qu).
                if (dep == nullptr && ctx.target_os == codegen::mc::TargetOS::ZeroPoint &&
                    imp.rfind("std::", 0) == 0) {
                    fs::path rest = module_name_to_path(imp.substr(5)); // "io.qu"
                    fs::path zp_root = ctx.root_path / "std" / "zp";
                    fs::path primary = zp_root / rest;
                    fs::path dir = zp_root / rest.parent_path() / rest.stem();

                    if (fs::exists(primary)) {
                        dep = load_module(primary);
                    } else if (fs::exists(dir) && fs::is_directory(dir)) {
                        for (const auto& dirent : fs::directory_iterator(dir)) {
                            if (dirent.path().extension() != ".qu") continue;
                            auto* m = load_module(dirent.path());
                            if (m->name == imp) dep = m;
                        }
                    }
                }
                // Pure-Quant std modules without a target override (e.g.
                // std::string, std::vector) fall back to the shared std/ tree.
                if (dep == nullptr) {
                    // Try loading as module name (e.g. "std::io")
                    fs::path mod_rel = module_name_to_path(imp);     // e.g. "std/io.qu"
                    fs::path mod_dir_rel = mod_rel.parent_path() / mod_rel.stem(); // e.g. "std/io"

                    std::vector<fs::path> bases;
                    if (!mod->path.empty()) bases.push_back(mod->path.parent_path());
                    bases.push_back(entry_dir);
                    bases.push_back(ctx.root_path);

                    for (const auto& base : bases) {
                        fs::path primary = base / mod_rel;      // "base/std/io.qu"
                        fs::path dir = base / mod_dir_rel;      // "base/std/io/"

                        // 1. Primary file
                        if (fs::exists(primary)) {
                            dep = load_module(primary);
                        }

                        // 2. Module directory - scan for additional .qu files
                        if (fs::exists(dir) && fs::is_directory(dir)) {
                            for (const auto& dirent : fs::directory_iterator(dir)) {
                                if (dirent.path().extension() != ".qu") continue;
                                auto* m = load_module(dirent.path());
                                if (m->name == imp) dep = m;
                            }
                        }

                        if (dep) break;
                    }
                }
            }

            if (!dep) {
                ctx.errors.add("Unknown imported module: " + imp);
                continue;
            }

            mod->dependencies.push_back(dep);
            dfs(dep);
        }
    };

    // Resolve the dependency graph for every loaded module. The entry module
    // is normally the only root, but modules loaded directly (e.g. the
    // embedded std::format runtime) are not reachable from it and still need
    // their own imports resolved. Snapshot the map first since dfs() loads new
    // modules while we iterate.
    std::vector<Module*> roots;
    roots.reserve(modules.size());
    for (auto& [_, mod] : modules) {
        if (mod) roots.push_back(mod);
    }
    std::sort(roots.begin(), roots.end(),
        [](Module* a, Module* b) { return a->name < b->name; });

    for (Module* mod : roots) {
        dfs(mod);
    }

    topo_sort();
}

const std::vector<Module*>& ModuleManager::ordered_modules() const {
    return ordered;
}

} // namespace quant::modules
