#include "quant/backend/symbol_table.h"
#include <cctype>

namespace quant::codegen {
    std::string asm_mangle(std::string name) {
        for (char& ch : name) {
            const unsigned char c = static_cast<unsigned char>(ch);
            if (!(std::isalnum(c) || ch == '_')) {
                ch = '_';
            }
        }
        return name;
    }

    std::string function_name(const IRFunction& fn) {
        if (!fn.export_name.empty()) {
            return fn.export_name;
        }
        return "qk_" + asm_mangle(fn.name);
    }

    std::string abi_name(const IRFunction& fn) {
        if (fn.is_extern) {
            return "qk_" + asm_mangle(fn.name);
        }
        return function_name(fn);
    }

    std::string string_label(uint32_t id) {
        return "str_" + std::to_string(id);
    }

    std::string global_label(uint32_t id) {
        return "gbl_" + std::to_string(id);
    }

    std::size_t align16(std::size_t n) {
        return (n + 15u) & ~std::size_t(15u);
    }

    const IRFunction* find_entry(const IRProgram& program) {
        const IRFunction* fallback = nullptr;
        for (const auto& fn : program.functions) {
            if (fn.is_entry) {
                return &fn;
            }
            if (fn.name.ends_with("main")) {
                fallback = &fn;
            }
        }
        return fallback;
    }

    namespace mc {
        uint32_t SymbolTable::ensure_symbol(const std::string& name, mc::SymBind bind, mc::SymType type,
                                    bool undefined, uint32_t section, uint64_t value, uint64_t size,
                                    const std::string& import_dll, const std::string& import_name) {
            auto it = index_.find(name);
            if (it != index_.end()) {
                if (!undefined) {
                    auto& s = symbols_[it->second];
                    s.bind = bind;
                    s.type = type;
                    s.undefined = false;
                    s.section = section;
                    s.value = value;
                    s.size = size;
                }
                return it->second;
            }
            mc::Symbol s;
            s.name = name;
            s.bind = bind;
            s.type = type;
            s.undefined = undefined;
            s.section = section;
            s.value = value;
            s.size = size;

            if (!import_dll.empty()) {
                s.import_dll = import_dll;
                s.import_name = import_name.empty() ? name : import_name;
            }

            symbols_.push_back(s);
            index_[name] = static_cast<uint32_t>(symbols_.size() - 1);
            return static_cast<uint32_t>(symbols_.size() - 1);
        }

        uint32_t SymbolTable::add_runtime_symbol(const std::string& name) {
            return ensure_symbol(name, mc::SymBind::Global, mc::SymType::Func, true, 0, 0, 0);
        }

        uint32_t SymbolTable::add_import_symbol(const std::string& dll, const std::string& name) {
            return ensure_symbol(name, mc::SymBind::Global, mc::SymType::Func, true, 0, 0, 0, dll, name);
        }

        uint32_t SymbolTable::add_func_symbol(const IRFunction& fn, uint64_t offset, bool imports) {
            if (fn.is_extern) {
                if (imports && !fn.import_dll.empty()) {
                    return ensure_symbol(abi_name(fn), mc::SymBind::Global, mc::SymType::Func,
                                        true, 0, 0, 0, fn.import_dll,
                                        fn.import_name.empty() ? fn.name : fn.import_name);
                }
                return ensure_symbol(abi_name(fn), mc::SymBind::Global, mc::SymType::Func, true, 0, 0, 0);
            }
            return ensure_symbol(function_name(fn), mc::SymBind::Global, mc::SymType::Func,
                                false, 0, offset, 0);
        }

    } // namespace mc
} // namespace quant::codegen
