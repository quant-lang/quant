# Quant Language

## Types

| Type | Size | Description        |
|------|------|--------------------|
| void | 0    | no value           |
| bool | 1    | boolean            |
| i8   | 1    | signed byte        |
| i16  | 2    | signed short       |
| i32  | 4    | signed int         |
| i64  | 8    | signed long        |
| u8   | 1    | unsigned byte      |
| u16  | 2    | unsigned short     |
| u32  | 4    | unsigned int       |
| u64  | 8    | unsigned long      |
| f32  | 4    | float              |
| f64  | 8    | double             |
| str  | 8    | string (pointer)   |
| *T     | 8 | pointer to T       |
| &T     | 8 | reference to T     |
| nullptr | 8 | "no pointer" value |

Integer literals are i32 by default. Float literals are f64 by default. Boolean literals `true` and `false` are `bool`.

## `nullptr`

`nullptr` means "there is no value to point to". You can only store it in a pointer variable:

```qu
*i32 p = nullptr;
```

You cannot use `nullptr` for references or other values.

Once a pointer may hold `nullptr`, the compiler treats it as "maybe null" forever, even if you put a real value into it later:

```qu
*i32 p = nullptr;

p = alloc(i32, 1); // p is still "maybe null"
```

This rule keeps things safe: a pointer that may be null cannot be turned into a reference. If you try, you get a compile error.

---

## Variables

```
i32 a = 10;            // immutable, must be initialized
mut str b = "hello";   // mutable
f64 c = 3.14;
f64 d = 1.5e2;         // scientific notation
```

Variables must have an explicit type annotation.

---

## Functions

```
i32 add(i32 x, i32 y) {
    return x + y;
}

void greet(str name) {
    std::io::print("hello ");
    std::io::print(name);
}

i32 main() {
    return 0;
}
```

- Return type is always required (use `void` if the function returns nothing).
- Parameters can be mutable: `void foo(mut i32 x)`.
- Function must have at least one return statement if return type is not void.

---

## Control Flow

### if / else if / else

```
if (x < 10) {
    std::io::print("small");
}
else if(x == 10){
  std::io::print("medium");
}
else {
    std::io::print("big");
}
```

The `if` and `else if` conditions must be of type `bool` (a compile-time error otherwise).

### while

The `while` condition must be of type `bool`.

```
while (x > 0) {
    x--;
}
```

### for

`for` is a wrapper over `while`:

```
for (init; cond; step) {
    // body
}
```

is rewritten at parse time into:

```
{
    init;
    while (cond) {
        // body
        step;
    }
}
```

Examples:

```
for (mut i32 i = 0; i < 10; i++) {
    std::io::print(i as str);
}

// reuse an existing variable
for (i = 0; i < 10; i++) { ... }

// omitted clauses
for (;;) { ... }                 // infinite loop, use break
for (; i < 10; i++) { ... } // no init
```

Rules:

- `init` is either a variable declaration or an expression, both end with `;`. Since the loop counter is mutated by `step`, declare it with `mut` (unless the `step` only reads it).
- `cond` may be omitted (treated as `true`).
- `step` may be omitted.
- `continue` in a `for` loop runs the `step` before re-checking the condition (same as C).

### break / continue

`break` immediately exits the nearest enclosing `while` loop or `switch`.

`continue` skips the rest of the current loop iteration and starts the next one:

```qu
while (i < 10) {
    i = i + 1;

    if (i == 5) {
        break;      // stop the loop
    }

    if (i == 3) {
        continue;   // skip sum += i
    }

    sum = sum + i;
}
```

Notes:

- `break` can only be used inside a `while` loop or `switch`.
- `continue` can only be used inside a `while` loop.
- If a `switch` is inside a `while`, `break` leaves only the `switch`, while `continue` starts the next iteration of the enclosing loop.

### switch / case / default

`switch` selects one branch based on the value of an expression:

```qu
switch (x) {
    case 1: {
        std::io::print("one");
    }

    case 2: {
        std::io::print("two");
    }

    default: {
        std::io::print("other");
    }
}
```

Multiple `case` labels may share the same body, just like in C:

```qu
switch (x) {
    case 1:
    case 2: {
        std::io::print("one or two");
    }

    default: {
        std::io::print("other");
    }
}
```

Rules:

- The `switch` expression must have type `bool`, `char`, or an integer type.
- `case` values must be compile-time constants (literals, enum variants, or `const` variables).
- Duplicate `case` values are a compile-time error.
- `default` is optional. If no case matches and there is no `default`, execution simply continues after the `switch`.
- A `switch` is considered terminating only if every possible branch (`case` and `default`) terminates (for example, by `return`).

### References

References (`&T`) are another name for an existing variable. They take 8 bytes, like pointers.

Field access and indexing through a reference are automatically dereferenced:

```
struct Point { i32 x; i32 y; };

i32 main() {
    Point p = {10, 20};
    &Point r = &p;
    return r.x + r.y;   // field access through a reference
}
```

References can be reassigned to point to different variables (the reference itself must be `mut`):

```
struct Point { i32 x; i32 y; };

i32 main() {
    mut Point a = {10, 20};
    mut Point b = {1, 2};
    mut &Point r = &a;
    r = &b;             // reassign reference to point to b
    return r.x;         // 1
}
```

References can be passed to functions that accept reference parameters. To modify the referenced value, the parameter must be `mut`:

```
struct Point { i32 x; i32 y; };

void bump(mut &Point p) {
    p.x = p.x + 1;      // modifies the original variable
}

i32 main() {
    mut Point p = {10, 20};
    bump(&p);
    return p.x;         // 11
}
```

Note: a reference cannot be taken to a builtin value type (`i32`, `bool`, `f64`, ...).

---

## Operators

### Arithmetic
```
+   -   *   /
```

### Comparison (result is `bool`)
```
==   !=   <   <=   >   >=
```

### Bitwise
```
&    (bitwise AND)
|    (bitwise OR)
```

### Logical (result is `bool`)
```
&&   (and - both operands true -> true)
||   (or  - at least one true -> true)
```

### Compound Assignment
```
+=   -=   *=   /=   &=   |=
```
Each desugars to `x = x op y` at parse time.

### Precedence (lowest -> highest)
```
=   +=   -=   *=   /=   &=   |=
||
&&
|
&
==   !=
<   <=   >   >=
+   -
*   /
```

---

## Casts

### Value conversion (`as`)

Converts between numeric types. Also converts numbers to string.

```
i64 a = 42 as i64;       // i32 -> i64
i8  b = 1000 as i8;      // i32 -> i8  (truncation)
f64 c = 10 as f64;       // i32 -> f64
i32 d = 3.9 as i32;      // f64 -> i32 (truncates to 3)
str s = 42 as str;       // number -> string
str t = 3.14 as str;     // float -> string
```

### Implicit conversions (widening only)

Most numeric conversions happen automatically as long as they are guaranteed
to preserve the value. If a conversion could lose information, the compiler
rejects it and you must write an explicit as cast.

```
i8  a = 42;             // the literal fits in i8
u8  b = 200;            // the literal fits in u8
i16 c = 30000;          // the literal fits in i16
f32 d = 2.5;            // the literal becomes f32
i32 e = c;              // i16 -> i32 is a safe widening conversion
f64 f = a;              // i8 -> f64 is also safe
```
```
i8 g = 300;            // ERROR: 300 does not fit in i8
i8 h = 200;            // ERROR: literal out of range for i8
u8 i = -1;             // ERROR: negative values cannot be assigned to u8
```

The same rules apply when passing function arguments, returning values, or
initializing structs.

Integer literals in binary expressions adapt to the other operand whenever
possible. If both operands have different numeric types, they are promoted to
a common type before the operation.

```
i32 a = 5;
i64 b = 10;
i64 s = a + b;          // a is promoted to i64

f64 d = a * 1.5;        // a is promoted to f64

u8  r = 250 + 5;        // both literals fit in u8, result is u8 (255)
```

Comparison operators (==, !=, <, <=, >, >=) also work with mixed
numeric types. Both operands are converted to a common type before the
comparison, and the result is always bool.

Logical operators are stricter: && and || require bool operands, and the
unary ! operator only works with bool.

### Bit reinterpretation (`as!`)

Reinterprets the bytes of a value as a different type. Source and target must have the same size.

```
u32 x = -1 as i32 as! u32;   // reinterpret i32 bytes as u32
```

---

## Structs

```
struct Point {
    i32 x;
    i32 y;
};

i32 main() {
    Point p = { 10, 20 };
    return p.x + p.y;
}
```

Fields can be mutable:
```
struct Counter {
    mut i32 val;
};
```
Methods are also supported
```
struct Point {
    i32 x;
    i32 y;

    void set(i32 _x, i32 _y) {
        x = _x;
        y = _y;
    }
};
```
And generic methods
```
struct Box<T> {
    T value;

    void set(T v) {
        value = v;
    }

    T get() {
        return value;
    }
};
```

---

## Enums

C-style enums with automatically assigned `i32` values starting from 0.
```
enum Color {
    Red,    // 0
    Green,  // 1
    Blue,   // 2
};
```

Variants can be accessed directly by name or qualified:

```
i32 x = Red;          // 0
i32 y = Color::Green; // 1
```

Enums support attributes like `@public` and `@private`:

```
@public enum Status {
    Ok,
    Error,
};
```

---

## Generics

Structs can have type parameters with `<>`:

```
struct Box<T> {
    T value;
};
```

When you use a generic struct, pass the type argument in `<>`:

```
@init mut Box<i32> b;
b.value = 42;
```

Multiple type parameters are also supported:

```
struct Pair<A, B> {
    A first;
    B second;
};
```

Generics are compiled lazily: the concrete struct (like `Box$i32`) is created only when you first access a field.

### Generic Functions

Functions can also have type parameters:

```
T identity<T>(T x) {
    return x;
}
```

Pass the type argument when calling:

```
identity<i32>(42);
```

Multiple type parameters work too:

```
A swap<A, B>(A a, B b) {
    return a;
}
```

Generic functions are compiled separately for each type you actually use. For example, `identity<i32>` and `identity<bool>` become two independent functions.

---

## Modules

Use `load` to import a module:

```
load "std::io";
```

Functions in other files are accessed via namespace:

```
std::io::print("hello");
std::io::exit(0);
```

`extern` declares a function implemented outside of Quant (system calls, native runtime, or another module):

```
extern void print(str text);
```

Functions marked with `@syscall(N)` are lowered to a raw syscall with number N (remapped per target; on ZeroPoint the number is the ABI number):

```
@syscall(1) extern i64 sys_write(i64 fd, str buf, i64 len);
```

The standard library (`std::io`, `std::heap`, `std::arena`, `std::string`, ...) is written in pure Quant: on Linux on top of syscalls, on Windows on top of `@import` (WinAPI), on ZeroPoint (`--target aarch64-zeropoint`) on top of single-buffer syscalls (`std/zp/io/io.qu`).

### `using`

Imports all symbols from a namespace into the current scope:

```
load "std::io";
using std::io;

void main() {
    print("hello\n");   // instead of std::io::print
}
```

Symbols are imported by value (copied) - visibility (`@public`/`@private`) is still enforced at the use-site based on the original module

---

## Attributes

Attributes annotate declarations with extra semantics. Syntax: `@name` before a declaration.

If the attribute takes arguments: `@name(expr1, expr2)`.

Supported attributes:

| Attribute  | Targets                              | Args | Description                                          |
|------------|--------------------------------------|------|------------------------------------------------------|
| `@entry`   | function                             | 0    | Mark function as program entry point                 |
| `@init`    | variable                             | 0    | Suppress uninitialized variable check                |
| `@guard`   | variable / field                      | 1    | Compile-time check: validate condition on use        |
| `@public`  | function / variable / field / struct | 0    | Make symbol visible outside the module               |
| `@private` | function / variable / field / struct | 0    | Hide symbol from other modules                       |
| `@hide`    | any top-level declaration            | 0    | Make all following declarations private by default   |
| `@unhide`  | any top-level declaration            | 0    | Reset @hide — following declarations are public      |
| `@syscall` | function (extern only)               | 1    | Lower function to raw syscall `N` (remapped per target) |
| `@export`  | function                             | 1    | Use `name` as the function symbol in the object file |
| `@import`  | function (extern only)               | 1-2  | Import a function from a Windows DLL                 |

### `@entry`

The function with `@entry` is the program entry point (replaces `main` by name).

```
@entry i32 start() {
    return 0;
}
```

If no `@entry` is found, the linker falls back to a function named `main`.

### `@init`

Declares a variable without an initializer, promising the compiler it will be initialized before use.

```
@init i32 x;
foo(x);                  // no "uninitialized variable" error
```

Without `@init`, an immutable variable without a value is a compile error.

### `@guard`

Validates a condition at compile time when the value is used (passed to a function or when a struct field is accessed). If the condition evaluates to `false`, the compiler reports a `guard failed` error.

The guard condition must be a compile-time constant expression (literals, const variables, and comparisons between them).

```
i32 count = 1;
@guard(count > 0) mut i32 value;

value = 10;               // ok (writes are not guarded)

void work(i32 x) {}
work(value);              // passes: count > 0 is true at compile time
```

If the condition is false, compilation fails:

```
i32 count = 0;
@guard(count > 0) mut i32 value;

void work(i32 x) {}
work(value);              // error: guard failed for 'value' in call to 'work'
```

Guard also works on struct fields:

```
struct X {
    @guard(true) i32 data;
};
```

### `@private`

Restricts access to the declaring module. Other modules cannot call or reference the symbol. Works on functions, variables, struct fields, and struct methods.

```
@private i32 helper() {
    return 42;
}

struct Config {
    @private i32 secret;
    i32 value;

    @private void internal() { ... }
};

i32 public_fn() {
    return helper();     // ok - same module
}
```

Access from another module produces: `Cannot access private symbol` or `Cannot access private field`.

### `@public`

Explicitly marks a symbol as accessible from other modules. Works on functions, variables, struct fields, and struct methods. Useful inside `@hide` blocks.

### `@hide`

Makes all following declarations private by default. Works as a switch — stays active until `@unhide`.

```
@hide i32 internal() { return 1; }
i32 also_internal()  { return 2; }   // private
@public i32 api()    { return 3; }   // public (explicit override)
i32 still_private()  { return 4; }   // private
@unhide i32 open()   { return 5; }   // public again
```

Works on struct fields and methods too:

```
@hide struct Config {
    i32 a;        // private
    i32 b;        // private
    @public i32 c; // public (explicit override)
};
```

### `@unhide`

Resets `@hide`. Following declarations are public by default.

```
@hide i32 a = 1;     // private
@unhide i32 b = 2;   // public
i32 c = 3;           // public
```

### `@syscall`

Declares an `extern` function implemented as a raw syscall. The argument is the syscall number. (Windows uses `@import` instead.)

```
@syscall(1) extern i64 sys_write(i64 fd, str buf, i64 len);
@syscall(60) extern void sys_exit(i64 code);
```

The generated wrapper follows the System V calling convention: arguments arrive in
`rdi, rsi, rdx, rcx, r8, r9` (up to 6), then `rcx` is moved to `r10`, the syscall
number is loaded into `rax`, and `syscall` is executed. The result is returned in `rax`.

On AArch64, source numbers follow the x86-64 convention and are remapped to Linux AArch64 at codegen time.
Arguments use `x0-x5`, the syscall number goes into `x8`, and `svc #0` is executed.

On the ZeroPoint target the number is used as-is (ZeroPoint ABI: write=0, read=1,
open=10, exit=20). ZeroPoint syscalls take a single `str buffer` argument in `x0`;
everything else (length, descriptor, mode) is computed by the OS.

```
// ZeroPoint ABI (std::zp::io / std/zp/io/io.qu)
@syscall(0)  extern void sys_print(str buffer);
@syscall(1)  extern void sys_read(str buffer);
@syscall(10) extern void sys_open(str buffer);   // read-only
```

```
extern void print(str text);   // error: @syscall requires extern
@syscall("1") extern i64 bad(); // error: argument must be an integer literal
```

### `@export`

Assigns a fixed symbol name to a function in the generated object file (instead of the
compiler-generated internal name `fn_<id>__<name>`). Useful for calling Quant functions
from native code or for stable symbols during debugging.

```
@export("my_func") i32 do_work() {
    return 42;
}
```

### `@import`

Declares an `extern` function implemented by a Windows DLL. The call is emitted
through the PE import table (IAT) by the native backend. Windows only.

The first argument is the DLL name. An optional second argument is the exported
symbol name; when omitted, the Quant function name is used as the export name.

```
@import("kernel32.dll", "ExitProcess") extern void sys_exit_process(u32 uExitCode);
@import("kernel32.dll") extern u32 GetTickCount();   // imports GetTickCount
```

On Linux, use `@syscall` instead.

```
@import extern i64 bad();          // error: @import requires a DLL name
extern i64 also_bad();             // error: @import requires extern
```

---

## Regions & Pointers. Arrays
Quant has region memory system. Pointers can only be declared in ```region{}```. If a region dies, all pointers are destroyed.
```
region r {
    *void p = alloc(32);
}
```
Arrays are also supported and can (only) be declared in region:
```
region r {
    *i32 p = alloc(i32, 10);
    p[0] = 42;
    p[1] = p[0] + 1;
    std::io::print(p[0] as str);
    std::io::print_char(32 as i8);
    std::io::print(p[1] as str);
    std::io::print_char(10 as i8);
}
```
```alloc(T, count);``` — typed allocation, returns `*T` where count is number of elements.
```alloc(size);``` — untyped allocation, returns `*void` where size is in bytes.

Nested regions are also supported:
```
region outer {
    *i32 a = alloc(i32, 1);
    region inner {
        *i32 b = alloc(i32, 1);
    }
}
```

---

## Comments

```
// line comment

/*
   block comment
*/
```

---

## `sizeof`

Compile-time type size query:

```
u64 sz = sizeof(i32);   // 4
u64 sz = sizeof(i64);   // 8
u64 sz = sizeof(u8);    // 1
u64 sz = sizeof(*T);    // 8  (pointer)
u64 sz = sizeof(&T);    // 8  (reference)
```

In generic functions, `sizeof(T)` substitutes the concrete type at compile time:

```
void foo<T>() {
   u64 x = sizeof(T);
}

void main() {
    foo<i64>();    // x = 8
    foo<i32>();    // x = 4
}
```

---

## Compile-Time Evaluation (`#`)

The `#` operator forces an expression to be evaluated at compile time. The result is replaced with a constant value, regardless of the optimization level

```qu
mut i32 a = #(2 + 3 * 4);   // 14, evaluated at compile time
mut i64 b = #factorial(10); // function call evaluated at compile time
mut bool c = #(10 > 3);     // true
```

`#` is explicit: unlike optimizer passes, it always performs the evaluation, even at `-O0`

### What can be evaluated

* Integer, float and bool literals, arithmetic and comparison operators.
* Logical operators with short-circuit `&&` / `||`.
* `sizeof` and numeric casts.
* Local variables with compile-time known values.
* Enum variants.
* Calls to defined, non-`extern` functions.
* `if` / `else`, `while` / `for`, `switch`, `break` and `continue`.
* Generic functions with concrete type arguments.

For example:

```qu
i32 factorial(i32 n) {
    mut i32 result = 1;

    while (n > 1) {
        result *= n;
        n -= 1;
    }

    return result;
}

i32 value = #factorial(10); // 3628800
```

### Restrictions

The compile-time evaluator currently does not support:

* Strings.
* Structs and arrays.
* Pointers and references.
* Field or index access.
* `extern` functions or syscalls.
* Runtime I/O.

If an expression cannot be evaluated at compile time, compilation fails:

```qu
mut i32 a = #(1 / 0);       // error: division by zero
mut i32 b = #(read_input()); // error: cannot evaluate at compile time
```

Evaluation has a step budget to prevent non-terminating compile-time code. If the budget is exceeded, the compiler reports:

```text
cannot evaluate expression at compile time
```

### Generics

`#` inside a generic function is evaluated when the generic is instantiated:

```qu
i32 size<T>() {
    return #(sizeof(T));
}

i32 a = size<i32>(); // 4
i32 b = size<i64>(); // 8
```

### `#` vs Optimization

`#` is a language feature, while optimization is optional compiler behavior.

```qu
i32 a = #(10 * 20); // guaranteed to be computed at compile time
i32 b = 10 * 20;   // may be folded by the optimizer
```

This means `#` can be used when compile-time evaluation is part of the program's intended behavior, not just an optimization opportunity.

### Casting the Result

`as` is postfix and binds tighter than `#`.

```qu
i32 value = #(2 + 2);
```

The result is computed during compilation. String conversion is currently not supported by the compile-time evaluator.

---

## How to Build and Run

```
qu file.qu -o output          # compile to native binary (ELF/PE32+)
qu file.qu --target aarch64   # cross-compile for AArch64 (ELF)
qu file.qu --target aarch64-zeropoint   # ZeroPoint OS (AArch64, static-PIE ELF)
qu file.qu --emit-ir          # print intermediate representation
qu file.qu --emit-asm         # print generated assembly
qu file.qu --time             # print compilation time
qu file.qu --no-compile       # semantic analysis only

qu file.qu -O2 -o output      # optimization level 0-3 (default: -O2)
```

### Optimization levels

Quant performs optimization directly on its own IR, between IR generation and the native backend. This means the same optimizer is shared by every supported target: x86-64, AArch64, Windows and ZeroPoint.

The goal is simple: **make the generated program smaller and faster without changing what it does.**

Optimization levels build on each other:

| Level | What happens                                                                                                                                             |
| ----- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `-O0` | No optimization. The IR is passed to the backend almost as-is. Useful for debugging the compiler and inspecting raw IR.                                  |
| `-O1` | Cleans up the local mess: constant folding, algebraic simplification, constant/copy propagation, control-flow cleanup, dead temporaries and dead stores. |
| `-O2` | Everything from `-O1`, plus compile-time evaluation of pure loops and whole-program pruning of unused functions, strings and globals.                    |
| `-O3` | Same optimizations as `-O2`, but with an 8× larger budget for compile-time loop evaluation.                                                              |

For example, code that looks like this:

```qu
i32 x = 10 * 20;
i32 y = x + 5;
```

doesn't need to survive into the final machine code as a sequence of calculations. At `-O1`, the optimizer can reduce it to the equivalent of:

```qu
i32 y = 205;
```

Quant can also evaluate **pure loops at compile time**. A loop whose body only works with local variables and integer arithmetic can disappear entirely from runtime:

```qu
i32 sum = 0;

for (mut i32 i = 0; i < 100; i++) {
    sum += i;
}
```

At `-O2`, the compiler can calculate the result while compiling the program instead of making the generated executable perform the loop every time it runs. Nested loops, `if`/`switch`, `break` and `continue` are supported as long as the evaluated code remains within the allowed compile-time rules.

`-O3` does not introduce a completely different optimizer. It simply gives compile-time evaluation a much larger budget, allowing longer calculations to be folded before runtime.

Quant also removes things that the program never actually uses. At `-O2`, unused functions, string literals and globals are pruned from the IR. This is especially useful for the standard library, where a program may depend on a module without needing every symbol it contains.

Optimization is deliberately kept in the IR rather than being tied to a particular backend. The pipeline is therefore:

```text
Source
  ↓
Lexer → Parser → AST → Semantic
  ↓
IR generation
  ↓
IR optimization (-O0..-O3)
  ↓
Native backend
  ↓
ELF / PE32+
```

For debugging or experimentation, individual optimization passes can be disabled with `QUANT_SKIP`:

```text
QUANT_SKIP=fold,branch,locals,cfg,loop,dead,dse,prune
```

The available passes are:

* `fold` - constant folding and algebraic simplification
* `branch` - branch/jump simplification
* `locals` - constant and copy propagation through locals
* `cfg` - control-flow cleanup and unused-label removal
* `loop` - compile-time evaluation of pure loops
* `dead` - dead temporary elimination
* `dse` - dead-store elimination
* `prune` - unused function, string and global removal

Quant includes a small benchmark tool for observing what the optimizer actually does:

```text
build/bin/quant_bench file.qu
```
Example:
```md
### tests/bench_loop_fold.qu

Front end (lexer, parser, semantic, IR gen): 1.339 ms
Optimizer time is the minimum of 5 runs.

| level | optimize, ms | IR insts    | insts in 'main' | functions | loops folded | branches folded |
|-------|--------------|-------------|-----------------|-----------|--------------|-----------------|
| -O0   | 0.000        | 1442 (100%) | 150             | 38 / 38   | 0            | 0               |
| -O1   | 0.870        | 1107 (77%)  | 134             | 38 / 38   | 0            | 1               |
| -O2   | 10.0         | 636 (44%)   | 21              | 25 / 38   | 5            | 1               |
| -O3   | 9.997        | 636 (44%)   | 21              | 25 / 38   | 5            | 1               |
```

It reports optimizer time and resulting IR size for each optimization level.

Correctness is checked separately: `scripts/check_opt.py` runs the test suite against every optimization level and verifies that the observable program output remains identical to `-O0`.

The native backend generates machine code directly: x86-64 (ELF/PE32+) and AArch64 (ELF). No external assembler is required. Use `--target aarch64` (or `--target arm64`) to cross-compile for AArch64. Cross-linking uses `ld.lld` for AArch64 targets.

### Backend selection (CMake) vs target selection (--target)

The set of backends compiled into the compiler binary is fixed at CMake configure time
with `QUANT_BACKENDS`; `--target` then selects one of the enabled targets per compilation:

```
cmake -B build -DQUANT_BACKENDS="x86_64-linux;aarch64-linux;x86_64-windows;aarch64-zeropoint"
```

- Defaults: `x86_64-windows` when building on Windows, otherwise `x86_64-linux;aarch64-linux;aarch64-zeropoint`.
- Only the enabled backends are compiled into the binary: an x86_64-only build contains no AArch64 code generator and no PE writer, a Windows-only build contains no ELF writer, and so on.
- Canonical target names: `x86_64-linux`, `aarch64-linux`, `x86_64-windows`, `aarch64-zeropoint`. Short aliases (`x86_64`, `arm64`, ...) are still accepted.
- An unknown target name fails with the list of known names; a known name that was not enabled in this build fails with the list of enabled backends and a reconfigure hint. There is no fallback to another backend.
- Without `--target`, the compiler uses its host's native backend (if enabled). For cross-compilation-oriented builds the default can be changed at CMake time: `-DQUANT_DEFAULT_TARGET=aarch64-zeropoint` (must be one of `QUANT_BACKENDS`; validated at configure time).

ZeroPoint (`--target aarch64-zeropoint`) produces a position-independent static-PIE executable (`ld.lld -pie --image-base=0x40000000`): ET_DYN, no interpreter, stock load address 0x40000000. Syscalls follow the ZeroPoint ABI — a single `str buffer` in X0, number in X8, `SVC #0`; the OS computes everything else. After `main` returns the program parks on `WFE` (exit is not implemented in the kernel yet). The kernel has no malloc yet (MMU only), so `region`/`alloc`, `std::heap`, `std::arena` and the format runtime (`as str`) are unavailable; syscalls outside the ABI set are rejected at compile time.

Requires: CMake 3.20+, C++20 compiler.

Per-platform builds live in dedicated toolchain files under `toolchains/`
(`linux-x86_64`, `linux-aarch64`, `windows-x86_64` via MinGW-w64).
`scripts/build_all.py` configures and builds each of them
(Ninja, Release, `-j$(nproc)`) into `build-<name>/bin/qu`; cross toolchains that
are not installed are skipped with a notice. The ZeroPoint compiler build uses
the same AArch64 Linux toolchain as the `aarch64` build (per the ZeroPoint
author's request).

---

## Notes

- String literals support escape sequences: `\n`, `\t`, `\r`, `\\`, `\"`, `\'`, `\0`.
- Character literals use single quotes: `'a'`, `'\n'`, etc. `char` is an alias for `u8`.
- Number literals are decimal only (no hex, octal, or binary prefixes).
