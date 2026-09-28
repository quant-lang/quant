# Contributing to Quant

Contributions are welcome. You can contribute to the compiler, standard library, tests, documentation, tools, or build system.

## Development Rules

* Read the existing implementation before changing it.
* Keep changes small and focused.
* Preserve the existing architecture and compiler invariants.
* Avoid unnecessary abstractions and unrelated refactors.
* Add tests for new behavior and bug fixes.
* Update documentation when user-visible behavior changes.
* Update `AI_CONTEXT.md` when compiler semantics, architecture, or important invariants change.

Compiler pipeline:

```text
Modules → Lexer → Parser → AST → Semantic → IR → Backend → ELF / PE32+
```

## Language Changes

For syntax or semantic changes:

1. Add the implementation.
2. Add tests.
3. Update `docs/lang.md`.
4. Update `AI_CONTEXT.md` when necessary.

Do not copy features or behavior from other languages without considering whether they fit Quant.

## Testing

New features should include tests.

Bug fixes should normally include a regression test.

Tests ending in `_err.qu` are expected to fail compilation and are used for error testing.

## AI

AI tools may be used for development, testing, documentation, analysis, and code generation.

AI-generated code must be understood, reviewed, built, and tested before submission.

Use `AI_CONTEXT.md` as the main context for AI tools working with Quant.

## Adding an OS ABI

Quant is OS-agnostic by design: the IR carries raw syscall numbers, while each
target defines how they are lowered and which standard library implementation is used.
If your OS is not listed in the [README target table](README.md#supported-targets),
PRs adding its ABI are welcome.

A minimal ABI port touches these places:

1. `include/quant/backend/mc.h` - add the OS to `TargetOS`.
2. Target registry (`src/backend/targets.cpp`) - add the canonical `<arch>-<os>` name
   and any aliases, plus the same name to `QUANT_KNOWN_BACKENDS` in `CMakeLists.txt`
   so it can be enabled via `-DQUANT_BACKENDS=...`. If it introduces a new
   architecture or executable format, extend the `QUANT_HAS_*` derivation in
   `CMakeLists.txt` and guard its dispatch in `src/backend/native_backend.cpp` so
   builds without this backend exclude its code entirely.
3. Backend (`src/backend/aarch64_isel.cpp` / `src/backend/isel.cpp`) — syscall lowering:
   number mapping, argument registers, and `_start` behavior. See
   `AArch64ISel::map_syscall` for the ZeroPoint example.
4. Standard library override in `std/<os>/` (e.g. `std/zp/io/io.qu`) - use the same
   module name (`module "std::io";`). Implementations are resolved per target
   automatically. Follow the pattern of `std/win/`.
5. Linking - executable format and linker flags in `src/main.cpp` if your OS differs
   from standard static ELF.
6. Semantic restrictions (optional) - if your ABI exposes a limited syscall surface,
   validate allowed `@syscall(N)` numbers in `src/semantic/semantic.cpp` so invalid
   syscalls fail at compile time rather than inside the kernel.

Then:

1. Add tests. A hello-world program under the new target is enough to start.
2. Update the target tables in `README.md`, `docs/lang.md`, and `AI_CONTEXT.md`.

Keep new ABIs behind their own `--target` value. Never change the behavior of
existing targets.

## Pull Requests

Before submitting a PR:

* build the project;
* run tests;
* add tests for new behavior;
* update documentation when needed;
* avoid unrelated changes.

Describe what changed, why, and how it was tested.

## Commits

Use:

```text
<type>: <short description>
```

Examples:

```text
feat: add enum parsing
fix: fix struct return
docs: update reference semantics
```

## Bug Reports

Include:

* Quant version or commit;
* minimal reproduction;
* expected behavior;
* actual behavior;
* compiler output when relevant.

## Feature Requests

Describe the problem, proposed behavior, example code, and why existing features are insufficient.

Do not add features solely because another language has them.
