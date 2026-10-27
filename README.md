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

The documentation is in [docs/lang.md](lang.md) and code style in [docs/codestyle.md](codestyle.md)

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

Priority order: **P0** first, then **P1**, **P2**, and **P3** last.

### P0 - Fix first: crashes, broken output, and diagnostics

* [x] Fix the compiler crash on malformed declarations. `tests/bad_function_err.qu` currently exits with a crash code instead of a normal compile error.
* [x] Fix Windows `.exe` files that are shorter than their `.rodata` section. `src/backend/pe_writer.cpp` writes the aligned `.rodata` size in the header but does not pad the last section. Windows then fails with `WinError 193`. Reproduce with `tests/else_if_str_test.qu -O0` or `tests/main.qu -O2`. This also breaks `scripts/check_opt.py` and `scripts/check_comptime.py`.
* [ ] Use one diagnostic format everywhere: show the location and source token first, then the message.
* [ ] Show the source token in every error. Improve unclear messages, for example "Undefined qualified symbol" should say which symbol was not found.
* [x] Unify all type mismatch errors. Always show the expected and received types.
* [x] Stop printing internal type IDs. Bad examples: `Type mismatch in assignment: Node vs 4` and `14 vs 4`.
* [ ] Fix the wrong error for an undefined variable used as the right-hand side of a variable initializer.
* [ ] Keep reporting independent semantic errors. For example, "Cannot assign to immutable variable" must not hide "Missing return in non-void function".

### P1 - Parser recovery and type checks

* [ ] Add parser recovery to `switch` and `enum` bodies, like the recovery already used for structs. Braces in these constructs are not a block, and one bad case should not create many follow-up errors.
* [ ] Continue semantic analysis after parse errors when it is safe, so more independent errors are reported.
* [ ] Check that struct fields are initialized before they are read. Variables already have this check, but fields do not.
* [ ] Fix the development-build module root. `tests/attr_hide_pub.qu` cannot load `tests::lib_hidden` because `root_path` resolves to `build/bin`.

### P2 - Backends, standard library, docs and quality of life

* [ ] Fix ZeroPoint exit. `Arm64ZeroPointTarget::emit_exit` uses Linux syscall 93, but the ZeroPoint ABI reserves syscall 20 for exit and documents it as not implemented. Decide between trapping and waiting for kernel support; do not call the Linux number.
* [ ] Add explicit checked accessors for `string` and `Vec` (`try_get` returning `option<T>`). Keep `get` unchecked. This matches the "no hidden behavior" rule.
* [ ] Unify std type names. Today `string`, `option`, and `slice` use snake_case, while `Vec` uses PascalCase. Pick one rule and update the std, tests and docs.
* [ ] Add a high-level `read_line` that stores the result in an owning `string` and removes the trailing `\n`. Allocation and freeing must stay explicit.
* [ ] Improve struct literal inference from context, for example in call arguments and return values. Do not add general type inference.
* [ ] Add a way to initialize an empty struct, for example `A a = A{};`.
* [ ] Make the trailing `;` optional after `struct` and `enum` (accept both forms).
* [ ] Fix the docs: a non-void function needs a return on **every** path, not "at least one return".
* [ ] Fix the docs: enum variants are now scoped (`Enum::Variant`); bare names are an error. Update `docs/lang.md` and `AI_CONTEXT.md`.
* [ ] Remove `--emit-asm` from the README and docs. The flag no longer exists.

### P3 - Long-term features

* [ ] Conditional operator (`cond ? a : b`). It fits the explicit style.
* [ ] Normal per-file compilation with per-file IR and standard linking.
* [ ] Build system.
* [ ] Fix the `scripts/check_errors.py` summary. It prints `unexpected_ok` in the `UNEXPECTED exit codes` line, so compiler crashes are listed but counted as 0.

---
## Self-hosting
* [x] front-end
  - [x] lexer
  - [x] parser
* [ ] middle-end
  - [ ] semantic
  - [ ] comptime
* [ ] back-end
  - [ ] ir
  - [ ] codegen

---

## Contributing

Contributions related to compilers and systems programming are welcome.

Porting Quant to your own OS or adding its ABI is explicitly welcome — see
[CONTRIBUTING.md → Adding an OS ABI](CONTRIBUTING.md).

---

## License
This project is under the [GPL-3.0](https://www.gnu.org/licenses/gpl-3.0.html) licence.
