#include "quant/backend/target.h"
#include "quant/backend/code_emitter.h"
#include "utils/logger.h"
#include "linux_policy.h"

namespace quant::codegen {
    namespace {
        class Arm64LinuxTarget final : public Target {
        public:
            std::string_view name() const override {
                return "aarch64-linux";
            }

            mc::TargetArch arch() const override {
                return mc::TargetArch::AARCH64;
            }

            mc::TargetOS os() const override {
                return mc::TargetOS::Linux;
            }

            mc::TargetAbi abi() const override {
                return mc::TargetAbi::AAPCS64;
            }

            mc::ObjectFormat object_format() const override {
                return linux_policy::object_format();
            }

            uint64_t image_base() const override {
                return 0;
            }

            void emit_entry(CodeEmitter& out, const std::string& entry_name) const override {
                auto& text = out.aarch64();
                text.bl(entry_name);
                text.mov_imm64(aarch64::X8, 93);
                text.svc();
            }

            void emit_exit(CodeEmitter& out, int value) const override {
                auto& text = out.aarch64();
                text.mov_imm64(aarch64::X0, static_cast<uint64_t>(value));
                text.mov_imm64(aarch64::X8, 93);
                text.svc();
            }

            void emit_mem_reserve(CodeEmitter& out, uint64_t size) const override {
                auto& text = out.aarch64();
                // mmap(0, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
                // Syscall 222 on aarch64 Linux.
                text.mov_imm64(aarch64::X0, 0);
                text.mov_imm64(aarch64::X1, size);
                text.mov_imm64(aarch64::X2, 3);          // PROT_READ|PROT_WRITE
                text.mov_imm64(aarch64::X3, 0x22);       // MAP_PRIVATE|MAP_ANONYMOUS
                text.mov_imm64(aarch64::X4, static_cast<uint64_t>(-1));
                text.mov_imm64(aarch64::X5, 0);
                text.mov_imm64(aarch64::X8, 222);
                text.svc();
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
                const auto nr = static_cast<uint32_t>(ir_number);
                switch (nr) {
                    case 0:   return 63;   // read
                    case 1:   return 64;   // write
                    case 2:   return 56;   // open
                    case 3:   return 57;   // close
                    case 8:   return 62;   // lseek
                    case 9:   return 222;  // mmap
                    case 11:  return 215;  // munmap
                    case 39:  return 174;  // getpid
                    case 57:  return 172;  // fork
                    case 59:  return 221;  // execve
                    case 60:  return 93;   // exit
                    case 74:  return 82;   // fsync
                    case 257: return 56;   // openat (close=57, openat on x86=257 -> aarch64=56)
                    default:  return nr;   // pass-through for unknown
                }
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
                return {"", "ld.lld", "", true, false};
            }

            std::string static_stdlib_name() const override {
                return linux_policy::static_stdlib_name(name());
            }
        };
    } // namespace

    const Target* make_aarch64_linux() {
        static const Arm64LinuxTarget target;
        return &target;
    }
} // namespace quant::codegen
