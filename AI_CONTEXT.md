# AI_CONTEXT.md - Working with the Quant Compiler

This file describes Quant's language semantics and compiler architecture as implemented.
It is written for LLMs to avoid wrong assumptions when generating or analyzing Quant code.

---

## 1. Language Model

Quant is an imperative, statically-typed systems language. Syntax uses C-like braces and semicolons.

- All variables require explicit type annotations: `i32 x = 5;`
- All functions require an explicit return type (`void` or the actual type).
- No type inference for variables or return types.
- No garbage collector, no RAII, no automatic destructors.
- Memory is managed via regions (arena) and explicit heap calls.
- Strings are immutable `str` (pointer to bytes, 8 bytes).
- `char` is an alias for `u8`.

Integer literals default to `i32`. Float literals default to `f64`. Boolean literals are `true`/`false` of type `bool`.

---

## 2. Type System

### Built-in Types

| Type     | Size                 | Notes                                    |
|----------|----------------------|------------------------------------------|
| void     | 0                    | no value                                 |
| bool     | 1 byte (stored as 8-byte qword) | boolean            |
| i8       | 1 byte               | signed byte                              |
| i16      | 2 bytes              | signed short                             |
| i32      | 4 bytes              | signed int (default for int literals)    |
| i64      | 8 bytes              | signed long                              |
| u8       | 1 byte               | unsigned byte, also `char` alias         |
| u16      | 2 bytes              | unsigned short                           |
| u32      | 4 bytes              | unsigned int                             |
| u64      | 8 bytes              | unsigned long                            |
| f32      | 4 bytes              | float                                    |
| f64      | 8 bytes              | double (default for float literals)      |
| str      | 8 bytes              | pointer to string data                   |
| *T       | 8 bytes              | pointer to T                             |
| &T       | 8 bytes              | reference to T                           |
| nullptr  | 8 bytes              | null pointer value                       |

### Struct field layout

**Critical invariant:** every struct field occupies exactly 8 bytes in memory, regardless of its actual type. A `bool` field and an `i64` field both consume 8 bytes. Fields are laid out in declaration order with no padding.

### Enums

C-style enums with `i32` values starting at 0. Variants are accessible by bare name or qualified (`Color::Green`). Enums are compile-time only - they produce no IR.

---

## 3. Mutability

- Variables are **immutable by default**. Use `mut` to make them mutable.
- Immutable variables **must be initialized** at declaration.
- `mut` parameters: `void foo(mut i32 x)` - the parameter can be reassigned inside the function.
- Struct fields can be `mut`: `mut i32 val;`
- Assignment to immutable variables is a compile error.
- Enum variant variables are immutable and store their value at compile time.

### @init

`@init` suppresses the initialization check for immutable variables:

```
@init i32 x;
// x is uninitialized, but the compiler trusts you.
// Later assignment marks it as initialized.
```

Without `@init`, an immutable variable without an initializer is a compile error.

---

## 4. Nullability

- `nullptr` can only be stored in pointer variables (`*T`), never in references (`&T`).
- Once a pointer variable has held `nullptr`, the compiler marks it as **"maybe null" forever**, even if later assigned a non-null value.
- A "maybe null" pointer **cannot be converted to a reference** - this is a compile error.
- Pointer function parameters are always considered "maybe null" (callers can pass `nullptr`).
- `alloc()`, `heap_malloc()`, and `heap_realloc()` are known non-null.
- `&x` (address-of) is known non-null.
- Other pointer expressions (struct fields, index results, casts, function returns) are conservatively treated as nullable.

```
*i32 p = nullptr;
p = alloc(i32, 1);  // p is still "maybe null"
&r = p;             // ERROR: cannot convert nullable pointer to reference
```

---

## 5. Pointers and References

Both `*T` and `&T` are 8 bytes.

### Pointers (*T)

- Can hold `nullptr`.
- Indexing: `p[i]` - only valid on pointer types.
- Pointer arithmetic: not directly supported. Use `as!` to manipulate address values.
- Only heap-allocated or region-allocated memory should be pointed to.

### References (&T)

- Cannot hold `nullptr` (enforced by nullability analysis).
- Auto-dereferenced for field access and indexing.
- Can be reassigned (if `mut`): `mut &Point r = &a; r = &b;`
- Cannot be taken to builtin value types (`i32`, `bool`, `f64`, etc.).
- Can be taken from: structs, pointers, other references.

```
Point p = {10, 20};
&r = &p;    // ok
r.x;        // auto-dereferenced
```

### Interop

`*T` and `&T` of the same pointed type are assignment-compatible. But nullability blocks conversion to `&T`.

---

## 6. Regions and Memory

### Regions

```
region r {
    *void p = alloc(32);         // untyped: returns *void
    *i32 arr = alloc(i32, 10);   // typed: returns *i32, 10 elements
}
// All pointers from this region are dead.
```

- `alloc(size)` - untyped allocation (bytes), returns `*void`.
- `alloc(T, count)` - typed allocation, returns `*T`.
- Nested regions are supported.
- Default region size is 1 MB (hardcoded in IR generation).
- `alloc` is only valid inside a `region` block.

### Heap

The standard library provides `std::heap::heap_malloc`, `std::heap::heap_realloc`, `std::heap::heap_free`. These are separate from region memory.

### Arena allocator

`std::arena::Region` provides a bump allocator backed by `mmap`. Must be explicitly destroyed.

---

## 7. Control Flow

### if / else if / else

Conditions must be of type `bool`.

```
if (x < 10) { ... }
else if (x == 10) { ... }
else { ... }
```

### while

```
while (condition) { ... }   // condition must be bool
```

### for

`for` is **desugared to while** at parse time:

```
for (init; cond; step) { body }
```
becomes:
```
{ init; while (cond) { body; step; } }
```

**Important:** `continue` jumps to the **step expression**, executes it, then re-checks the condition. This matches C semantics.

```
for (mut i32 i = 0; i < 10; i++) {
    if (i == 3) continue;  // i IS incremented here (step runs), loop progresses
}
```

### break / continue

- `break` exits the nearest `while` or `switch`.
- `continue` jumps to the nearest enclosing loop's continue target: for a plain `while` this is the condition check; for a desugared `for` this is the step expression (the step runs, then the condition is re-checked).
- Both are checked at compile time.

### switch / case / default

```
switch (x) {
    case 1: { ... }
    case 2: { ... }
    default: { ... }
}
```

- Switch expression must be `bool`, `char` (`u8`), or integer type.
- Case values must be **compile-time constants** (literals, enum variants, or const immutable variables).
- Duplicate case values are a compile error.
- `default` is optional.
- Multiple `case` labels can share one body (C-style fallthrough).
- A switch terminates only if every branch terminates.

---

## 8. Functions

```
i32 add(i32 x, i32 y) { return x + y; }
void greet(str name) { ... }
@entry i32 main() { return 0; }
```

- Return type is always required.
- `mut` parameters: `void bump(mut i32 x)` - allows reassignment inside.
- Functions must have at least one `return` if return type is not `void`.
- `extern` functions have no body: `extern void print(str text);`
- Struct-returning functions use sret (hidden pointer as first argument).

### Methods

Methods are defined inside structs:

```
struct Counter {
    i32 count;

    void bump() {
        count = count + 1;    // bare field access, no 'self.' prefix
    }
};
```

- Methods receive a hidden `self` (pointer to struct) as the first argument.
- Inside method bodies, struct fields are in scope without prefix.
- A method can call sibling methods of the same struct directly.
- Method calls on pointers and references are auto-dereferenced: `p.method()` works.

### Overloading

No function overloading by parameter types. Function signatures (name + argument types + return type) must be unique. Two declarations with the same name but different signatures is a compile error.

---

## 9. Structs and Methods

```
struct Point {
    i32 x;
    i32 y;

    i32 manhattan() { return x + y; }
};
```

### Initialization

```
Point p = {10, 20};        // positional field init
Point q = Point{30, 40};   // explicit type syntax
```

- Argument count must match field count exactly.
- Each argument type must match the field type.
- No named initialization syntax.
- No default field values in struct init expression (but fields can have defaults in declaration).

### Field defaults

```
struct Config {
    bool enabled = true;   // default value
};
```

### Methods on multiple structs

Multiple structs can have methods with the same name - they are distinct functions under different qualified names (`Struct::method`).

### No operator overloading

Attempting `==`, `+`, etc. on struct values produces: "Operator overloading is not supported for this type".

---

## 10. Generics

### Generic Structs

```
struct Box<T> {
    T value;

    void set(T v) { value = v; }
    T get() { return value; }
};
```

- Instantiated lazily: `Box$i32` is created only when a field is first accessed.
- Multiple type parameters: `struct Pair<A, B> { ... }`
- Generic struct methods are monomorphized per concrete instantiation.

### Generic Functions

```
T identity<T>(T x) { return x; }
```

- Call with explicit type args: `identity<i32>(42)`
- Type inference from arguments is supported.
- Each concrete instantiation becomes a separate function.
- Mangled names: `func_name$size_type` (e.g., `identity$4i32`).

### Rules

- Generic functions are only analyzed and code-generated when instantiated with concrete types.
- Generic struct methods (methods with their own type params) are **not supported** - methods inherit the struct's type params only.
- Type arguments are substituted at call sites and during instantiation.

---

## 11. Modules

### Loading

```
load "std::io";
```

- Module path uses `::` as separator: `"std::io"`.
- The module file must be named after the last path segment: `io.qu` in `std/io/`.

### Namespace access

```
std::io::print("hello");
```

- All symbols are accessed fully qualified unless imported.

### using

```
load "std::io";
using std::io;

print("hello");  // instead of std::io::print
```

- Imports all symbols from a namespace into the current scope.
- Visibility (`@public`/`@private`) is still enforced at the use-site.

### module declaration

```
module "std::io";
```

- Declares the module name in the source file. Must match the load path.

### extern

```
extern void print(str text);
@syscall(1) extern i64 sys_write(i64 fd, str buf, i64 len);
```

- `@syscall(N)` source-level numbers follow the x86-64 Linux convention; the AArch64 backend remaps them automatically.
- On the ZeroPoint target (`--target aarch64-zeropoint`) the number N is used as-is (ZeroPoint ABI: write=0, read=1, open=10, exit=20).

---

## 12. Attributes

Syntax: `@name` or `@name(args)` before a declaration.

| Attribute  | Targets                              | Args | Description                                    |
|------------|--------------------------------------|------|------------------------------------------------|
| `@entry`   | function                             | 0    | Program entry point                            |
| `@init`    | variable                             | 0    | Suppress uninitialized variable check          |
| `@guard`   | variable, field                      | 1    | Compile-time condition check on use            |
| `@public`  | function, variable, field, struct    | 0    | Visible outside the module                     |
| `@private` | function, variable, field, struct    | 0    | Hidden from other modules                      |
| `@hide`    | any top-level declaration            | 0    | All following declarations private by default  |
| `@unhide`  | any top-level declaration            | 0    | Reset @hide — following are public             |
| `@syscall` | function (extern only)               | 1    | Lower to a raw syscall N (remapped per target) |
| `@export`  | function                             | 1    | Fixed symbol name in object file               |
| `@import`  | function (extern only)               | 1-2  | Import from Windows DLL                        |

### @guard

Validates a condition at compile time when the value is used (passed to a function or when a struct field is accessed). Condition must be a compile-time constant expression.

```
@guard(count > 0) mut i32 value;
value = 10;               // writes are not guarded
work(value);              // checked at compile time
```

If the condition evaluates to `false`, compilation fails with "guard failed".

---

## 13. Type Conversions

### Implicit widening only

No implicit narrowing. No implicit signed<->unsigned unless the target is strictly wider.

```
i8 a = 42;             // fits
i8 b = 300;            // ERROR: does not fit
i16 c = 30000;         // fits
i32 d = c;             // i16 -> i32: safe widening
```

Literals adapt to the target type when the value fits. Mixed-type binary expressions promote to a common type.

### as - value conversion

Numeric <-> numeric, or numeric -> string.

```
i64 a = 42 as i64;
i8 b = 1000 as i8;     // truncation (explicit only)
str s = 42 as str;     // number -> string
```

Only numeric types can be converted. Struct-to-struct or struct-to-int conversions are not allowed.

### as! - bit reinterpretation

Source and target must have the same size. No size check for pointer <-> pointer or reference <-> reference.

```
u32 x = -1 as i32 as! u32;
*i32 p = 0 as! *i32;
```

### Usual arithmetic conversions

Binary operations on mixed numeric types promote to a common type:
1. If either operand is `f64`, both become `f64`.
2. Otherwise if either is `f32`, both become `f32`.
3. For integer types: same signedness means larger wins; signed wins only if strictly larger.

### Comparison operators

Comparison operators work with mixed numeric types. Both operands are promoted to a common type, and the result is always `bool`. For non-numeric types, only `==` and `!=` are supported (and both sides must be the same type).

### Logical operators

`&&` and `||` require `bool` operands only. The unary `!` operator only works with `bool`. No implicit integer-to-bool conversion.

---

## 14. Standard Library

All stdlib is written in pure Quant. Key modules:

| Module        | Description                                    |
|---------------|------------------------------------------------|
| `std::io`     | `print`, `println`, `print_char`, `exit`       |
| `std::format` | String formatting                              |
| `std::string` | String operations                              |
| `std::math`   | Math functions                                 |
| `std::cmp`    | Ordering, comparison                           |
| `std::heap`   | `heap_malloc`, `heap_realloc`, `heap_free`     |
| `std::arena`  | Bump allocator backed by mmap                  |
| `std::vector` | Generic `Vec<T>` backed by heap                |
| `std::types`  | `option<T>`, `either<L,R>`, `pair<A,B>`, `slice<T>`, `result<T,E>` |

On Linux, I/O uses `@syscall`. On Windows, `@import` for WinAPI. On ZeroPoint (`std/zp/io/io.qu`), single-buffer syscalls: `print(str)`, `read(str)`, `open(str)` — the OS computes everything except the buffer.

`option<T>` uses `@guard(has_value)` on the value field. `either<L,R>` uses guards on both variants.

---

## 15. Compiler Architecture

### Pipeline

```
Source -> Lexer -> Parser -> AST -> Semantic Analysis -> IR -> IR Optimizer -> Native Backend -> ELF/PE32+
```

On Linux, the native backend links with `ld` (x86-64) or `ld.lld` (AArch64).

### Components

| Directory       | Purpose                                          |
|-----------------|--------------------------------------------------|
| `src/frontend/` | Lexer (`lexer.cpp`), Parser (`parser.cpp`), AST (`ast.cpp`) |
| `src/semantic/` | Semantic analysis, symbol table, type checking   |
| `src/ir/`       | IR generation (`ir_gen.cpp`), IR optimizer (`opt.cpp`), IR dump |
| `src/backend/`  | Instruction selection (x86-64 `isel.cpp`, AArch64 `aarch64_isel.cpp`), emitters (`fasmcodegen.cpp`, `aarch_64.cpp`), ELF writer, PE writer |
| `src/modules/`  | Module loading                                   |
| `src/support/`  | Type context, symbol path utilities              |
| `src/utils/`    | Logger, error reporting, file manager            |
| `include/`      | All headers (mirrors `src/` structure)           |
| `std/`          | Standard library in Quant                        |

### Key files

- `src/semantic/semantic.cpp` (2852 lines) - the largest file; all type checking, attribute validation, generic instantiation.
- `src/ir/ir_gen.cpp` (2081 lines) - IR generation from AST.
- `include/quant/frontend/ast.h` - all AST node definitions.
- `include/quant/ir/ir.h` - all IR instruction definitions.
- `src/ir/opt.cpp` - target-independent IR optimizer (`-O0`..`-O3`): constant folding, constant/copy propagation through locals, CFG cleanup, compile-time evaluation of pure loops, dead code / dead store elimination, whole-program pruning. Temps are single-assignment; locals are mutable slots written only by `IRStoreLocal` (a local whose address is taken via `IRAddrOf` is never tracked).
- `include/quant/attributes/attributes.h` - attribute registry.
- `src/backend/aarch64_isel.cpp` (~950 lines) - AArch64 instruction selection and code generation.
- `src/backend/aarch_64.cpp` - AArch64 emitter (binary encoding for all instructions).

### IR

Register-based IR with labels. Key instructions: `IRLoadConst`, `IRBinary`, `IRCall`, `IRReturn`, `IRJump`, `IRBranch`, `IRGetField`, `IRSetField`, `IRCast`, `IRAlloca`, `IRRegionBegin`/`IRRegionAlloc`/`IRRegionEnd`, `IRLoadElement`, `IRStoreElement`.

### Struct return (sret)

Functions returning structs pass a hidden pointer as the first argument. The caller allocates stack space and passes it; the callee writes the result there and returns 0.

### Multi-target support

The compiler separates **build configuration** from **compilation target selection**:

- CMake (`QUANT_BACKENDS`, e.g. `-DQUANT_BACKENDS="x86_64-linux;aarch64-linux"`) decides which backends are enabled in the binary. Defaults: `x86_64-windows` on Windows hosts, `x86_64-linux;aarch64-linux;aarch64-zeropoint` elsewhere. The list is passed to C++ as the `QUANT_ENABLED_BACKENDS` compile definition and consumed by the target registry (`src/backend/targets.cpp`). Unknown backend names fail at CMake configure time.
- Disabled backends are not compiled at all: CMake derives `QUANT_HAS_X86_64` / `QUANT_HAS_AARCH64` / `QUANT_HAS_PE` / `QUANT_HAS_ELF` from `QUANT_BACKENDS` and excludes the corresponding sources (`isel.cpp`+`x86_64.cpp`, `aarch64_isel.cpp`+`aarch_64.cpp`, `pe_writer.cpp`, `elf_writer.cpp`). The registry intersects the enabled list with these flags, so a target is selectable only when its generator exists in the binary.
- `--target` resolves a CLI spelling through the registry (`include/quant/backend/targets.h`). Canonical names: `x86_64-linux`, `aarch64-linux`, `x86_64-windows`, `aarch64-zeropoint`. Legacy aliases preserved: `x86_64`/`x86-64` (host-relative), `arm64` -> aarch64-linux, `arm64-zeropoint`. An unknown spelling errors with the known list; a known-but-disabled target errors with the enabled list — never silently falls back. No `--target`: the CMake-time default (optional `-DQUANT_DEFAULT_TARGET=...`, validated against `QUANT_BACKENDS`), else the host-native backend if enabled; otherwise an explicit `--target` is required.

Targets share the same IR but differ in:

- **Calling convention**: x86-64 uses RDI/RSI/RDX/RCX/R8/R9; AArch64 uses X0-X7 with X8 for sret (AAPCS64).
- **Syscalls**: IR embeds x86-64 syscall numbers; the AArch64 Linux backend remaps them (e.g., `write=1` → `64`, `mmap=9` → `222`). The ZeroPoint backend passes numbers through unchanged.
- **Executable flavor**: chosen by `TargetOS`, not by the host — `Windows` → PE32+ (`pe::write`), everything else → ELF (`elf::write`) + external linker. Both writers are portable byte emitters.
- **Stack frame**: x86-64 grows down (RBP-based); AArch64 uses SUB/ADD SP with FP+16-based local addressing.
- **Registers**: x86-64 has 16 GPRs; AArch64 has 31 GPRs. Both use stack-based lowering for temps.
- **ISA encoding**: x86-64 is variable-length; AArch64 is fixed 32-bit with specific bitfield layouts.

### ZeroPoint target

ZeroPoint is an AArch64 ELF OS with its own minimal ABI ("Linux-like, different syscalls"):

- **Syscalls**: one `str buffer` argument in X0, number in X8, `SVC #0`. Numbers: write=0, read=1, open=10, exit=20. Everything except the buffer (length, descriptor, mode) is computed by the OS; `open` is read-only and returns nothing (the OS prints/stores the contents itself).
- **Output**: static-PIE (`ld.lld -pie --image-base=0x40000000`), ET_DYN, no interpreter; stock load address 0x40000000.
- **_start**: after `main` returns it parks the core (`WFE` + branch-to-self loop) because exit(20) is not implemented in the kernel yet.
- **Regions**: `region`/`alloc` are rejected at codegen time (no mmap/munmap in the ABI yet).
- **Heap**: the kernel has no malloc yet (MMU exists, allocation syscalls are TODO). Any `@syscall(N)` outside the ABI set is a compile-time error, so loading `std::heap`/`std::arena` fails cleanly instead of emitting undefined syscalls.
- **Format runtime**: `std::format` is not loaded on this target (`as str` casts fail at link time with undefined `qk_format_*`) until the kernel provides allocation.
- **Stdlib override**: `load "std::io"` resolves to `std/zp/io/io.qu` when the target is ZeroPoint (same pattern as the Windows `std/win/` override).

---

## 16. Compiler Invariants

1. **Struct fields are always 8 bytes.** This is hardcoded in both semantic analysis (`ir_gen.cpp:102`) and type size computation. A `bool` field takes 8 bytes.

2. **No implicit narrowing.** The semantic analyzer rejects any assignment where the source type cannot be widened to the target type.

3. **`bool` is not an integer.** No implicit conversion between `bool` and any integer type. `&&`/`||` require `bool` operands. `!` only works with `bool`. Using an integer where `bool` is expected is a compile error.

4. **Nullability is monotonically increasing.** Once a pointer variable is marked "maybe null", it can never lose that status. This prevents sound reference conversions.

5. **No implicit type inference.** Every variable must have an explicit type annotation. There is no `auto`, `let`, or `var` keyword.

6. **Function return type is mandatory.** Even for functions returning nothing, `void` must be specified.

7. **Immutable variables must be initialized.** Only `@init` or `mut` can bypass this check.

8. **Forward declarations are supported.** A function can be declared without a body, then defined later with the same signature.

9. **All struct fields occupy 8 bytes (qword)** regardless of their declared type. This is relevant for sizeof calculations and memory layout.

10. **`for` loop `continue` executes the step.** The `for` loop is desugared at parse time into a `WhileStmt` that keeps its step expression in `for_step`; IR generation places the continue label right before the step. Plain `while` loops still jump straight to the condition (tests/for_continue.qu).

---

## 17. Common Mistakes / Wrong Assumptions

### DO NOT:

- **Do not treat Quant as Rust.** There is no ownership system, no borrow checker, no lifetime annotations. References (`&T`) are simple pointers without lifetime constraints.

- **Do not treat `&T` like C++ references.** Quant references are nullable-safe pointers. They are auto-dereferenced for field access but can be reassigned. They can be taken from pointers (with nullability checks).

- **Do not assume `bool` is interchangeable with integers.** `if (x)` where `x` is `i32` is a compile error. Use `if (x != 0)`.

- **Do not assume implicit narrowing is allowed.** `i8 b = 300;` is an error. Use explicit `as` casts.

- **Do not assume GC or RAII.** Memory is managed manually via regions and heap. There is no automatic cleanup when variables go out of scope.

- **Do not use hex, octal, or binary number literals.** Only decimal literals are supported.

- **Do not use `#include`, `import`, `package`, or other module syntax from other languages.** Use `load "module::path";`.

- **Do not add `self.` prefix in method bodies.** Struct fields are accessed directly by name inside methods.

- **Do not assume operator overloading for structs.** `==`, `+`, etc. on struct values is not supported.

- **Do not use integer types as array sizes in declarations.** Arrays are declared as pointers with `alloc`.

- **Do not invent language features.** There are no traits, interfaces, closures, lambdas, pattern matching, destructuring, optional chaining, null coalescing, or comprehensions.

- **Do not use `new` or `delete`.** Use `alloc` inside regions or `std::heap::heap_malloc`/`heap_free`.

- **Do not assume `char` is a separate type.** `char` is an alias for `u8`.

- **Do not use string interpolation or template literals.** Use `std::format` functions.

- **Do not assume default parameter values.** All function arguments must be provided.

- **Do not use semicolons after function definitions or struct definitions inside braces.** The semicolon goes after the closing brace of the struct: `struct X { ... };`

---

## 18. Canonical Examples

### Basic program

```
load "std::io";

@entry i32 main() {
    mut i32 x = 0;
    for (mut i32 i = 0; i < 10; i++) {
        x = x + i;
    }
    std::io::println(x as str);
    return 0;
}
```

### Region-based allocation

```
load "std::io";

i32 main() {
    region r {
        *i32 arr = alloc(i32, 5);
        for (mut i32 i = 0; i < 5; i++) {
            arr[i] = i * 10;
        }
        std::io::println(arr[2] as str);  // 20
    }
    return 0;
}
```

### Struct with methods

```
struct Counter {
    mut i32 val;

    void bump() {
        val = val + 1;
    }

    i32 get() {
        return val;
    }
};

i32 main() {
    Counter c = Counter{0};
    c.bump();
    c.bump();
    return c.get();  // 2
}
```

### References

```
struct Point {
    i32 x;
    i32 y;
};

void bump(mut &Point p) {
    p.x = p.x + 1;
}

i32 main() {
    mut Point p = {10, 20};
    bump(&p);
    return p.x;  // 11
}
```

### Generics

```
T identity<T>(T x) {
    return x;
}

i32 main() {
    i32 v = identity<i32>(42);
    return v;
}
```

### Nullability trap

```
i32 maybe_ref_error() {
    *i32 p = nullptr;
    p = alloc(i32, 1);
    &r = p;   // ERROR: p is still "maybe null"
    return 0;
}
```

### Option type (from stdlib)

```
load "std::types";
using std::types;
load "std::io";

i32 main() {
    option<i32> r = option_some<i32>(42);
    if (r.is_some()) {
        std::io::println(r.unwrap() as str);
    }
    return 0;
}
```

### Enum and switch

```
enum Color {
    Red,
    Green,
    Blue,
};

i32 main() {
    mut i32 c = Red;
    switch (c) {
        case Red: { return 1; }
        case Green: { return 2; }
        case Blue: { return 3; }
        default: { return 0; }
    }
}
```

### Pointer indexing

```
i32 main() {
    region r {
        *i32 buf = alloc(i32, 3);
        buf[0] = 10;
        buf[1] = 20;
        buf[2] = 30;
        return buf[1];  // 20
    }
}
```

---

## Conflicts and Documentation Issues

### 1. Struct field size

Doc.md does not mention that all struct fields occupy 8 bytes regardless of their declared type. The implementation (`ir_gen.cpp:102`) always uses `8` for every field. This is a significant ABI/layout detail that the documentation omits.

### 2. `for` loop `continue` behavior

`continue` in a `for` loop executes the step expression before re-checking the condition (C semantics). The desugaring keeps the step in `WhileStmt::for_step`, and IR generation places the continue label before it (tests/for_continue.qu).

### 3. `switch` on `bool` type

Doc.md states "The switch expression must have type bool, char, or an integer type." The implementation (`semantic.cpp:1519-1522`) confirms this by checking `k >= TypeKind::Bool && k <= TypeKind::U64`.

### 4. Reference auto-dereferencing not documented as implicit

Doc.md shows reference field access working without explicit dereferencing, but does not explicitly state that `&T` auto-dereferences for ALL field access and indexing operations. The implementation (`semantic.cpp:566-568`, `semantic.cpp:2023-2025`) confirms this is a pervasive behavior.

### 5. Method implicit receiver calls

Doc.md does not document that methods can call sibling methods of the same struct without an explicit receiver. The implementation (`semantic.cpp:2558-2567`) confirms this implicit self-call behavior.

### 6. No `else if` as a separate construct

Doc.md shows `else if` syntax. The parser (`parser.cpp:586`) handles this by chaining `ElseIfStmt` nodes. It is not a separate statement type but part of the `if` AST node.

### 7. Bitcast (`as!`) allows pointer/reference conversions without size check

Doc.md says `as!` requires same-size types. The implementation (`semantic.cpp:2817-2818`) skips the size check for pointer and reference types. This is not documented.

### 8. Doc.md says "Number literals are decimal only"

Confirmed by the lexer (`lexer.cpp`). No hex (`0x`), octal (`0o`), or binary (`0b`) prefix support.

### 9. String to numeric `as` conversion

Doc.md shows `str s = 42 as str;` and `str t = 3.14 as str;`. The implementation (`semantic.cpp:2806-2808`) confirms numeric-to-string conversion via `as`. However, string-to-numeric conversion is **not** supported - there is no `as` from `str` to any numeric type.

### 10. Doc.md mentions `else if` spacing inconsistency

Doc.md shows `else if(x == 10)` without a space, which may confuse formatting. Both `else if (` and `else if(` work since the parser simply looks for the `if` token after `else`.

### 11. Implicit `out` variable for struct-returning functions

The semantic analyzer (`semantic.cpp:1432-1446`) declares an implicit `out` variable for struct-returning functions. This is an internal compiler mechanism and not part of the language specification, but it affects how struct return values are handled internally.

### 12. Doc.md says `void main()` in examples

Doc.md shows `void main()` in the `using` example. However, the `@entry` attribute or a function named `main` with return type `i32` is the standard convention. `void main()` is valid but unusual.


## Explicit compile-time evaluation (`#`)

`#` marks an expression for AST interpretation during semantic analysis; its
result replaces that expression with a typed literal. It is not preprocessing.
The scalar evaluator keeps each call's locals isolated and resolves function
bodies in their defining namespace. A void call may be used as a statement,
but a top-level `#` expression must produce a supported scalar value.
Integer arithmetic follows the backend's 64-bit slots; signed division overflow
and invalid float-to-integer conversion produce compile-time diagnostics.
The existing step (50,000,000) and call-depth (128) limits remain in force.
Regression commands and measured performance are documented in `benchmark.md`.
