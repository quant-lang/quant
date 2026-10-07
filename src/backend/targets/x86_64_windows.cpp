#include "quant/backend/target.h"
#include "quant/backend/code_emitter.h"
#include "utils/logger.h"

namespace quant::codegen {
    namespace {
        class X64WindowsTarget final : public Target {
        public:
            std::string_view name() const override {
                return "x86_64-windows";
            }

            mc::TargetArch arch() const override {
                return mc::TargetArch::X86_64;
            }

            mc::TargetOS os() const override {
                return mc::TargetOS::Windows;
            }

            mc::TargetAbi abi() const override {
                return mc::TargetAbi::Win64;
            }

            mc::ObjectFormat object_format() const override {
                return mc::ObjectFormat::PE;
            }

            uint64_t image_base() const override {
                return 0x140000000ull;
            }

            void emit_entry(CodeEmitter& out, const std::string& entry_name) const override {
                auto& text = out.x86();
                text.sub_rsp_imm32(8);
                if (out.symbols().has("qk_io_init")) {
                    text.call_rel32("qk_io_init");
                }
                text.call_rel32(entry_name);

                text.mov_r64_r64(x86::RCX, x86::RAX);
                text.sub_rsp_imm32(32);
                text.call_mem_rip("ExitProcess");
            }

            void emit_exit(CodeEmitter& out, int value) const override {
                auto& text = out.x86();
                // ExitProcess(1) - region overflow
                out.symbols().add_import_symbol("kernel32.dll", "ExitProcess");
                text.mov_r64_imm(x86::RCX, value);
                text.sub_rsp_imm32(32);
                text.call_mem_rip("ExitProcess");
            }

            void emit_mem_reserve(CodeEmitter& out, uint64_t size) const override {
                auto& text = out.x86();
                // VirtualAlloc(0, size, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE)
                text.mov_r64_imm(x86::RCX, 0);
                text.mov_r64_imm(x86::RDX, size);
                text.mov_r64_imm(x86::R8, 0x3000);
                text.mov_r64_imm(x86::R9, 0x04);
                text.sub_rsp_imm32(32);
                text.call_mem_rip("VirtualAlloc");
                text.add_rsp_imm32(32);
            }

            void emit_mem_release(CodeEmitter& out) const override {
                auto& text = out.x86();
                // VirtualFree(ptr, 0, MEM_RELEASE)
                text.mov_r64_mem(x86::RCX, x86::mem_base(x86::RAX, 0));
                text.mov_r64_imm(x86::RDX, 0);
                text.mov_r64_imm(x86::R8, 0x8000);
                text.sub_rsp_imm32(32);
                text.call_mem_rip("VirtualFree");
                text.add_rsp_imm32(32);
            }

            bool supports_syscalls() const override {
                return false;
            }

            void emit_syscall(CodeEmitter&, uint64_t) const override {
                utils::logger::crash("raw syscalls are not supported on the Windows target");
            }

            uint64_t map_syscall(uint64_t ir_number) const override {
                return ir_number;
            }

            void declare_runtime_imports(CodeEmitter& out) const override {
                out.symbols().add_import_symbol("kernel32.dll", "VirtualAlloc");
                out.symbols().add_import_symbol("kernel32.dll", "VirtualFree");
                out.symbols().add_import_symbol("kernel32.dll", "ExitProcess");
            }

            bool entry_emitted_first() const override {
                return false;
            }

            std::string_view stdlib_root() const override {
                return "std/win";
            }

            bool load_format_runtime() const override {
                return true;
            }

            bool validate_syscall(uint64_t) const override {
                return true;
            }

            std::string syscall_surface() const override {
                return "";
            }

            LinkSpec link_spec() const override {
                return {".exe", "", "", false, true};
            }

            std::string static_stdlib_name() const override {
                return "";
            }
        };
    } // namespace

    const Target* make_x86_64_windows() {
        static const X64WindowsTarget target;
        return &target;
    }
} // namespace quant::codegen
