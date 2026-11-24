# Quant

Quant is an OS-agnostic systems programming language.

The compiler uses a native backend (IR -> machine code, no external assembler):
ELF executables on Linux and other ELF-based OSes, PE32+ on Windows.
x86-64 and AArch64 are supported; third-party OS ABIs are welcome (see [CONTRIBUTING.md](CONTRIBUTING.md)).

> Note: Quant is still in development and not everything is complete

---

## Example

```qu
load "std::io";
using std::io;

@entry i32 main() {
    i32 value = 10;
    mut i32 result = value + 2;

    if (value > 5) {
        result = 0;
    } else {
        result = 1;
    }
    str msg = result as str; // casting
    println(msg);
    return 0;
}
```

## Documentation

The documentation is in [docs/lang.md](docs/lang.md) and code style in [docs/codestyle.md](docs/codestyle.md)

---

## Architecture

```text
Load modules -> AST -> attribute lowering -> semantic analysis -> dynamic constructions attribute lowering -> IR -> native binary
```

### Pipeline

0. Load all modules
1. Parse source code into AST
2. Run attribute lowering passes (rewrite supported runtime attributes, e.g. `@init`)
3. Run semantic validation passes
4. Generate intermediate representation
5. Generate machine code (native backend: ELF on Linux, PE32+ on Windows)

---

## Features

* OS-agnostic, with support for third-party operating systems
* Minimal hidden behavior
* Explicit behavior
* Attributes
* Arena-based compiler memory management
* Standard library written entirely in Quant
  * `io`
  * `format`
  * `heap`
  * `arena`
  * `vector`
  * `string`
  * etc.
* Windows standard library through `@import` and WinAPI, with no assembly runtime

---

## Build

### Requirements

* CMake 3.20+
* C++20 compiler

Build the compiler:

```bash
git clone https://github.com/quant-lang/quant.git
cd quant

cmake -B build
cmake --build build
```

### Prebuilt stdlib (`libqu`)

On Linux, `-DQUANT_USE_LIBQU=ON` builds `std/std.qu` into one
`libqu-<arch>-linux.a` per enabled Linux backend (using a bootstrap compiler
built from the same sources) and embeds those archives into `qu`. At runtime
the compiler unpacks the archive for the selected `--target` into the temp dir
and links against it, instead of regenerating stdlib code on every run. The
`.qu` sources stay embedded for parsing and semantic analysis.

```bash
cmake -B build -DQUANT_USE_LIBQU=ON
cmake --build build

# optional: pick the linker/archiver used while building the archives
cmake -B build -DQUANT_USE_LIBQU=ON -DQUANT_LD=lld -DQUANT_AR=llvm-ar
```

---

## Usage

Compile to a native executable:

```bash
./build/bin/qu file.qu -o program
```

Other options: `--emit-ir`, `--no-compile`, `--time`.

---

## Supported Targets

Quant is not tied to a specific operating system. The `@syscall(N)` attribute lowers
to the raw syscall interface of the selected target, while the standard library
automatically selects the appropriate OS implementation (`std/win/`, `std/zp/`, `std/superos/`, ...).

Backends are chosen at CMake configure time; `--target` then picks one of the enabled
ones at compile time (a known-but-disabled target fails with a clear error, no silent fallback):

```bash
cmake -B build -DQUANT_BACKENDS="x86_64-linux;aarch64-linux;aarch64-zeropoint;x86_64-superos"
# optional: pick a different default target for builds without the host backend
cmake -B build -DQUANT_BACKENDS="x86_64-superos" -DQUANT_DEFAULT_TARGET="x86_64-superos"
```

| Target | Flag | Output | Status | Notes |
| -------- | ------ | -------- | -------- | ------- |
| Linux x86-64 | *(default)* | ELF | supported | Syscall numbers use the x86-64 Linux ABI |
| Linux AArch64 | `--target aarch64` | ELF | supported | Syscall numbers are mapped automatically |
| Windows x86-64 | `--target x86_64-windows` | PE32+ | supported | WinAPI via `@import`, no direct syscalls |
| [ZeroPoint](https://github.com/Operator-about/ZeroPoint) | `--target aarch64-zeropoint` | static-PIE ELF | experimental | Custom ABI with single-buffer syscalls, base `0x40000000` |
| [SUPER OS](https://github.com/grafmorkov/super-os) | `--target x86_64-superos` | ELF64 `ET_EXEC` | experimental | x86-64 ring-3 userland, `int 0x80` syscalls, base `0x400000` |

Quant can target custom operating systems and architectures as well. Adding a new
OS ABI is a small, self-contained change. See
[CONTRIBUTING.md -> Adding an OS ABI](CONTRIBUTING.md).

---

## Project Status

| Component            | Status                      |
| -------------------- | --------------------------- |
| Lexer                | completed                   |
| Parser               | completed                   |
| AST                  | completed                   |
| Semantic analysis    | completed                   |
| IR                   | completed                   |
| Native backend       | completed (Linux & Windows) |
| Optimizations        | completed                   |
| Self-hosted compiler | in progress                 |

---

## Todo

* [ ] Constructors and destructors in structs (like dynamics with @onbegin @onend)
* [ ] Conditional operator (`cond ? a : b`). It fits the explicit style
* [x] LSP
* [ ] Normal per-file compilation with per-file IR and standard linking.
* [ ] Build system (like cmake, optional)
* [ ] refactor semantic.cpp and ir.cpp

---

## Self-hosting

* [x] front-end
  * [x] lexer
  * [x] parser
* [ ] middle-end
  * [ ] semantic
  * [ ] comptime
* [ ] back-end
  * [ ] ir
  * [ ] codegen

---

## Contributing

Contributions related to compilers and systems programming are welcome.

Porting Quant to your own OS or adding its ABI is explicitly welcome — see
[CONTRIBUTING.md → Adding an OS ABI](CONTRIBUTING.md).

---

## License

This project is under the [GPL-3.0](https://www.gnu.org/licenses/gpl-3.0.html) licence.
