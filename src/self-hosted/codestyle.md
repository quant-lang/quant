# Quant Code Style

This document describes how Quant code should look and be organized.  
It is a style guide, not a language specification.

## Basics

- Use tabs for indentation
- Keep indentation and whitespace consistent
- Explicit types on variables and function return values
- Use `mut` only when a value is reassigned
- Opening braces stay on the same line
- Functions and variables use `snake_case`
- Types use `PascalCase`
- Enum variants use `SCREAMING_SNAKE_CASE` in compiler code
- Keep code readable over following arbitrary formatting rules

```qu
i32 min(i32 a, i32 b) {
    if (a < b)
        return a;
    return b;
}
````

## Files

A source file normally starts with its module name and a short description:

```qu
// front::parser
//
// Parses tokens into the Quant AST.
```

Then come imports and the module declaration:

```qu
load "front::token";
using front::token;

module "front::parser";
```

Keep declarations in a sensible order:

1. imports and module
2. external declarations
3. enums and structs
4. low-level helpers
5. factories and list helpers
6. higher-level functions
7. entry point

Long files can use section headers:

```qu
/// Token helpers ///

/// AST factories ///

/// Parsing ///
```

Don't split a small file into twenty sections just because the heading syntax exists.

## Formatting

Use spaces around operators and after commas:

```qu
i32 result = a + b;
foo(a, b, c);
```

Keep simple control-flow statements compact:

```qu
if (token == TOKEN_EOF)
    return;

while (node != nullptr) {
    process(node);
    node = node.next;
}
```

Use braces when a body contains multiple statements or becomes easier to read with them.

```qu
if (valid) {
    parse();
    finish();
}
```

Don't force everything into one-line statements just to satisfy the style guide.

Keep functions separated by one blank line. Avoid unnecessary blank lines inside small blocks.

## Naming

Use names that describe what the value actually is.

```text
parse_*       parsing
make_*        constructing values
alloc_*       allocating nodes
*_push        adding to a list
check_*       checking without consuming
match_*       checking and consuming
ser_*         serialization
*_init        initialization
```

Short names are fine when the context is obvious:

```qu
*Parser p;
*Token t;
*ExprNode n;
mut u64 i;
```

Booleans should normally read like predicates:

```qu
bool is_mut;
bool has_body;
bool is_comptime;
```

Avoid unnecessary prefixes, abbreviations, and names that only make sense to the person who wrote them three hours ago.

## Variables

Types are always explicit.

```qu
i32 count = 0;
mut i32 pos = 0;
u64 length = text.len();
```

Variables are immutable by default. Add `mut` when they actually change.

Declare variables close to where they are first used:

```qu
Token t = next_token();

if (t.type == TOKEN_EOF)
    return;

mut i32 depth = 0;
```

Don't create a wall of declarations at the beginning of every function.

## Functions

Return types are always explicit, including `void`.

```qu
i32 add(i32 a, i32 b) {
    return a + b;
}

void reset(mut State s) {
    s.pos = 0;
}
```

Use early returns when they make the main path clearer:

```qu
if (node == nullptr)
    return nullptr;

if (!valid(node))
    return nullptr;

return parse_node(node);
```

Avoid unnecessary nesting just to produce a single exit point.

## Structs and enums

Keep structs simple and readable:

```qu
struct SourceLocation {
    str file;
    i32 line;
    i32 column;
    i32 length;
};
```

Enum variants in compiler internals use explicit domain prefixes:

```qu
enum TokenType {
    TOKEN_EOF,
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER
};
```

Don't add formatting tricks to make fields or enum values line up.

## Control flow

Use the simplest construct that expresses the logic.

```qu
switch (type) {
    case TOKEN_NUMBER:
        return parse_number();

    case TOKEN_STRING:
        return parse_string();

    default:
        return nullptr;
}
```

Grouped cases are fine:

```qu
switch (type) {
    case TOKEN_PLUS:
    case TOKEN_MINUS:
        return parse_operator();

    default:
        return nullptr;
}
```

Use `while` for condition-based iteration and the normal counting form of `for` when an index is needed.

```qu
for (mut u32 i = 0; i < items.len(); i++) {
    process(items[i]);
}
```

Don't introduce clever control-flow patterns just because the language allows them.

## Pointers and memory

Be explicit about ownership and mutation.

```qu
void update(mut *Node node);
void inspect(*Node node);
```

Use `nullptr` for pointers.

Pointer casts use `as!` when the operation is a bit reinterpretation:

```qu
mut *u8 ptr = value as! *u8;
```

Keep pointer-heavy code explicit. Quant is a systems language. Hiding what memory is doing defeats the point.

## Comments

Comments should explain **why**, not narrate the code.

Bad:

```qu
i++;
// Increment i
```

Good:

```qu
// Keep the original index because the serializer writes the count first
i++;
```

Use normal comments for local explanations:

```qu
// Skip whitespace before parsing the next token.
```

Use section comments for larger groups:

```qu
/// Serialization ///
```

Functions that are genuinely non-obvious can have a short description:

```qu
// escape_char - converts an escaped character into its byte value
option<u8> escape_char(char c) {
    ...
}
```

Don't comment every function merely because a checklist says every function needs a comment. If the function is `add(a, b)`, its purpose is already obvious.

## Error handling

Parser errors should point to the relevant token and then recover or return.

```qu
error_at(p, p.current, "Expected type");
return nullptr;
```

Keep error messages short and consistent.

```text
Expected ')'
Expected expression
Expected type
```

Don't continue processing an invalid node unless the parser has explicitly recovered.

## Self-hosted compiler

The self-hosted compiler should follow the structure of the existing compiler where that structure is useful.

Keep AST nodes, factories, serializers, and parser helpers organized consistently so the C++ and Quant implementations are easy to compare.

Use the same naming families and data structures where possible.

Do not copy C++ structure blindly when Quant has a simpler way to express the same thing.

## General rule

The style guide exists to make Quant code easier to read, review, and maintain.

When a rule makes the code harder to read, prefer readability.

Consistency matters, but **consistency is not the goal. Readable code is.**
