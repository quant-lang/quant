# Quant

Quant is an OS-agnostic systems programming language.

The compiler uses a native backend (IR -> machine code, no external assembler):
ELF executables on Linux and other ELF-based OSes, PE32+ on Windows.
x86-64 and AArch64 are supported; third-party OS ABIs are welcome (see [CONTRIBUTING.md](CONTRIBUTING.md)).

> Note: Quant is still in development and not everything is done

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

The documentation is in **Doc.md** file

---

## Architecture

```text
Load modules -> AST -> semantic analysis -> IR -> native binary
```

### Pipeline

0. Load all modules
1. Parse source code into AST
2. Run semantic validation passes
3. Generate intermediate representation
4. Generate machine code (native backend: ELF on Linux, PE32+ on Windows)

---

## Features

* OS-agnostic, with support for third-party operating systems
* Minimal hidden behavior
* Explicit behavior
* Attributes
* Arena-based compiler memory management
* Standard library written entirely in Quant
  - `io`
  - `format`
  - `heap`
  - `arena`
  - `vector`
  - `string`
  - etc.
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

---

## Usage

Compile to a native executable:

```bash
./qu file.qu -o program
```

Other options: `--emit-ir`, `--emit-asm`, `--no-compile`, `--time`.

---
## Supported Targets

Quant is not tied to a specific operating system. The `@syscall(N)` attribute lowers
to the raw syscall interface of the selected target, while the standard library
automatically selects the appropriate OS implementation (`std/win/`, `std/zp/`, ...).

Backends are chosen at CMake configure time; `--target` then picks one of the enabled
ones at compile time (a known-but-disabled target fails with a clear error, no silent fallback):

```bash
cmake -B build -DQUANT_BACKENDS="x86_64-linux;aarch64-linux;aarch64-zeropoint"
# optional: pick a different default target for builds without the host backend
cmake -B build -DQUANT_BACKENDS="aarch64-zeropoint" -DQUANT_DEFAULT_TARGET="aarch64-zeropoint"
```

| Target | Flag | Output | Status | Notes |
|--------|------|--------|--------|-------|
| Linux x86-64 | *(default)* | ELF | supported | Syscall numbers use the x86-64 Linux ABI |
| Linux AArch64 | `--target aarch64` | ELF | supported | Syscall numbers are mapped automatically |
| Windows x86-64 | *(native on Windows)* | PE32+ | supported | WinAPI via `@import`, no direct syscalls |
| [ZeroPoint](https://github.com/Operator-about/ZeroPoint) | `--target aarch64-zeropoint` | static-PIE ELF | experimental | Custom ABI with single-buffer syscalls, base `0x40000000` |

Quant can target custom operating systems and architectures as well. Adding a new
OS ABI is a small, self-contained change. See
[CONTRIBUTING.md -> Adding an OS ABI](CONTRIBUTING.md).

---

## Project Status

| Component            | Status      |
| -------------------- | ----------- |
| Lexer                | completed   |
| Parser               | completed   |
| AST                  | completed   |
| Semantic analysis    | completed   |
| IR                   | completed   |
| Native backend       | completed (Linux & Windows) |
| Optimizations        | completed     |
| Self-hosted compiler | in progress     |

---

## Todo
* [x] Basic optimizations
* [x] Advanced optimizations
* [x] Compile time operator ('#')
* [ ] Windows .lib support
* [ ] Rewrite front-end in Quant; make linking with qu & cpp in CMake and -DQUANT_FRONTEND flag

---

## Contributing

Contributions related to compilers and systems programming are welcome.

Porting Quant to your own OS or adding its ABI is explicitly welcome — see
[CONTRIBUTING.md → Adding an OS ABI](CONTRIBUTING.md).

---

## License
This project is under the [GPL-3.0](https://www.gnu.org/licenses/gpl-3.0.html) licence.
