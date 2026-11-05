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

# 4. Expressions

[← Declarations](03-declarations.md) · [Table of Contents](README.md) · [Operators →](05-statements.md)

## 4.1 Priorities

From strong to weak:

| Level | Operators | Associativity |
|---|---|---|
| 1 | Calling `f(...)`, accessing `a[i]`, and accessing `a.b` | on the left |
| 2 | unary `-`, `!`, `~`, `@` | on the right |
| 3 | `as` | on the left |
| 4 | `*`, `/`, `%` | left |
| 5 | `+`, `-` | left |
| 6 | `<<`, `>>` | left |
| 7 | `&` | from the left |
| 8 | `^` | from the left |
| 9 | `\|` | left |
| 10 | `==`, `!=`, `<`, `<=`, `>`, `>=` | no |
| 11 | `&&` | left |
| 12 | `\|\|` | left |
| 13 | `?:` | right |

- **`&`, `|`, `^` are stronger than comparisons**, unlike in C:
  `x & MASK == 0` is equivalent to `(x & MASK) == 0`.
- **`as` is stronger than arithmetic:** `a + b as UWord` is equivalent to `a + (b as UWord)`, and `-x as UWord` is equivalent to `(-x) as UWord`.
- **Comparisons are not chained:** `a < b < c` is an error; you need `a < b && b < c`.
- Ranges `..` and `...` are not expressions; they are only used within `for` loops.
  ([5.7](05-statements.md#57-for)).
- Assignment operators, such as `+=` and `++`, and `--`, are statements, not expressions.
([5.3](05-statements.md#53-assignment)).

## 4.2 Literals and Constant Expressions

### Literal Type

An integer literal does not have its own type until it is defined by the context.

- The variable type in `let`, the type of parameter when calling, and the type of the result in `return`.
- Type of another operand for a binary operator: in `a + 1`, for `a: UByte`
  literal `1` – `UByte`.
- the type of a field or element within a literal structure or array;
- The type of variable `for` used for the boundaries and step of the range.
- Target type in `as`: `7 as Direction`.

A literal without a specified type according to the context is an error. Since the type of each variable is explicitly defined, this rarely happens – for example, `1 << 40` without a variable on the left.

`typeof` and `variantof` always return a `Type` value, regardless of the external context ([2.14](02-types.md#214-type-descriptors)).

`sizeof`, `alignof`, and `offsetof` behave like integer literals: they are non-typed constants, and their type is determined by the context. The `length(a)` of an array returns a regular `UWord` value during execution.

Floating literals with the `f` suffix, such as `1.5f`, always have the type `Float`. Literals without the suffix, such as `1.5`, always have the type `Double`. The context does not change these types; to convert between `Float` and `Double`, use `as`.

`'a'` — always `UByte`; `"..."` — always `String`; `true`/`false` — `Bool`; `null` — that reference, which can be a `String`, a dynamic array, or a function type, expected by the context.

### Constant Expression

The value is known at compile time, if the expression consists only of:

- literals;
- Global `let`-variables (without `mut`) with a constant initializer;
- Local `let`-variables (without `mut`) of a scalar type or `Type` with a constant initializer: these cannot be modified, so their value always remains the same as when they were initialized (3.2).
- elements of `enum`;
- `sizeof`, `alignof`, `offsetof`, `typeof`;
- arithmetic, bitwise, shift, comparison, logical operators, conditional operator, and `as` applied to all of the above.
- literals of structs from constants
- Dynamic literal templates for arrays – only for global initialization, without calculating their length or content.
- descriptors for functions and function literals
- Templates `@value` with constant content – only for global initialization. When a VM instance is created, a separate object is generated; a reference to it is not a numerical constant.

Where constants are mandatory: initializer and initial length of a global variable, an element of an `enum`, a `case` label, and the `by` step.

The initial length of a local array can be calculated during execution (3.2).

`typeof(expr)` is a constant, even for variables that are modified or function calls:
the value of the operand is not evaluated. Comparing `Type` constants using
`==`/`!=` is also constant; this does not make `Type` an integer type, and
does not allow it to be used as a step or array size. Constant descriptors are acceptable as `case` labels in `switch` statements based on `Type`.

`variantof(expr)` calculates the expression exactly once. For unions, it returns the type of the active variant; for regular values, it returns their full static type.

This is a runtime operation and is not permitted in a constant initializer. Explicit references are not read automatically; details can be found in [2.16](02-types.md#216-union-types).

**Constant expression consisting only of literals** (and `sizeof`, `alignof`, `offsetof`) **is treated precisely**, with arbitrary precision, and only the result is cast to the context type.
If it doesn't fit—an error:

```
let a: UByte = 300 - 100        // 200: Intermediate 300 – possible
let b: UByte = 300              // error
let c: UByte = -1               // error
let d: UByte = 0x10 * 0x10      // Error: 256
```

If a constant expression contains a typed value (a `let` constant or an `enum`
member), operations use its type and the ordinary wrapping rules (4.7).

Floating constant expressions are evaluated during compilation according to IEEE 754 in the context type, rounded to the nearest even number – just as the VM would calculate them.

## 4.3 Basic Expressions

- literal (1.5–1.7);
- Variable name, parameter, function name, element name of the `enum` (`Cause.Syscall`).
- expression within parentheses `( ... )`
- The literal of a struct `{ .r = 1, .g = 2 }` and an array `[1, 2, 3]` – only when the type is known from the context (4.11);
- Function literal: `(x: Word): Word { return x * 2 }`
([3.8](03-declarations.md#38-nested-functions-and-literals));
- Calling a built-in function ([7.4](07-runtime.md#74-built-in-functions)).

## 4.4 Postfix Operators

### Call

```
f(a, b)
handlers[code](code)
```

- You can call a function by its name (including `extern`) or by its function type value. Calling `null` or an invalid descriptor is a trap ([2.8](02-types.md#28-function-type)).
- The generic function initially receives explicit type arguments:
  `identity::<Word>(42)`. This form without a call provides a specific function descriptor; the name `identity` itself is not assigned a value (2.15).
- The number and types of arguments must match the parameters (taking into account implicit casts [2.10](02-types.md#210-compatibility-and-implicit-conversions)). For a function with the last `args: ...`, this rule applies to fixed parameters; additional arguments of permissible types form a package [3.4](03-declarations.md#variable-parameters).
- **Arguments are evaluated from left to right**, followed by the call.

### Index

`a[i]` refers to an element within the `T[]` array. The expression `s[i]` for the `s: String` variable returns a Unicode code point of type `UWord`.

- An index may have any unsigned integer type. A signed index is an error;
  an explicit `as` cast to an unsigned type is required.
- For an array, it’s required that `i < length(a)`. Accessing beyond the length of `a` or accessing `null` results in `memory` access. Accessing beyond the length is not possible.
- Modifying an element requires a mutable path to the array (2.5).
- The string requires `i < length(s)`; accessing beyond the length results in `memory`, and operations with `null` result in `string`. String elements are immutable.
- A reference to an array or string is read automatically: `r[i]` accesses the element's content. One level of referencing is automatically removed. Modifying an array element via this path requires `@mut T`. `@a[i]` still creates a new object with a copy of the element.

### Field

`s.f` is a field within the struct. For `r: @T`/`@mut T`, where `T` is a struct, `r.f` accesses its field without requiring any additional access property.

- Writing to a field within a struct via a reference requires `@mut T`, regardless of whether the variable itself is mutable. Upon access, a `null` check is performed; the reference is maintained until the operation is complete.
- The `value` name is not reserved and is not a property of the reference:

`r.value` is only valid when the `T` struct has a regular `value` field.
- Implicit dereferencing removes one level. For nested references, the inner reference is first assigned to a variable of its type (2.4).
- The `String` type has no fields. The VM checks the full content type and write permissions.

## 4.5 Unary Operators

| Operator | Operand | Result |
|---|---|---|
| `-x` | integer | Same, modulo type width |
| `-x` | `Float`, `Double` | Same, but with altered sign |
| `~x` | integer | bitwise NOT of the same type |
| `!x` | `Bool` | logical negation |
| `@x` | Value of `T`, excluding `Void` | `@mut T`, a new GC object with a copy of the value |

`@` does not bind to the storage location of the operand. For example, `@local`, `@record.field`, `@items[i]`, and `@makeValue()` create new objects; subsequent changes to their contents do not reassign the original location.

The rules for lifetime and write access are described in [2.4](02-types.md#24-references).

## 4.6 Casting `as`

`x as T` is an explicit type conversion (apart from the three implicit
conversions in [2.10](02-types.md#210-compatibility-and-implicit-conversions)).

| Source | Destination | What happens |
|---|---|---|
| integer | wider integer | expansion: signed – by sign, unsigned – by zeros (depending on the **source** type) |
| integer | integer already or of the same width | least significant bits; sign – according to the result type |
| Integer | `Float`, `Double` | Round to nearest even |
| `Float`, `Double` | integer | to zero, **with wrapping**: if greater than maximum – maximum, if less than minimum – minimum; NaN – 0 (not UB) |
| `Float` | `Double` | Precision |
| `Double` | `Float` | Round to nearest even; out of range — ±∞ |
| `Bool` | integer | `false` = 0, `true` = 1 |
| integer | `enum` | cast to a base type; enum membership is not checked |
| `enum` | integer | cast to a whole number type according to rules |

When `r as T`, where `r: @T`/`@mut T`, a copy of the content is read.
If the content is numerical, the regular numerical `as` can also follow an implicit read: `r as Long` for `r: @Word` reads `Word` and expands it to `Long`. The numerical descriptor does not become a link, and a link is not created from a number. If `T` itself is a link, `r as T` reads the stored internal link; this is a read of a typed value, not a reinterpretation of an external link descriptor.

Integers are not cast to `Bool`: use `x != 0`. Strings, arrays, and functions are not cast to numbers and are not reconstructed from numbers.

The `Type` descriptor is not cast to a number, string, or any other type; implicit reading of `@Type`/`@mut Type` as `Type` follows general rules.

Casting descriptors between different types of references, between structs, arrays, or functions of different types, and casting `Void` is not defined.

`@mut T` becomes `@T` only when implicit read-write removal is explicitly allowed; no inverse conversion exists.

## 4.7 Arithmetic

`+`, `-`, `*`, `/`, `%`.

For two operands of type `String`, the operator `+` performs concatenation and returns a `String`. The contents of the operands are preserved. Joining with a number requires explicit formatting using a library function. The compiler uses `CONCAT` for the string `+` operator; other arithmetic operations cannot be applied to `String`.

For numerical operands:

- **Both operands must be of the same type**, and the result will be of the same type. There are no overflows, as in C: `UByte + UByte` results in `UByte`, and `UByte + UWord` results in an error – you need to use `(a as UWord) + b`.
- `+`, `-`, `*` **always wrap** modulo 2ⁿ, including for signed integers:
  `0xFF + 1` for `UByte` gives `0x00`, and `127 + 1` for `Byte` gives `-128`.
  Overflow does not trap.
- **Division by zero does not trap**; its result is defined by the language:
  `x / 0` is all one bits in the type (`0xFF` for `UByte`, `0xFFFF_FFFF_FFFF_FFFF`
  for `ULong`, and `-1` for signed types); `x % 0` is `x`.
- `MIN / -1` for signed integers – `MIN`, `MIN % -1` – `0`.
- `/` rounds to zero, and the sign of `%` matches that of the dividend, as in C.
- For `Float` and `Double`: `+`, `-`, `*`, `/` are supported according to IEEE 754, without traps.
  `%` is not supported.

## 4.8 Bitwise Operators and Shifts

- `&`, `|`, `^`, `~` – are only used with integers; both operands must be of the same type.
- `x << n`, `x >> n`:
  - `x` is an integer, and the result’s type is `x`.
  - `n` — any `unsigned integer` type, not necessarily the same as the one used for `x`.
  - The shift occurs in the width of the type `x`, and the displaced bits are lost: for `UByte`.
    `0x81 << 1` — `0x02`
  - For **shifts by the type width or more**, the count is not reduced modulo
    the width as on CPUs; the shift runs to completion: `x << n` and unsigned
    `x >> n` give `0`, while signed `x >> n` gives `0` or `-1` according to
    the sign of `x`. For `UByte`, `x << 8` is `0`; for `ULong`, `x << 64` is `0`;
  - `>>` is arithmetic for signed integers, and logical for unsigned integers.

## 4.9 Comparisons and Logical Operators

- `==`, `!=` – for integers, `Float`, `Double`, `Bool`, `String`, `Type`, `enum`, links, dynamic arrays, and functions. The types of operands are compared, taking into account permitted implicit casts; links, strings, arrays, and functions can be compared with `null`.
- Two strings are compared based on their UTF-8 content, without performing Unicode normalization. `null == null` — `true`, and `null == ""` — `false`.
- Two `Type` values are compared for type equality, as described ([2.14](02-types.md#214-type-descriptors)), rather than by name or size. Comparing a `Type` with `null` is an error.
- `<`, `<=`, `>`, `>=` – apply to integers, `Float`, `Double`, and `enum` types, including after reading such content via a reference.
  Strings, arrays, functions, and descriptors themselves are not sorted.
- Two compatible links `==`/`!=` are compared for identity.
  When comparing a link to a regular `T` value, its contents are read.
  For example, `a == b` for two `@Word` objects compares objects, while
  `a as Word == b as Word` compares numbers. Arrays are compared for identity; structures `==` are not compared.
- The result is `Bool`.
- `Float`, `Double`: NaN is not equal to anything, not even itself; `-0.0 == 0.0`.
- `&&`, `||` accept only `Bool` and **short-circuit**: the right operand
  is evaluated only when the result depends on it.

## 4.10 Conditional Operator

`condition ? whenTrue : whenFalse` evaluates the condition of type `Bool`, then
only one of the branches is executed, and its value is returned.

- Both branches are checked during compilation. Their types must match, taking into account permitted implicit casts; the expression context is applied to both branches, resulting in a common type for the result.
- The operator is associative on the right: `a ? b : c ? d : e` is equivalent to `a ? b : (c ? d : e)`.
- If the condition and the chosen branch are constant, then the condition and the result are also constant.

## 4.11 Structures and Arrays Literals

```
let c: Color = { .r = 0xFF, .b = 0x80 }
let p: Point[] = [{ .x = 1 }, { .y = 2 }]
```

- Unnamed literal; the type is determined from the context: the type of the variable, parameter, result, field, element, or record accessed via a reference. Without context – an error.
- A struct literal names fields with `.name = value`, in any order, each at most
  once. An unknown field is an error.
- Unnamed fields of the struct receive zero values. A literal of an array in the context of `T[]` creates a new object consisting exactly of the specified elements; `[]` creates a new empty array.
- In the declaration `let a: T[n] = [...]`, the initial length is `n`, and any missing elements are assigned zero values. This is a rule of declaration (3.2), and not an additional type literal.
- The initial length and the number of literal elements must not exceed `4_294_967_295`. The number of listed elements must not exceed the specified `n`; this is described in 3.2.
- Commas are required between elements, with the last one being optional.
- Values are calculated from left to right.

## 4.12 Order of Evaluation

- The arguments passed in a call are from left to right.
- The operands for binary operators are read from left to right.
- `&&` and `||` – the right-hand operand may not be evaluated.
- `?:` – evaluates the condition and only selects the chosen branch.
- In assignment, the expression on the left is evaluated first, followed by the value on the right;
  necessary references are held until the assignment is complete.
