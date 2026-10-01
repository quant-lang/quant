#include "quant/backend/pe_writer.h"

#include <cstring>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "utils/logger.h"

namespace quant::codegen::pe {

namespace {

constexpr uint64_t IMAGE_BASE = 0x140000000ull;
constexpr uint32_t FILE_ALIGN = 0x200;
constexpr uint32_t SEC_ALIGN = 0x1000;

constexpr uint16_t PE32P_MAGIC = 0x20B;
constexpr uint32_t SUBSYSTEM_CONSOLE = 3;
// NX_COMPAT (no ASLR: the image is pure PIC with RIP-relative addressing
// and has no .reloc section, so it must stay at the preferred base).
constexpr uint16_t DLL_CHARACTERISTICS = 0x100;
// EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE
constexpr uint16_t COFF_CHARACTERISTICS = 0x0002 | 0x0020;

enum : uint32_t {
    IMAGE_SCN_CNT_CODE = 0x20,
    IMAGE_SCN_CNT_INITIALIZED_DATA = 0x40,
    IMAGE_SCN_MEM_EXECUTE = 0x20000000,
    IMAGE_SCN_MEM_READ = 0x40000000,
    IMAGE_SCN_MEM_WRITE = 0x80000000,
};

constexpr uint32_t TEXT_CHARS =
    IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
constexpr uint32_t DATA_CHARS =
    IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
constexpr uint32_t RODATA_CHARS =
    IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;

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

std::size_t align_to(std::size_t n, std::size_t a) {
    return (n + a - 1) & ~(a - 1);
}

struct Import {
    std::string dll;
    std::string name;
};

// Unique (dll, name) pairs, in order of first appearance.
std::vector<Import> collect_imports(const mc::Object& obj) {
    std::vector<Import> out;
    std::unordered_map<std::string, bool> seen;
    for (const auto& s : obj.symbols) {
        if (!s.undefined || s.import_dll.empty()) continue;
        const std::string key = s.import_dll + '\0' + s.import_name;
        if (seen.count(key)) continue;
        seen[key] = true;
        out.push_back({s.import_dll, s.import_name});
    }
    return out;
}

void write_dos_header(Buf& b, uint32_t pe_offset) {
    b.pad_to(0x40);
    b.data[0] = 'M';
    b.data[1] = 'Z';
    // e_lfanew at file offset 0x3C
    b.data[0x3C] = static_cast<uint8_t>(pe_offset & 0xFF);
    b.data[0x3D] = static_cast<uint8_t>((pe_offset >> 8) & 0xFF);
    b.data[0x3E] = static_cast<uint8_t>((pe_offset >> 16) & 0xFF);
    b.data[0x3F] = static_cast<uint8_t>((pe_offset >> 24) & 0xFF);
}

void write_pe_header(Buf& b, uint16_t num_sections, uint32_t entry_rva,
                     uint32_t base_of_code, uint32_t size_of_image,
                     uint32_t size_of_headers, uint32_t size_of_code,
                     uint32_t size_of_init_data, uint32_t import_dir_rva,
                     uint32_t import_dir_size) {
    b.u32(0x00004550); // "PE\0\0"

    // COFF header
    b.u16(0x8664);                  // Machine: x64
    b.u16(num_sections);
    b.u32(0);                       // TimeDateStamp
    b.u32(0);                       // PointerToSymbolTable
    b.u32(0);                       // NumberOfSymbols
    b.u16(240);                     // SizeOfOptionalHeader (PE32+ with 16 dirs)
    b.u16(COFF_CHARACTERISTICS);

    // Optional header (PE32+)
    b.u16(PE32P_MAGIC);
    b.u8(0); b.u8(0);               // linker version
    b.u32(size_of_code);
    b.u32(size_of_init_data);
    b.u32(0);                       // SizeOfUninitializedData
    b.u32(entry_rva);
    b.u32(base_of_code);
    b.u64(IMAGE_BASE);
    b.u32(SEC_ALIGN);
    b.u32(FILE_ALIGN);
    b.u16(6); b.u16(0);             // OS version
    b.u16(0); b.u16(0);             // image version
    b.u16(6); b.u16(0);             // subsystem version
    b.u32(0);                       // Win32VersionValue
    b.u32(size_of_image);
    b.u32(size_of_headers);
    b.u32(0);                       // CheckSum
    b.u16(SUBSYSTEM_CONSOLE);
    b.u16(DLL_CHARACTERISTICS);
    b.u64(0x100000);                // SizeOfStackReserve
    b.u64(0x1000);                  // SizeOfStackCommit
    b.u64(0x100000);                // SizeOfHeapReserve
    b.u64(0x1000);                  // SizeOfHeapCommit
    b.u32(0);                       // LoaderFlags
    b.u32(16);                      // NumberOfRvaAndSizes

    // data directories
    for (int i = 0; i < 16; ++i) {
        if (i == 1) {
            b.u32(import_dir_rva);
            b.u32(import_dir_size);
        } else {
            b.u32(0);
            b.u32(0);
        }
    }
}

void write_section_header(Buf& b, const char* name, uint32_t virtual_size,
                          uint32_t rva, uint32_t raw_size, uint32_t raw_off,
                          uint32_t characteristics) {
    std::size_t pos = b.data.size();
    b.pad_to(pos + 8);
    for (int i = 0; name[i] && i < 8; ++i) b.data[pos + static_cast<std::size_t>(i)] = static_cast<uint8_t>(name[i]);
    b.u32(virtual_size);
    b.u32(rva);
    b.u32(raw_size);
    b.u32(raw_off);
    b.u32(0); // PointerToRelocations
    b.u32(0); // PointerToLinenumbers
    b.u16(0); // NumberOfRelocations
    b.u16(0); // NumberOfLinenumbers
    b.u32(characteristics);
}

} // namespace

std::vector<uint8_t> write(const mc::Object& obj) {
    const std::vector<Import> imports = collect_imports(obj);

    // Group import indices by DLL (order of first appearance).
    std::vector<std::string> dll_order;
    std::unordered_map<std::string, std::vector<uint32_t>> dll_idxs;
    for (uint32_t i = 0; i < imports.size(); ++i) {
        const auto& imp = imports[i];
        if (!dll_idxs.count(imp.dll)) dll_order.push_back(imp.dll);
        dll_idxs[imp.dll].push_back(i);
    }

    // --- Build the .rdata content ---
    // Per-import offset tables; RVAs are patched once rdata_rva is known.
    std::vector<uint32_t> hint_name_off(imports.size(), 0); // offset of hint/name entry
    std::vector<uint32_t> iat_off(imports.size(), 0);       // offset of IAT slot
    std::vector<uint32_t> ilt_off(imports.size(), 0);       // offset of ILT entry
    std::unordered_map<std::string, uint32_t> hint_name_by_name;
    std::unordered_map<std::string, uint32_t> iat_by_name; // (dll,name) -> IAT offset
    std::unordered_map<std::string, uint32_t> dll_name_off;
    std::vector<uint32_t> dll_ilt_off(dll_order.size(), 0);
    std::vector<uint32_t> dll_iat_off(dll_order.size(), 0);

    Buf rd;

    // Descriptor array (size known only after grouping; reserve generous space).
    const uint32_t desc_off = static_cast<uint32_t>(rd.data.size());
    rd.pad_to(desc_off + static_cast<uint32_t>(dll_order.size() + 1) * 20);

    // Hint/name entries (dedup by name).
    // Every entry must start at an even RVA: an odd thunk value is treated by
    // the loader as an import-by-ordinal (IMAGE_ORDINAL_FLAG bit 0).
    rd.pad_to(align_to(rd.data.size(), 2));
    for (uint32_t i = 0; i < imports.size(); ++i) {
        const auto& imp = imports[i];
        auto it = hint_name_by_name.find(imp.name);
        if (it != hint_name_by_name.end()) {
            hint_name_off[i] = it->second;
            continue;
        }
        rd.pad_to(align_to(rd.data.size(), 2));
        const uint32_t off = static_cast<uint32_t>(rd.data.size());
        rd.u16(0); // hint (ordinal) - unused
        rd.bytes(imp.name.data(), imp.name.size());
        rd.u8(0);
        hint_name_by_name[imp.name] = off;
        hint_name_off[i] = off;
    }

    // ILT and IAT per DLL.
    rd.pad_to(align_to(rd.data.size(), 8));
    for (uint32_t d = 0; d < dll_order.size(); ++d) {
        const auto& idxs = dll_idxs[dll_order[d]];
        dll_ilt_off[d] = static_cast<uint32_t>(rd.data.size());
        for (uint32_t k = 0; k < idxs.size(); ++k) {
            ilt_off[idxs[k]] = static_cast<uint32_t>(rd.data.size());
            rd.u64(0); // patched later with hint/name RVA
        }
        rd.u64(0); // null terminator
    }

    rd.pad_to(align_to(rd.data.size(), 8));
    for (uint32_t d = 0; d < dll_order.size(); ++d) {
        const auto& idxs = dll_idxs[dll_order[d]];
        dll_iat_off[d] = static_cast<uint32_t>(rd.data.size());
        for (uint32_t k = 0; k < idxs.size(); ++k) {
            iat_off[idxs[k]] = static_cast<uint32_t>(rd.data.size());
            rd.u64(0); // patched later; loader overwrites with the address
        }
        rd.u64(0); // null terminator
    }

    // DLL name strings.
    rd.pad_to(align_to(rd.data.size(), 1));
    for (uint32_t d = 0; d < dll_order.size(); ++d) {
        dll_name_off[dll_order[d]] = static_cast<uint32_t>(rd.data.size());
        rd.bytes(dll_order[d].data(), dll_order[d].size());
        rd.u8(0);
    }

    const uint32_t rdata_size = static_cast<uint32_t>(align_to(rd.data.size(), 8));

    // --- Section layout ---
    const uint32_t text_size = static_cast<uint32_t>(obj.text.size());
    const uint32_t data_size = static_cast<uint32_t>(obj.data.size());
    const uint32_t rodata_size = static_cast<uint32_t>(align_to(obj.rodata.size(), 8));

    constexpr uint32_t num_sections = 4;
    // DOS header (padded to 0x80) + PE sig + COFF header + optional header + section headers.
    constexpr uint32_t HEADERS_SIZE = 0x80 + 4 + 20 + 240 + num_sections * 40;
    const uint32_t size_of_headers = static_cast<uint32_t>(align_to(HEADERS_SIZE, FILE_ALIGN));

    const uint32_t text_rva = SEC_ALIGN; // first section
    const uint32_t text_off = size_of_headers;
    const uint32_t data_rva = static_cast<uint32_t>(align_to(text_rva + text_size, SEC_ALIGN));
    const uint32_t data_off = static_cast<uint32_t>(align_to(text_off + text_size, FILE_ALIGN));
    const uint32_t rdata_rva = static_cast<uint32_t>(align_to(data_rva + data_size, SEC_ALIGN));
    const uint32_t rdata_off = static_cast<uint32_t>(align_to(data_off + data_size, FILE_ALIGN));
    const uint32_t rodata_rva = static_cast<uint32_t>(align_to(rdata_rva + rdata_size, SEC_ALIGN));
    const uint32_t rodata_off = static_cast<uint32_t>(align_to(rdata_off + rdata_size, FILE_ALIGN));

    // --- Resolve import-related RVAs ---
    // IAT slot RVA for each import; symbol name -> IAT RVA.
    std::unordered_map<std::string, uint32_t> sym_iat_rva;
    for (uint32_t i = 0; i < imports.size(); ++i) {
        const uint32_t slot_rva = rdata_rva + iat_off[i];
        const uint32_t hint_rva = rdata_rva + hint_name_off[i];
        // patch ILT and IAT entries
        for (uint32_t k = 0; k < 8; ++k) {
            rd.data[ilt_off[i] + k] = static_cast<uint8_t>((hint_rva >> (8 * k)) & 0xFF);
            rd.data[iat_off[i] + k] = static_cast<uint8_t>((hint_rva >> (8 * k)) & 0xFF);
        }
        const std::string key = imports[i].dll + '\0' + imports[i].name;
        iat_by_name[key] = iat_off[i];
    }
    // Symbol name -> IAT slot RVA.
    for (const auto& s : obj.symbols) {
        if (!s.undefined || s.import_dll.empty()) continue;
        const std::string key = s.import_dll + '\0' + s.import_name;
        auto it = iat_by_name.find(key);
        if (it == iat_by_name.end()) continue;
        sym_iat_rva[s.name] = rdata_rva + it->second;
    }

    // --- Build import descriptors (patch into rd) ---
    for (uint32_t di = 0; di < dll_order.size(); ++di) {
        const auto& idxs = dll_idxs[dll_order[di]];
        const uint32_t ilt_rva = rdata_rva + dll_ilt_off[di];
        const uint32_t iat_rva = rdata_rva + dll_iat_off[di];
        const uint32_t name_rva = rdata_rva + dll_name_off[dll_order[di]];
        Buf desc;
        desc.u32(ilt_rva);
        desc.u32(0); // TimeDateStamp
        desc.u32(0); // ForwarderChain
        desc.u32(name_rva);
        desc.u32(iat_rva);
        std::memcpy(rd.data.data() + desc_off + di * 20, desc.data.data(), 20);
    }
    std::memset(rd.data.data() + desc_off + dll_order.size() * 20, 0, 20);
    rd.data.resize(rdata_size);

    const uint32_t import_dir_rva = rdata_rva + desc_off;
    const uint32_t import_dir_size = static_cast<uint32_t>(dll_order.size() + 1) * 20;

    // --- Resolve relocations in .text ---
    // Symbol -> RVA (defined symbols and import IAT slots).
    auto symbol_rva = [&](const mc::Symbol& s) -> uint32_t {
        if (s.undefined) {
            auto it = sym_iat_rva.find(s.name);
            if (it != sym_iat_rva.end()) return it->second;
            utils::logger::crash("PE: unresolved undefined symbol: " + s.name);
        }
        if (s.section == mc::SEC_DATA) return data_rva + static_cast<uint32_t>(s.value);
        if (s.section == mc::SEC_RODATA) return rodata_rva + static_cast<uint32_t>(s.value);
        return text_rva + static_cast<uint32_t>(s.value);
    };

    std::vector<uint8_t> text = obj.text;
    for (const auto& r : obj.relocs) {
        const mc::Symbol* sym = nullptr;
        for (const auto& s : obj.symbols) {
            if (s.name == r.symbol) { sym = &s; break; }
        }
        if (!sym) utils::logger::crash("PE: relocation references unknown symbol: " + r.symbol);
        const int64_t target_rva = static_cast<int64_t>(symbol_rva(*sym));
        const int64_t place_rva = static_cast<int64_t>(text_rva) + r.offset;
        switch (r.type) {
            case mc::RelType::X86_64_PC32:
            case mc::RelType::X86_64_PLT32: {
                const int64_t disp = target_rva + r.addend - place_rva;
                if (disp < INT32_MIN || disp > INT32_MAX) {
                    utils::logger::crash("PE: relocation out of range for symbol: " + r.symbol);
                }
                const uint32_t v = static_cast<uint32_t>(static_cast<int32_t>(disp));
                for (int k = 0; k < 4; ++k) {
                    text[r.offset + static_cast<std::size_t>(k)] = static_cast<uint8_t>((v >> (8 * k)) & 0xFF);
                }
                break;
            }
            default:
                utils::logger::crash("PE: unsupported relocation type");
        }
    }

    // --- Entry point ---
    const mc::Symbol* start = nullptr;
    for (const auto& s : obj.symbols) {
        if (s.name == "_start" && !s.undefined) { start = &s; break; }
    }
    if (!start) utils::logger::crash("PE: no _start symbol found");
    const uint32_t entry_rva = text_rva + static_cast<uint32_t>(start->value);

    const uint32_t size_of_image = static_cast<uint32_t>(align_to(rodata_rva + rodata_size, SEC_ALIGN));
    const uint32_t size_of_code = static_cast<uint32_t>(align_to(text_size, SEC_ALIGN));
    const uint32_t size_of_init_data = static_cast<uint32_t>(align_to(data_size + rdata_size + rodata_size, SEC_ALIGN));

    // --- Emit final image ---
    Buf out;
    write_dos_header(out, 0x80);
    out.pad_to(0x80);
    write_pe_header(out, num_sections, entry_rva, text_rva, size_of_image,
                    size_of_headers, size_of_code, size_of_init_data,
                    import_dir_rva, import_dir_size);

    out.pad_to(0x80 + 4 + 20 + 240);
    // Section headers must stay inside SizeOfHeaders.
    write_section_header(out, ".text", text_size, text_rva, text_size, text_off, TEXT_CHARS);
    write_section_header(out, ".data", data_size, data_rva, data_size, data_off, DATA_CHARS);
    write_section_header(out, ".rdata", rdata_size, rdata_rva, rdata_size, rdata_off, DATA_CHARS);
    write_section_header(out, ".rodata", rodata_size, rodata_rva, rodata_size, rodata_off, RODATA_CHARS);

    out.pad_to(text_off);
    out.bytes(text.data(), text.size());

    out.pad_to(data_off);
    out.bytes(obj.data.data(), obj.data.size());

    out.pad_to(rdata_off);
    out.bytes(rd.data.data(), rd.data.size());

    out.pad_to(rodata_off);
    out.bytes(obj.rodata.data(), obj.rodata.size());

    return std::move(out.data);
}

} // namespace quant::codegen::pe
