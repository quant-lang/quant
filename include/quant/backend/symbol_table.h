#pragma once
#include <unordered_map>
#include "quant/backend/mc.h"
#include "quant/ir/ir.h"

namespace quant::codegen {
std::string asm_mangle(std::string name);
std::string function_name(const IRFunction& fn);
std::string abi_name(const IRFunction& fn);
std::string string_label(uint32_t id);
std::string global_label(uint32_t id);
std::size_t align16(std::size_t n);
const IRFunction* find_entry(const IRProgram& program);

namespace mc {
// Reference the object's vector so insertion order and symbol indices stay stable.
class SymbolTable {
public:
    explicit SymbolTable(std::vector<Symbol>& symbols) : symbols_(symbols) {}
    SymbolTable(const SymbolTable&) = delete;
    SymbolTable& operator=(const SymbolTable&) = delete;
    uint32_t ensure_symbol(const std::string& name, SymBind bind, SymType type,
                           bool undefined = false, uint32_t section = 0,
                           uint64_t value = 0, uint64_t size = 0,
                           const std::string& import_dll = {},
                           const std::string& import_name = {});
    uint32_t add_runtime_symbol(const std::string& name);
    uint32_t add_import_symbol(const std::string& dll, const std::string& name);
    uint32_t add_func_symbol(const IRFunction& fn, uint64_t offset, bool imports);
    bool has(const std::string& name) const { return index_.count(name) != 0; }
    void clear() { symbols_.clear(); index_.clear(); }
    std::vector<Symbol>& symbols() { return symbols_; }
private:
    std::vector<Symbol>& symbols_;
    std::unordered_map<std::string, uint32_t> index_;
};
} // namespace mc
} // namespace quant::codegen
