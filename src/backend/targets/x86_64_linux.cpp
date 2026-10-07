#include "quant/backend/target.h"
#include "quant/backend/code_emitter.h"
#include "utils/logger.h"
#include "linux_policy.h"

namespace quant::codegen {
    namespace {
        class X64LinuxTarget final : public Target {
        public:
            std::string_view name() const override {
                return "x86_64-linux";
            }

            mc::TargetArch arch() const override {
                return mc::TargetArch::X86_64;
            }

            mc::TargetOS os() const override {
                return mc::TargetOS::Linux;
            }

            mc::TargetAbi abi() const override {
                return mc::TargetAbi::SysV;
            }

            mc::ObjectFormat object_format() const override {
                return linux_policy::object_format();
            }

            uint64_t image_base() const override {
                return 0;
            }

            void emit_entry(CodeEmitter& out, const std::string& entry_name) const override {
                auto& text = out.x86();
                text.call_rel32(entry_name);
                text.mov_r64_r64(x86::RDI, x86::RAX);
                text.mov_r64_imm(x86::RAX, 60);
                text.syscall();
            }

            void emit_exit(CodeEmitter& out, int value) const override {
                auto& text = out.x86();
                // exit(1) inline - no qk_exit dependency
                text.mov_r64_imm(x86::RDI, value);
                text.mov_r64_imm(x86::RAX, 60);
                text.syscall();
            }

            void emit_mem_reserve(CodeEmitter& out, uint64_t size) const override {
                auto& text = out.x86();
                // mmap(0, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
                text.mov_r64_imm(x86::RDI, 0);
                text.mov_r64_imm(x86::RSI, size);
                text.mov_r64_imm(x86::RDX, 3);
                text.mov_r64_imm(x86::R10, 0x22);
                text.mov_r64_imm(x86::R8, -1);
                text.mov_r64_imm(x86::R9, 0);
                text.mov_r64_imm(x86::RAX, 9);
                text.syscall();
            }

            void emit_mem_release(CodeEmitter& out) const override {
                auto& text = out.x86();
                // munmap(ptr, size)
                text.mov_r64_imm(x86::RAX, 11);
                text.syscall();
            }

            bool supports_syscalls() const override {
                return true;
            }

            void emit_syscall(CodeEmitter& out, uint64_t ir_number) const override {
                auto& text = out.x86();
                text.mov_r64_r64(x86::R10, x86::RCX);
                text.mov_r64_imm(x86::RAX, static_cast<int64_t>(map_syscall(ir_number)));
                text.syscall();
                text.ret();
            }

            uint64_t map_syscall(uint64_t ir_number) const override {
                return ir_number;
            }

            void declare_runtime_imports(CodeEmitter&) const override {}

            bool entry_emitted_first() const override {
                return false;
            }

            std::string_view stdlib_root() const override {
                return linux_policy::stdlib_root();
            }

            bool load_format_runtime() const override {
                return linux_policy::load_format_runtime();
            }

            bool validate_syscall(uint64_t n) const override {
                return linux_policy::validate_syscall(n);
            }

            std::string syscall_surface() const override {
                return linux_policy::syscall_surface();
            }

            LinkSpec link_spec() const override {
                return {"", "ld", "", true, false};
            }

            std::string static_stdlib_name() const override {
                return linux_policy::static_stdlib_name(name());
            }
        };
    } // namespace

    const Target* make_x86_64_linux() {
        static const X64LinuxTarget target;
        return &target;
    }
} // namespace quant::codegen
