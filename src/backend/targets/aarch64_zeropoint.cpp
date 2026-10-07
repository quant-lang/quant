#include "quant/backend/target.h"
#include "quant/backend/code_emitter.h"
#include "utils/logger.h"

namespace quant::codegen {
    namespace {
        class Arm64ZeroPointTarget final : public Target {
        public:
            std::string_view name() const override {
                return "aarch64-zeropoint";
            }

            mc::TargetArch arch() const override {
                return mc::TargetArch::AARCH64;
            }

            mc::TargetOS os() const override {
                return mc::TargetOS::ZeroPoint;
            }

            mc::TargetAbi abi() const override {
                return mc::TargetAbi::AAPCS64;
            }

            mc::ObjectFormat object_format() const override {
                return mc::ObjectFormat::ELF;
            }

            uint64_t image_base() const override {
                return 0x40000000;
            }

            void emit_entry(CodeEmitter& out, const std::string& entry_name) const override {
                auto& text = out.aarch64();
                text.bl(entry_name);
                text.wfe();
                text.b(-4);
            }

            void emit_exit(CodeEmitter& out, int value) const override {
                auto& text = out.aarch64();
                text.mov_imm64(aarch64::X0, static_cast<uint64_t>(value));
                text.mov_imm64(aarch64::X8, 93);
                text.svc();
            }

            void emit_mem_reserve(CodeEmitter&, uint64_t) const override {
                utils::logger::crash("region alloc is not supported on the ZeroPoint target");
            }

            void emit_mem_release(CodeEmitter& out) const override {
                auto& text = out.aarch64();
                text.mov_imm64(aarch64::X8, 215);
                text.svc();
            }

            bool supports_syscalls() const override {
                return true;
            }

            void emit_syscall(CodeEmitter& out, uint64_t ir_number) const override {
                auto& text = out.aarch64();
                text.mov_imm64(aarch64::X8, map_syscall(ir_number));
                text.svc();
                text.ret();
            }

            uint64_t map_syscall(uint64_t ir_number) const override {
                return static_cast<uint32_t>(ir_number);
            }

            void declare_runtime_imports(CodeEmitter&) const override {}

            bool entry_emitted_first() const override {
                return true;
            }

            std::string_view stdlib_root() const override {
                return "std/zp";
            }

            bool load_format_runtime() const override {
                return false;
            }

            bool validate_syscall(uint64_t n) const override {
                return n == 0 || n == 1 || n == 10 || n == 20;
            }

            std::string syscall_abi_name() const override { return "ZeroPoint"; }

            std::string syscall_surface() const override {
                return "0=write, 1=read, 10=open, 20=exit";
            }

            LinkSpec link_spec() const override {
                return {"", "ld.lld", "-pie --image-base=0x40000000", true, false};
            }

            std::string static_stdlib_name() const override {
                return "";
            }
        };
    } // namespace

    const Target* make_aarch64_zeropoint() {
        static const Arm64ZeroPointTarget target;
        return &target;
    }
} // namespace quant::codegen
