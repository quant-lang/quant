#include "quant/backend/elf_writer.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace quant::codegen::elf {

namespace {

enum : uint32_t {
    SHN_UNDEF = 0,
    SHT_PROGBITS = 1,
    SHT_SYMTAB = 2,
    SHT_STRTAB = 3,
    SHT_RELA = 4,
    SHF_WRITE = 0x1,
    SHF_ALLOC = 0x2,
    SHF_EXECINSTR = 0x4,
};

struct Buf {
    std::vector<uint8_t> data;

    void u8(uint8_t v) { data.push_back(v); }
    void u16(uint16_t v) {
        for (int i = 0; i < 2; ++i) u8(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) u8(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) u8(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
    void bytes(const void* p, std::size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        data.insert(data.end(), b, b + n);
    }
    void pad_to(std::size_t target) {
        while (data.size() < target) u8(0);
    }
};

std::size_t align8(std::size_t n) {
    return (n + 7u) & ~std::size_t(7u);
}
std::size_t align16(std::size_t n) {
    return (n + 15u) & ~std::size_t(15u);
}

struct StringTable {
    std::vector<uint8_t> bytes;
    std::unordered_map<std::string, uint32_t> offsets;

    StringTable() {
        bytes.push_back(0);
    }
    uint32_t add(const std::string& name) {
        auto it = offsets.find(name);
        if (it != offsets.end()) return it->second;
        const uint32_t off = static_cast<uint32_t>(bytes.size());
        offsets[name] = off;
        bytes.insert(bytes.end(), name.begin(), name.end());
        bytes.push_back(0);
        return off;
    }
};

void write_shdr(Buf& b, uint32_t name, uint32_t type, uint64_t flags, uint64_t offset,
                uint64_t size, uint32_t link, uint32_t info, uint64_t addralign,
                uint64_t entsize) {
    b.u32(name);
    b.u32(type);
    b.u64(flags);
    b.u64(0); // sh_addr
    b.u64(offset);
    b.u64(size);
    b.u32(link);
    b.u32(info);
    b.u64(addralign);
    b.u64(entsize);
}

} // namespace

std::vector<uint8_t> write(const mc::Object& obj, mc::TargetArch arch) {
    // Sort symbols: locals first, then globals.
    std::vector<const mc::Symbol*> syms;
    for (const auto& s : obj.symbols) {
        if (s.bind == mc::SymBind::Local) syms.push_back(&s);
    }

    const uint32_t first_global = static_cast<uint32_t>(syms.size()) + 1;

    for (const auto& s : obj.symbols) {
        if (s.bind == mc::SymBind::Global) syms.push_back(&s);
    }

    // Split .text into per-function sections.
    struct FuncSlice {
        std::string sec_name;
        uint32_t start;
        uint32_t size;
    };
    std::vector<FuncSlice> func_slices;

    {
        struct TextSym {
            uint64_t value;
            std::string name;
        };

        std::vector<TextSym> text_syms;
        for (const auto* s : syms) {
            if (!s->undefined && s->section == 0) {
                text_syms.push_back({s->value, s->name});
            }
        }

        std::sort(text_syms.begin(), text_syms.end(),
                  [](const TextSym& a, const TextSym& b) { return a.value < b.value; });

        const uint32_t text_size = static_cast<uint32_t>(obj.text.size());
        for (size_t i = 0; i < text_syms.size(); ++i) {
            uint32_t start = static_cast<uint32_t>(text_syms[i].value);
            uint32_t end = (i + 1 < text_syms.size())
                ? static_cast<uint32_t>(text_syms[i + 1].value)
                : text_size;

            if (end > start) {
                func_slices.push_back({
                    ".text." + text_syms[i].name,
                    start,
                    end - start
                });
            }
        }
    }

    std::vector<FuncSlice> sections;
    if (!func_slices.empty() && func_slices[0].start > 0) {
        sections.push_back({".text", 0, func_slices[0].start});
    }

    for (auto& fs : func_slices) {
        sections.push_back(std::move(fs));
    }

    if (sections.empty() && !obj.text.empty()) {
        sections.push_back({
            ".text",
            0,
            static_cast<uint32_t>(obj.text.size())
        });
    }

    // Assign relocations to text sections.
    std::vector<std::vector<mc::Relocation>> sec_relocs(sections.size());
    for (const auto& r : obj.relocs) {
        for (size_t i = 0; i < sections.size(); ++i) {
            if (r.offset >= sections[i].start &&
                r.offset < sections[i].start + sections[i].size) {
                mc::Relocation adj = r;
                adj.offset -= sections[i].start;
                sec_relocs[i].push_back(adj);
                break;
            }
        }
    }

    // Build string tables and section indices.
    StringTable strtab;
    for (const auto* s : syms) strtab.add(s->name);

    StringTable shstrtab;
    shstrtab.add(".text");
    shstrtab.add(".data");
    shstrtab.add(".rodata");
    shstrtab.add(".symtab");
    shstrtab.add(".strtab");
    shstrtab.add(".shstrtab");

    const uint32_t n_text = static_cast<uint32_t>(sections.size());

    uint32_t n_rela = 0;
    for (uint32_t i = 0; i < n_text; ++i) {
        if (!sec_relocs[i].empty()) ++n_rela;
    }

    const uint32_t IDX_DATA = n_text + n_rela + 1;
    const uint32_t IDX_RODATA = IDX_DATA + 1;
    const uint32_t IDX_SYMTAB = IDX_RODATA + 1;
    const uint32_t IDX_STRTAB = IDX_SYMTAB + 1;
    const uint32_t IDX_SHSTRTAB = IDX_STRTAB + 1;
    const uint32_t SECTION_COUNT = IDX_SHSTRTAB + 1;

    for (auto& sec : sections) {
        shstrtab.add(sec.sec_name);
    }

    for (uint32_t i = 0; i < n_text; ++i) {
        if (!sec_relocs[i].empty()) {
            shstrtab.add(".rela" + sections[i].sec_name);
        }
    }

    // Compute file layout.
    uint32_t rela_total = 0;
    for (uint32_t i = 0; i < n_text; ++i) {
        rela_total += static_cast<uint32_t>(sec_relocs[i].size()) * 24;
    }

    const uint32_t data_size = static_cast<uint32_t>(obj.data.size());
    const uint32_t rodata_size = static_cast<uint32_t>(obj.rodata.size());
    const uint32_t symtab_size = static_cast<uint32_t>(1 + syms.size()) * 24;
    const uint32_t strtab_size = static_cast<uint32_t>(strtab.bytes.size());
    const uint32_t shstrtab_size = static_cast<uint32_t>(shstrtab.bytes.size());

    uint32_t off = 64;
    std::vector<uint32_t> text_sec_off(n_text);

    for (uint32_t i = 0; i < n_text; ++i) {
        off = static_cast<uint32_t>(align16(off));
        text_sec_off[i] = off;
        off += sections[i].size;
    }

    std::vector<uint32_t> rela_sec_off(n_text, 0);
    std::vector<uint32_t> rela_sec_size(n_text, 0);

    for (uint32_t i = 0; i < n_text; ++i) {
        if (!sec_relocs[i].empty()) {
            off = static_cast<uint32_t>(align16(off));
            rela_sec_off[i] = off;
            rela_sec_size[i] = static_cast<uint32_t>(sec_relocs[i].size()) * 24;
            off += rela_sec_size[i];
        }
    }

    off = static_cast<uint32_t>(align8(off));
    const uint32_t data_off = off;
    off += data_size;

    off = static_cast<uint32_t>(align8(off));
    const uint32_t rodata_off = off;
    off += rodata_size;

    off = static_cast<uint32_t>(align8(off));
    const uint32_t symtab_off = off;
    off += symtab_size;

    const uint32_t strtab_off = off;
    off += strtab_size;

    const uint32_t shstrtab_off = off;
    off += shstrtab_size;

    const uint32_t shoff = static_cast<uint32_t>(align8(off));

    // Map symbols to section indices.
    auto find_text_sec = [&](uint64_t value) -> uint32_t {
        for (uint32_t i = 0; i < n_text; ++i) {
            if (value >= sections[i].start &&
                value < sections[i].start + sections[i].size) {
                return 1 + i;
            }
        }
        return 1;
    };

    auto elf_section_of = [&](const mc::Symbol& s) -> uint32_t {
        if (s.undefined) return SHN_UNDEF;
        if (s.section == mc::SEC_TEXT) return find_text_sec(s.value);
        if (s.section == mc::SEC_RODATA) return IDX_RODATA;
        return IDX_DATA;
    };

    auto adjusted_value = [&](const mc::Symbol& s) -> uint64_t {
        if (s.undefined) return 0;

        if (s.section == mc::SEC_TEXT) {
            for (uint32_t i = 0; i < n_text; ++i) {
                if (s.value >= sections[i].start &&
                    s.value < sections[i].start + sections[i].size) {
                    return s.value - sections[i].start;
                }
            }
        }

        return s.value;
    };

    // Write ELF.
    Buf b;

    // ELF header.
    b.u8(0x7F); b.u8('E'); b.u8('L'); b.u8('F');
    b.u8(2);
    b.u8(1);
    b.u8(1);
    b.u8(0);
    b.u8(0);
    for (int i = 0; i < 7; ++i) b.u8(0);

    b.u16(1);
    b.u16(arch == mc::TargetArch::AARCH64 ? 183 : 62);
    b.u32(1);
    b.u64(0);
    b.u64(0);
    b.u64(shoff);
    b.u32(0);
    b.u16(64);
    b.u16(0);
    b.u16(0);
    b.u16(64);
    b.u16(SECTION_COUNT);
    b.u16(IDX_SHSTRTAB);

    // Text sections.
    for (uint32_t i = 0; i < n_text; ++i) {
        b.pad_to(text_sec_off[i]);
        b.bytes(obj.text.data() + sections[i].start, sections[i].size);
    }

    // Relocation sections.
    for (uint32_t i = 0; i < n_text; ++i) {
        if (sec_relocs[i].empty()) continue;

        b.pad_to(rela_sec_off[i]);

        std::unordered_map<std::string, uint32_t> sym_index;
        for (uint32_t j = 0; j < syms.size(); ++j) {
            sym_index[syms[j]->name] = j + 1;
        }

        for (const auto& r : sec_relocs[i]) {
            b.u64(r.offset);
            b.u64((static_cast<uint64_t>(sym_index.at(r.symbol)) << 32) |
                  static_cast<uint32_t>(r.type));
            b.u64(static_cast<uint64_t>(r.addend));
        }
    }

    // Data.
    b.pad_to(data_off);
    b.bytes(obj.data.data(), obj.data.size());

    // Read-only data (string literals): no SHF_WRITE.
    b.pad_to(rodata_off);
    b.bytes(obj.rodata.data(), obj.rodata.size());

    // Symbol table.
    b.pad_to(symtab_off);
    {
        b.u32(0); b.u8(0); b.u8(0); b.u16(0); b.u64(0); b.u64(0);

        for (const auto* s : syms) {
            b.u32(strtab.offsets.at(s->name));
            b.u8(static_cast<uint8_t>((static_cast<uint8_t>(s->bind) << 4) |
                                      static_cast<uint8_t>(s->type)));
            b.u8(0);
            b.u16(static_cast<uint16_t>(elf_section_of(*s)));
            b.u64(adjusted_value(*s));
            b.u64(s->size);
        }
    }

    // String tables.
    b.pad_to(strtab_off);
    b.bytes(strtab.bytes.data(), strtab.bytes.size());

    b.pad_to(shstrtab_off);
    b.bytes(shstrtab.bytes.data(), shstrtab.bytes.size());

    // Section headers.
    b.pad_to(shoff);

    write_shdr(b, 0, 0, 0, 0, 0, 0, 0, 0, 0);

    for (uint32_t i = 0; i < n_text; ++i) {
        write_shdr(b, shstrtab.offsets.at(sections[i].sec_name), SHT_PROGBITS,
                   SHF_ALLOC | SHF_EXECINSTR,
                   text_sec_off[i], sections[i].size, 0, 0, 16, 0);
    }

    for (uint32_t i = 0; i < n_text; ++i) {
        if (sec_relocs[i].empty()) continue;

        std::string rela_name = ".rela" + sections[i].sec_name;
        write_shdr(b, shstrtab.offsets.at(rela_name), SHT_RELA,
                   0, rela_sec_off[i], rela_sec_size[i],
                   IDX_SYMTAB, 1 + i, 8, 24);
    }

    write_shdr(b, shstrtab.offsets.at(".data"), SHT_PROGBITS,
               SHF_ALLOC | SHF_WRITE, data_off, data_size, 0, 0, 8, 0);

    write_shdr(b, shstrtab.offsets.at(".rodata"), SHT_PROGBITS,
               SHF_ALLOC, rodata_off, rodata_size, 0, 0, 1, 0);

    write_shdr(b, shstrtab.offsets.at(".symtab"), SHT_SYMTAB,
               0, symtab_off, symtab_size, IDX_STRTAB, first_global, 8, 24);

    write_shdr(b, shstrtab.offsets.at(".strtab"), SHT_STRTAB,
               0, strtab_off, strtab_size, 0, 0, 1, 0);

    write_shdr(b, shstrtab.offsets.at(".shstrtab"), SHT_STRTAB,
               0, shstrtab_off, shstrtab_size, 0, 0, 1, 0);

    return std::move(b.data);
}
} // namespace quant::codegen::elf
