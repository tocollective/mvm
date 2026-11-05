<!--
Copyright 2026 tocollective contributors
SPDX-License-Identifier: Apache-2.0

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# MIR — Textual Intermediate Representation

[Format Overview](README.md) · [Binary MO](mo.md) · [mvm Container](mvm.md)

The current version is MIR 4. The assembler is implemented in `../mvm-c/source/mo.c`; MIR 2
and MIR 3 are accepted for the previous subsets of types and instructions.

## 1. Purpose

MIR is an architecture-independent textual representation of a single M module.
It resides between the language frontend and the binary object:

```text
M → type analysis and layout → MIR → encoding and relocation → MO
```

MIR is not a general-purpose programming language and is not executed directly by the mvm. It already supports M scopes, calculates struct layouts, selects opcodes, defines global initializers, and determines the maximum stack depth. The MIR compiler does not perform optimizations that change the observed behavior.

One `.mir` file contains exactly one module. The module name is logical and matches the name used in inter-module imports within MO/mvm.

## 2. Encoding and Lexicon

- The file is UTF-8 without BOM. It accepts both LF and CRLF; the canonical generator writes LF.
- The first declaration must be `mir 4`, and the second—`module "..."`.
- Comments: `//` to the end of the line and nested `/* ... */`, as in M.
- Spaces and newline characters separate tokens. A semicolon after a declaration or instruction is allowed, but the canonical generator does not write it.
- Keywords are case-sensitive; instruction names are written in all caps.
- Local function name or global value: `@name`; type: `$name`;
  labels: `label:`. The `name` must conform to `[A-Za-z_][A-Za-z0-9_]*`.
- Local names are unique within their scope. Scopes for functions, global variables, types, imports, and labels are distinct; a function name and an imported function cannot be used.
- The external name in `export` is a string literal, and therefore is not subject to the rules of a local identifier.

Strings are double-quoted and encode UTF-8. Supported escapes are `\"`, `\\`, `\n`, `\r`, `\t`, `\0`, `\xNN`, `\uNNNN`, and `\UNNNNNNNN`.
`\xNN` specifies one byte and may only encode byte sequences within a literal;
the resulting name must be valid UTF-8. An M string value cannot contain `U+0000`,
so `\0` is allowed only in byte data, not in `LIT_STRING` or an M string constant.

Integer literals: decimal, `0x`/`0X` with hexadecimal digits, or `0b`/`0B` with binary digits; `_` is allowed only between digits. The sign `-` is part of the numerical literal constant. The range is checked by the instruction type or declaration. Floating-point literals use decimal notation with a dot, exponent, and an optional `f` suffix; additionally, `nan`, `inf`, and `-inf` are accepted. The canonical generator can write binary32/binary64 as `float(0xXXXXXXXX)` and `double(0xXXXXXXXXXXXXXXXX)`, preserving the original bits.

## 3. Example

```mir
mir 4
module "app/main.m"

type $Point = struct size 16 align 8 {
  .x: Word @ 0
  .y: Word @ 8
}

global @answer: Word = 42
global @greeting: String = "Hello"

import @twice from "lib/math.m": (value: Word): Word
extern @println: (value: String): Void

export "Point" $Point
export "answer" @answer
export "main" host @main

func @main(args: String[]): Word locals 0 stack 4 {
entry:
  LIT_UWORD 42
  RETURN
}
```

`locals` does not include parameters. `stack` includes parameters, a hidden packet, local slots, and temporary values – essentially, the maximum number of values for the current frame.

## 4. Declarations

### 4.1 Module

```ebnf
file           = "mir", "4", module-decl, { top-decl } ;
module-decl    = "module", string ;
top-decl       = type-decl | global-decl | import-decl
               | extern-decl | export-decl | function-decl ;
```

The module name is a normalized path with `/`, typically the original path with the `.m` suffix. It does not begin with `/`, does not contain empty components, `.`, `..`, backslashes, NUL, or colons. The frontend allows relative `import` statements before MIR creation; the MIR compiler does not search for source files. The root module indicator is not a syntactic property of MIR: it is assigned by the build driver when creating the `Entry-Module`.

### 4.2 Types

```ebnf
type-decl      = "type", type-symbol, "=", type-body ;
type-body      = struct-body | enum-body | type ;
struct-body    = [ "packed" ], "struct", "size", integer,
                 "align", integer, "{", { field-decl }, "}" ;
field-decl     = ".", name, ":", type, "@", integer ;
enum-body      = "enum", ":", integer-type, "{",
                 { ".", name, "=", integer }, "}" ;

type           = primitive | type-symbol | ref-type | array-type
               | function-type | nullable-type | "(", type, ")" ;
nullable-type  = type, "?" ;
primitive      = "Void" | "Bool" | "UByte" | "UHalf" | "UWord"
               | "ULong" | "Byte" | "Half" | "Word" | "Long"
               | "Float" | "Double" | "String" | "Type" | "Thread" ;
ref-type       = "@", [ "mut" ], type ;
array-type     = [ "mut" ], type, "[", "]" ;
function-type  = "(", [ parameter, { ",", parameter }, [ "," ] ],
                 ")", ":", type ;
parameter      = name, ":", ( type | "..." ) ;
type-symbol    = "$", name ;
```

An empty suffix `[]` represents only a dynamic array; the initial length of the array remains in a constant template and does not become part of the type. `mut T[]` is only permissible for parameters. The alias `type $A = T` does not create a new nominal identity. `struct` and `enum` do create one.

The parameter `name: ...` represents a named block of additional arguments and makes the function's type variable. It can only be the last parameter and may be the only one. The argument block has no type M and is not included in the list of fixed parameter types. The names of the parameters do not affect the identity of the functional type; it is the variability that matters.

The size, alignment, and offsets for `struct`s are already calculated by the frontend. Fields are listed in ascending order of their offsets, without overlapping, and completely contained within the size. Alignment must be a non-zero power of 2. The size must be a multiple of the alignment. The `packed` flag is maintained as a metadata flag, but it does not override explicit layout checking. Empty structs and recursive structures by value are prohibited; recursion via `@T`/`@mut T` is allowed.

`enum` has an integer-based type. The values of the elements must fit within it; the names and values of the elements must be unique.

### 4.3 Global Variables

```ebnf
global-decl    = "global", [ "mut" ],
                 global-symbol, ":", type, [ "=", constant ] ;
global-symbol  = "@", name ;
```

- `global @x: T = C` – represents an immutable value.
- `global mut @x: T = C` — an initialized mutable value.
- `global mut @x: T` — zero initializer `zero`.
- Declaring an immutable variable without using `=` is incorrect.

The initializer does not execute bytecode. The constant graph is described in section 7.

### 4.4 Module Import

```ebnf
import-decl    = "import", ( global-symbol | type-symbol ),
                 [ "as", string ], "from", string, ":", import-type ;
import-type    = type | function-type ;
```

`import @f` with a function signature imports a function with a different type – a global value. `import $T` imports a named type. The `from` string contains a normalized module name. The string after `as` defines an external name for the export; if `as` is absent, it matches the local `name`.

```mir
import @mix as "mix" from "color.m": (a: Word): Word
```

The MIR compiler does not perform self-import; instead, the final linker handles this via MO.

### 4.5 Native Import

```ebnf
extern-decl    = "extern", global-symbol, ":", function-type ;
```

`extern` has no module or body. The name without `@` is passed to the executor. All identically named `extern` functions within a single mvm must have the same signature.

### 4.6 Export

```ebnf
export-decl    = "export", string, [ "host" ],
                 ( global-symbol | type-symbol ), [ "doc", string ] ;
```

The export of functions and global values uses `@`, with the type `$`. External names are unique within a module, regardless of the object type. One object can be exported multiple times under different names.

The `host` function is only accessible to functions and global variables within the root module, and it makes exporting part of the executor API. Regular exporting is intended for inter-module resolution. `doc` preserves UTF-8 documentation without performing any actions.

### 4.7 Function

```ebnf
function-decl  = "func", global-symbol, function-type,
                 "locals", integer, "stack", integer,
                 "{", { label | debug-location | debug-variable | instruction }, "}" ;
label          = name, ":" ;
debug-location = ".", "loc", string, integer, integer ;
debug-variable = ".", "var", integer, string, ":", type ;
```

Fixed parameters receive indices `0..fixed_argc-1`; the following
indices for `locals` are additional local slots. The number of fixed
parameters is not greater than 255, for a variable function – not greater than 254, and the number of additional local slots is not greater than 65535. In the frame of a variable function, after fixed parameters, there is a hidden block in one slot. It is not accessible via `LOCAL_GET`/`LOCAL_SET`; local slots begin after it.

The label is local to a function and represents the offset between instructions. Two labels can point to the same location. Jumps are only allowed to labels within the same function. The last instruction of any possible path must transfer control; falling through a closing `}` is prohibited.

`.loc "file" line column` sets the initial position of subsequent instructions until the next `.loc` directive or the end of the function. The line number starts at 1; the column number starts at 1, and 0 indicates an unknown column. Markers do not change the position. The position is unknown when entering a function. The assembler creates `DEBUG_LINES` records only for instructions with a known position. `mvm-c --debug` / `mvm-c -d` generates these directives from the source AST; without the flag, they are not added.

`.var slot "name": Type` associates the original name and the full type with a local slot of the function. The slot/function combination is unique; variadic packets and temporary compiler slots are not described. The assembler saves records in `DEBUG_VARIABLES`. These directives are only created with `--debug` and do not modify the bytecode.

## 5. Instructions

The instruction name matches the opcode name without the `MVM_OP_` prefix. Numerical values, operand sizes, and stack semantics are defined in [MO](mo.md#10-isa-22).

### 5.1 Symbolic Operands

| MIR Form | What the compiler creates |
|---|---|
| `CALL @f, 2`, `TAIL_CALL @f, 2` | Function address and relocation |
| `CALL import @f, 2` | Relocation to import function |
| `LIT_FUNC @f`, `LIT_FUNC import @f` | function descriptor |
| `JUMP done`, `JUMP_IF_FALSE failed`, `JUMP_IF_TRUE yes` | Offset of the label within the current function |
| `JUMP_IF_LOCAL 0, 1, LT, true, loop` | two local indices, comparison, direction, and label |
| `GLOBAL_GET @g`, `GLOBAL_SET @g` | Local global variable index |
| `GLOBAL_GET import @g`, `GLOBAL_SET import @g` | Imported variable index |
| `LIT_STRING "text"` |  Creates a NUL-terminated string in the `rodata` section and performs relocation. |
| `NATIVE @f, 1` | Native import identifier |
| `TYPED_ALLOC $T`, `VALUE_COPY $T`, `VALUE_ZERO $T` | type identifier |
| `TYPED_ALLOC import $T` and similar | Imported type identifier |
| `LIT_TYPE_ID T`, `VA_ARG T`, `REF_READ @T`, `REF_WRITE @mut T`, `TYPED_INIT T` | full type identifier |
| The same operations with `import $T` | Imported type identifier |

In `CALL`, `TAIL_CALL`, `CALL_INDIRECT`, and `NATIVE`, `argc` is a decimal or hexadecimal integer `0..255`. For non-variadic calls, it equals the number of fixed parameters; for variadic calls, it equals `fixed_argc + 1`: the last slot is reserved for a hidden packet. For `CALL_INDIRECT`, this is verified against the actual function value during execution.

`LOCAL_GET 0`, `LOCAL_SET 3`, `FIELD_GET 1`, `FIELD_SET 1`, and `FIELD_ADDR 1`
accept an unsigned index in the range `0..65535`. The local slot index
must also be less than `fixed_argc + locals` for a non-variadic function, and
`fixed_argc + 1 + locals` for a variadic function; hidden package indices are prohibited.

### 5.2 Immediate Operands

- `LIT_UBYTE`, `LIT_UHALF`, `LIT_UWORD`, `LIT_ULONG` accept an integer within the corresponding type’s range.
- `LIT_SBYTE`, `LIT_SHALF`, `LIT_SWORD`, and `LIT_SLONG` accept signed integers.
- `LIT_FLOAT` and `LIT_DOUBLE` accept a floating-point literal or a bit-level representation.
- `LIT_NULL`, `LIT_FALSE`, `LIT_TRUE`, `RETURN`, `VA_FORWARD`, `VA_COUNT`,
  Arithmetic and stack instructions without operands are represented by a single word.
- `CAST`, `IS`, `LIT_TYPE`, `LOAD`, `STORE`, and `ARRAY_NEW` all accept a runtime-kind: `VOID`, `NULL`, `BOOL`, `UBYTE`, `UHALF`, `UWORD`, `ULONG`, `SBYTE`, `SHALF`, `SWORD`, `SLONG`, `FLOAT`, `DOUBLE`, `STRING`, `ARRAY`, `TYPE`, `REFERENCE`, `FUNC`, or `THREAD`. Specific instruction constraints from MO are mandatory; for example, `ARRAY_NEW VOID` is invalid.

Example body:

```mir
func @add(a: Word, b: Word): Word locals 0 stack 4 {
entry:
  LOCAL_GET 0
  LOCAL_GET 1
  ADD
  RETURN
}
```

### 5.3 Variable Calls

`VA_PACK [T1, T2, ...]` replaces additional arguments at the top of the stack with a single internal package. The number of types equals the number of values being packed; an empty list `VA_PACK []` creates an empty package. Each type is encoded with a unique `type_id` and can be `import $T`. Integer, `Bool`, `Float`, `Double`, `String`, `Type`, enum, `@T`/`@mut T`, and functions are allowed; structures, arrays, `Thread`, and `Void` are prohibited.

```mir
LIT_SWORD 10
LIT_SWORD 20
LIT_SWORD 30
VA_PACK [Word, Word]
CALL @sum, 2
```

The package contains 8-byte representations of values, their full types, and information for GC. This is an internal call ABI value, not an M value – it cannot be stored in a slot, a global variable, a field, or an array, nor can it be returned from a function or retrieved via its `Type`. `VA_FORWARD` places the current variadic function's package onto the stack without copying values or types. The redirection is passed via a single additional argument; it cannot be mixed with other values.

`VA_COUNT` returns `ULong`: the number of elements in the current frame's packet.
`VA_ARG T` pops an unsigned index and returns a value after bounds and type checks.
Integers allow signedness changes and truncation to the low bits, but not widening.
Enums require the same nominal identity. `Float`, `Double`, `Bool`, `String`,
and `Type` require an exact type. Functions are checked against their full signature.
An `@T` can be read from `@mut T`, but an `@mut T` cannot be obtained from `@T`.
`VA_FORWARD`, `VA_COUNT`, and `VA_ARG` are valid only in variadic functions.

The `NATIVE` variant uses the same package. The VM calls the native-adapter, which maintains GC roots and provides a trusted executor; `va_list` and variable calls with libffi are not used for this.

### 5.4 Typed References and Complete Types

`REF_READ` accepts a complete type `@T` or `@mut T`; `REF_WRITE` accepts only `@mut T`.
Both instructions check `null`, object ownership by the instance, and the complete
types of the reference and contents; writing also checks `@mut` permission.
A VM reference value retains its complete type and access rights, including for interior references.

`TYPED_INIT T` retrieves the value `T`, allocates an object, and returns an initialized reference `@mut T`. The source and intermediate object are retained by the GC. The reference is not exposed until initialization is complete. The structure is copied by value, without deep copying of nested objects.

`LIT_TYPE_ID T` creates a complete `Type` descriptor. Aliases provide the same descriptor; structs/enums preserve nominal identity, arrays – element type, references – rights, functions – signature and variability. `EQ`/`NE` compare canonical full `type_id`s. `LIT_TYPE` and `TYPEOF` are preserved for compatibility as runtime operations and do not encode `typeof` in MIR 2.

The M compiler calculates the type of the `typeof` operand statically and only generates `LIT_TYPE_ID`. No operand code is created or executed.

## 6. Stack MIR Verification

The assembler must perform control flow analysis based on labels.

- Upon entry, the depth is equal to the number of fixed parameters plus one hidden packet for the variable function.
- `LOCAL_GET` requires an existing slot, so the function compiler typically first initializes additional local slots.
- `VA_PACK` requires `count` values and replaces them with a single package; `VA_FORWARD` adds one slot, `VA_COUNT` adds one slot, `VA_ARG` does not change the depth.
- Any instruction must have the required number of values from the current frame.
- The stack depth at each label is consistent for all incoming paths.
- The depth never exceeds the declared `stack`.
- `RETURN` has an upper limit on the result; for `Void`, the compiler places an internal `LIT_NULL`, as required by the current VM ABI.
- Each instruction is reachable from the function's entry point; following an unconditional jump, the subsequent instruction must have a label with an incoming transition.

Analysis checks depth, not full static typing of the stack. The language types have already been verified by the frontend, and the MO loader repeats structural checks.

## 7. Constant Graph

```ebnf
constant       = "zero" | "null" | "true" | "false" | number | string
               | "func", [ "import" ], global-symbol
               | "typeof", "(", [ "import" ], type-symbol, ")"
               | "global", [ "import" ], global-symbol
               | "array", "(", type, ",", "[", [ constants ], "]", ")"
               | "arrayN", "(", type, ",", integer, ",",
                 "[", [ constants ], "]", ")"
               | "struct", "(", type-symbol, ",", "[", [ constants ], "]", ")"
               | "object", "(", type, ",", constant, ")" ;
constants      = constant, { ",", constant }, [ "," ] ;
```

- `zero` – the default value for the expected type. For references, strings, arrays, functions, and `Thread`, it’s `null`; for `Type`, it’s `typeof(Void)`. Structures are zeroed recursively.
- `null` is type-checked to expect a reference context.
- `func` defines either the function within the current module or an import.
- `typeof` sets a type descriptor without executing any code.
- `global` only reads immutable global constants. A reference to an array or object maintains the underlying object within the created instance.
- `array(T[], [...])` creates a new array. The type of each element must match `T`, and the length must equal the number of elements. For the global `T[n]` template, the initial length is found in a separate template: `arrayN(T[], N, [...])`. Missing elements are assigned `zero`. `N` and the number of elements are limited by `UINT32_MAX`.
- `struct($T, [...])` contains one value for each field in the order of declaration. Any omitted fields in the source are represented as `zero`.
- `object(@T, C)` or `object(@mut T, C)` creates a separate managed object with a copy of `C`.

Each instance of `array`, `arrayN`, or `object` creates a new object within each instance. References via `global` utilize a pre-existing value and therefore can maintain a common identity. Loops dependent on global initializers are prohibited. Calling functions, reading `global mut`, and executing instructions within a constant graph are forbidden.

## 8. Diagnostics and Canonical Representation

The error message should include a path ending in `.mir`, a string, a column, and a brief description.

The assembler rejects unknown declarations, instructions, types, and constant values; it does not attempt to repair the file heuristically.

Canonical MIR uses LF, two spaces for indentation in bodies, declaration order in the input file, explicit `size`/`align`/offsets, and bit-form for NaN.

Canonical form simplifies comparison and testing, but does not alter semantics.


## MIR 3 Extension: union

`union(T1, T2, ...)` is a structural type consisting of 2–4096 distinct concrete variants.

An alias can be exported as a named access to its original type; it does not create a new nominal identity. Nested unions are flattened by the frontend. The order of variants does not affect equality.

| Instruction | Operands | Stack |
|---|---|---|
| `UNION_NEW` | complete union type, complete type of variant | `T → U` |
| `UNION_VARIANTOF` | no | `U → Type` |
| `UNION_GET` | full type variant | `U → T` |
| `UNION_WIDEN` | full target union type | `U → V` |
| `TYPED_CONSTRUCT` | full type | `→ internal storage for the value being constructed` |
| `ARRAY_NEW_TYPED` | full array type | `length → array` |

All generic operands use type relocation. `UNION_GET` copies the struct by value. `TYPED_CONSTRUCT` initializes GC-safe slots, but does not create a default for the union; accessing an uninitialized field is not possible. `TYPED_ALLOC` and `VALUE_ZERO` reject non-default types. `ARRAY_NEW_TYPED` with a non-zero length requires a default for the element. A standard union array literal is created empty and filled through `ARRAY_PUSH`.

Constant templates: `unionValue(U, T, payload)` and `unionWiden(V, U, payload)`. The `payload` is a constant of the corresponding type.

Full label descriptors for `switch` are generated via `LIT_TYPE_ID`, followed by comparison using `EQ`. For a standard type, `variantof` calculates and removes the operand from the stack before `LIT_TYPE_ID`; `typeof` does not calculate the operand.

Service exports `$mvm-c$type$...` preserve the identity of nominal types, accessible within exported unions and signatures. They are not host ABI.

## MIR 4 Extension: Explicit Nullability

The postfix `?` marks nullable reference-like types and participates in full type identity. Parentheses disambiguate nullable references and functions: `(@mut $Node)?` and `((value: Word): Void)?`. `String?[]` and `String[]?` remain distinct. MIR 2/3 inputs retain their legacy implicit-nullability behavior.

`NULL_CHECK` converts a checked nullable value to its non-null representation and traps on null. Non-null native results are validated at the boundary; nullable results may contain null.
