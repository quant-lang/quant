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

Each `quant::codegen::Target` represents one architecture/OS combination. Target-specific code lives in `src/backend/targets/`.

To add a new OS ABI:

1. **Add a target implementation**

   * Create `src/backend/targets/<arch>_<os>.cpp`.
   * Implement `Target` and add a `make_<arch>_<os>()` factory.
   * Keep architecture-specific instruction lowering in the existing instruction selectors.

2. **Register the target**

   * Add its name to `QUANT_KNOWN_BACKENDS` in `CMakeLists.txt`.
   * Register the target in `src/backend/registry.cpp`.
   * Reuse existing architecture, OS, and object-format enums when possible.

3. **Implement the OS ABI**
   Define the target's:

   * syscall mapping and validation;
   * calling convention;
   * runtime emission;
   * standard library policy;
   * linker settings.

   Use `stdlib_root()` for an optional OS-specific `std/<os>` tree. Use `link_spec()` for output extensions and linker options.

4. **Add tests**

   * Add target-specific tests where needed.
   * Update the target tables.
   * Run the existing target baseline checks.

A new architecture or OS may require changes to the corresponding enums in `mc.h`. A new instruction set or object format also requires the appropriate backend support.

### Important invariants

When adding or moving target code:

* ISel prepares function/entry symbols before target emission hooks run.
* Target hooks must preserve the existing memory-release calling sequence.
* Do not reorder `mc::SymbolTable` entries.
* Keep the existing AArch64 syscall number behavior, including its 32-bit truncation.
* Do not change existing target behavior as part of a structural refactor.

Build and test the compiler before and after the change. Compare target outputs with `scripts/check_target_baseline.py`.

For targets with restricted syscall ABIs, use `validate_syscall()`, `syscall_surface()`, and `syscall_abi_name()`.

Add CLI aliases in `registry.cpp` only when needed.


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
