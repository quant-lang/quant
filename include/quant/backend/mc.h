#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace quant::codegen::mc {

enum class TargetArch : uint8_t {
    X86_64,
    AARCH64,
};

// Target operating system / ABI. Determines the syscall layer and
// executable flavor (see targets.h, AArch64ISel, elf/pe writers).
enum class TargetOS : uint8_t {
    Linux,
    Windows,
    ZeroPoint,  // AArch64-only: single-buffer syscalls, PIC, no exit yet.
};

// An (architecture, OS) pair identifying one compilation target.
// See targets.h for the registry of known/enabled targets.
struct Target {
    TargetArch arch = TargetArch::X86_64;
    TargetOS os = TargetOS::Linux;
};

enum class RelType : uint32_t {
    X86_64_64   = 1,   // S + A
    X86_64_PC32 = 2,   // S + A - P
    X86_64_PLT32 = 4,  // PLT(S) + A - P

    AARCH64_ABS64           = 257,  // S + A
    AARCH64_CALL26          = 283,  // (S + A - P) >> 2
    AARCH64_JUMP26          = 282,  // (S + A - P) >> 2
    AARCH64_ADR_PREL_PG_HI21 = 275, // (page(S+A) - page(P)) >> 12
    AARCH64_LDST64_ABS_LO12_NC = 286, // (S + A) & 0xFFF  (for LDR/STR)
    AARCH64_ADD_ABS_LO12_NC    = 277, // (S + A) & 0xFFF  (for ADD immediate)
};

struct Relocation {
    uint32_t offset = 0;      // byte offset in the .text section
    std::string symbol;
    RelType type = RelType::X86_64_PC32;
    int64_t addend = 0;
};

enum class SymBind : uint8_t {
    Local = 0,
    Global = 1,
};

enum class SymType : uint8_t {
    Notype = 0,
    Object = 1,
    Func = 2,
};

struct Symbol {
    std::string name;
    SymBind bind = SymBind::Local;
    SymType type = SymType::Notype;
    bool undefined = false;
    uint32_t section = 0;   // 0 = .text, 1 = .data, 2 = .rodata (internal ids)
    uint64_t value = 0;     // offset within the section
    uint64_t size = 0;
    std::string import_dll;   // non-empty: imported from a PE DLL (undefined symbol)
    std::string import_name;  // imported symbol name (defaults to `name`)
};

struct Object {
    std::vector<uint8_t> text;
    std::vector<uint8_t> data;
    std::vector<uint8_t> rodata;  // string literals: read-only, no writes are emitted
    std::vector<Relocation> relocs;
    std::vector<Symbol> symbols;
};

// Internal section ids used by Symbol::section
enum : uint32_t {
    SEC_TEXT = 0,
    SEC_DATA = 1,
    SEC_RODATA = 2,
};

} // namespace quant::codegen::mc
