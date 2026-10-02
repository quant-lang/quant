# Quant Code Style

Style rules for Quant (`.qu`) code. This document covers code style, not language semantics.

## Naming

* Functions and variables use `snake_case`.
* Types use `PascalCase`. (stdlib types may be called with `snake_case`)
* Compiler enum variants use `SCREAMING_SNAKE_CASE`.
* Boolean names should read as predicates: `is_mut`, `has_body`, `is_comptime`.
* Use common short names when context is obvious: `p`, `t`, `n`, `i` 

 For example:
```qu
Token t = next_token();

if (t.type == TOKEN_EOF)
    return;
```


Common naming patterns:

```text
parse_*     parsing
make_*      constructing values
alloc_*     allocation
*_push      append to a list
check_*     check without consuming
match_*     check and consume
ser_*       serialization
*_init      initialization
```

## Files

Source files normally begin with a module header:

```qu
// front::parser
//
// Parses tokens into the Quant AST.
```

Then imports and the module declaration:

```qu
load "front::token";
using front::token;
module "front::parser";
```

Recommended declaration order:

1. imports and module
2. external declarations
3. enums and structs
4. low-level helpers
5. factories and list helpers
6. higher-level functions
7. entry point

Large files may use section headers:

```qu
/// Token helpers ///
/// AST factories ///
/// Parsing ///
```

## Formatting

* Opening braces stay on the same line.
* Use spaces around operators and after commas.
* Separate functions with one blank line.
* Avoid unnecessary blank lines inside small blocks.
* Omit braces for simple single-statement control flow.

```qu
if (a < b)
    return a;

while (node != nullptr) {
    process(node);
    node = node.next;
}
```

Use braces for multi-statement bodies:

```qu
if (valid) {
    parse();
    finish();
}
```

## Structs and Enums

Keep fields and enum values one per line.

```qu
struct SourceLocation {
    str file;
    i32 line;
    i32 column;
    i32 length;
};

enum TokenType {
    TOKEN_EOF,
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER
};
```

Do not align fields or enum values with extra whitespace.

## Control Flow

Use:

* `if` for conditional execution
* `switch` for multiple discrete cases
* `while` for condition-based iteration
* `for` for indexed iteration

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

Grouped cases are valid:

```qu
switch (type) {
    case TOKEN_PLUS:
    case TOKEN_MINUS:
        return parse_operator();

    default:
        return nullptr;
}
```

## Pointers and Memory

Make pointer mutation explicit in function signatures.

```qu
void update(mut *Node node);
void inspect(*Node node);
```

Use `nullptr` for null pointers.

Use `as!` for pointer casts that reinterpret the underlying bits:

```qu
mut *u8 ptr = value as! *u8;
```

## Comments

Use comments for non-obvious decisions, invariants, and implementation details.

```qu
// Keep the original index because the serializer writes the count first
i++;
```

Use section comments for larger groups:

```qu
/// Serialization ///
```

Do not comment self-explanatory code

```qu
i++;
```

## Error Handling

Keep error messages short and consistent:

```text
Expected ')'
Expected expression
Expected type
```

## Self-Hosted Compiler

Keep the self-hosted compiler structurally consistent with the existing compiler where useful.

* Keep AST nodes, factories, serializers, and parser helpers organized consistently.
* Reuse naming families and data structures where practical.
* Do not mirror C++ structure when Quant provides a simpler implementation.
