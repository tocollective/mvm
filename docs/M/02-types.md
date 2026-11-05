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

# 2. Types

[← Lexical](01-lexical.md) · [Table of Contents](README.md) · [Declarations →](03-declarations.md)

## 2.1 Overview

| Category | Types |
|---|---|
| Integers | `Byte`, `UByte`, `Half`, `UHalf`, `Word`, `UWord`, `Long`, `ULong` |
| Logical | `Bool` |
| Floating-point | `Float`, `Double` |
| Strings | `String` – a reference to an immutable UTF-8 text. |
| Threads | `Thread` – an opaque handle to a specific OS thread instance |
| Type Descriptors | `Type` — an opaque descriptor of a fully static type |
| Empty | `Void` – only the result type |
| References | `@T`, `@mut T` – typed references to GC-managed objects |
| Arrays | `T[]`; creation with an initial length: `let arr: T[n] = []`; mutable parameter `mut T[]` |
| Structures | `type Name { ... }`, `packed type Name { ... }` |
| Enums | `enum Name: Base { ... }` |
| Combinations | `A | B` – one of the options from a closed set |
| Functions | `(a: A, b: B): R` |
| Aliases | `type Name = T` |
| Generic Types | `type Box<T> { value: T }`, `Box<Word>`, `type Items<T> = T[]` |

Type information is not displayed: the type of each variable, parameter, and result is written explicitly ([3.2](03-declarations.md#32-variables)). The types of generic arguments for declarations are also specified explicitly ([2.15](#215-generic-types-and-functions)).

Reference-like types are non-null by default. The postfix `?` makes a type explicitly nullable: `String?`, `T[]?`, `@T?`, `@mut T?`, function types, `Thread?`, `Mutex?`, `ConditionVariable?`, and `Atomic?`. Suffixes compose from left to right: `String?[]` is a non-null array of nullable strings, while `String[]?` is a nullable array of non-null strings. Conversion from `T` to `T?` is implicit; consuming `T?` as `T` requires flow-sensitive null narrowing or a checked `as T` conversion. Nullable locals are narrowed by `if (x != null)`, by early exits such as `if (x == null) return`, in the corresponding `while` body, and on the short-circuit side of `&&`/`||` where appropriate.

## 2.2 Scalar Types

| M | C | Size / Alignment | Range |
|---|---|---|---|
| `Byte` | `int8_t` | 1 / 1 | −128 … 127 |
| `UByte` | `uint8_t` | 1 / 1 | 0 … 255 |
| `Half` | `int16_t` | 2 / 2 | −32 768 … 32 767 |
| `UHalf` | `uint16_t` | 2 / 2 | 0 … 65 535 |
| `Word` | `int32_t` | 4 / 4 | −2³¹ … 2³¹−1 |
| `UWord` | `uint32_t` | 4 / 4 | 0 … 2³²−1 |
| `Long` | `int64_t` | 8 / 8 | −2⁶³ … 2⁶³−1 |
| `ULong` | `uint64_t` | 8 / 8 | 0 … 2⁶⁴−1 |
| `Bool` | `bool` | 1 / 1 | `false` = 0, `true` = 1 |
| `Float` | `float` | 4 / 4 | IEEE 754 binary32 |
| `Double` | `double` | 8 / 8 | IEEE 754 binary64 |

Column C displays the corresponding native ABI mvm scalar types
([8.2](08-interop.md#82-types-at-the-boundary)). Size and alignment
determine the representation of M values in the instance's memory.

- Signed integers are represented in two's complement. The byte order in memory is little-endian, on any executor platform.
- `ULong` is used for sizes and counters. The representation of references is closed to the program; there are no conversions between references and numbers.
- `Float` and `Double` adhere to the IEEE 754 standard, using round-to-nearest-even rounding. Without traps or flags: invalid operations result in NaN, overflow, and division by zero—±∞. The exact bits used for the NaN result are not defined by the language.
- `Void` is written as the return type of a function, as well as in the type of the function itself, and also as an argument `typeof(Void)` (2.14). There are no variables, parameters, fields, arrays, or references with the value `Void`.

## 2.3 Mutability

`let` prevents modification of the variable's value and its fields; `let mut` allows it.

For arrays, this also defines the right to modify elements and the length through this name. The mutability of a reference variable and an object are independent: `let r: @mut T` prevents replacing the reference itself, but allows writing to the value `T` through `r`, as well as modifying its fields and elements.

The reference `@T` provides read access, while `@mut T` provides both read and write access. These are distinct types. `mut T[]` in the parameter allows modification of the array object, while preserving the immutability of the parameter itself (2.5).

## 2.4 References

`@T` and `@mut T` are typed references to individual GC-managed objects, containing the value of `T`. `T` is any data type, except `Void`.

```m
let number: ULong = 1
let cell: @mut ULong = @number
cell = 2                        // The number remains equal to 1.
let readOnly: @ULong = cell            // The same object, read access.
let mut current: @ULong = readOnly     // It can be replaced.
current = @10                         // New object.
```

- The size and alignment of the link – 8 / 8. The link represents an object of a specific VM instance. Its representation is hidden from the program.
- `@expression` creates a new GC object and copies the value of the expression into it. The resulting object has the type `@mut T`, and it can be implicitly passed where `@T` is required. The link context defines the type of the literal operand: `let r: @Word = @42`, `let p: @Point = @{ .x = 1, .y = 2 }`.
- The operand is calculated exactly once; its value and any references contained within it are held until the allocation is complete. The reference is published after the entire object has been initialized. A failure to allocate results in an `allocation`.
- Each calculation of `@expression` creates a separate object. Fields within structures are copied by value; arrays and references maintain shared objects. `@r` creates a new object with the reference `r` within.
- Assignment, passing, and returning—these all copy the reference itself: all aliases refer to a single object. The GC (garbage collector) retains the object as long as the reference is reachable, and it collects unreachable cycles. The reference can be returned from a function.
- References are transparent in operations on values. If an operation expects `T`, the reference `@T`/`@mut T` is read as its content. For example, `let n: Word = r` copies a number, `r + 1` performs arithmetic, `r.x` accesses a field of a struct, and `r[i]` accesses an element of an array. There is no separate property for accessing the content: `value` remains a regular field name if it is declared.
- In a reference-type context, the reference itself is preserved:
  `let alias: @T = r`, an argument to an `@T` parameter, and an `@T` function
  result do not copy the contents. This does not make `@T` and `T` the same type:
  function signatures differ. Implicit reading removes one reference level;
  nested references can be read through an intermediate variable of the appropriate type.
- There are two cases for assignment. If the right-hand side is compatible with the type of the reference, the reference is replaced; this requires `let mut`. Otherwise, the right-hand side must be compatible with `T`, and the assignment goes to an object via `@mut T`. For example, `r = 20` changes a number, while `r = otherRef` or `r = @20` replaces a reference. The reference case takes precedence, including for `r = null`; if replacement is prohibited, the compiler does not choose the other case.
- `r += 1`, `r++`, `r.field = v`, and `r[i] = v` modify the content and require `@mut T`; reassignment of `r` is not performed for these operations.
  The full assignment `r = v` checks standard compatibility rules with `T`, including copying structures by value or array descriptors.
  This is not deep immutability: nested references retain their permissions.

- `@x` creates a copy of the value in a new object, rather than a link to the variable `x`. There are no references to local slots, global variables, fields, or array elements. For example, `@a[i]` copies the current element into a new object; subsequent writes to it do not replace the element `a[i]`.
- `@T` and `@mut T` are non-null. Use `@T?` or `@mut T?` when absence is intentional. Dereferencing a nullable reference requires narrowing or checked `as`; defensive VM checks still report `memory` for malformed values. Copying a reference and comparing it to `null` do not read the contents. Two compatible references are compared by identity, while a reference and a value of type `T` are compared by value after reading. To perform explicit comparison of the contents of two references, you first obtain the values of type `T` (e.g., using `as T`). The references themselves are not ordered.
- `@mut T` is implicitly converted to `@T`. It is not possible to add write access, modify the content type, or convert a reference to a number using `as`. There is no generic, untyped reference type; data for callbacks is passed as a explicitly typed parameter.

| Declaration | Replace reference | Write value through `r` |
|---|---|---|
| `let r: @T` | no | no |
| `let mut r: @T` | yes | no |
| `let r: @mut T` | no | yes |
| `let mut r: @mut T` | yes | yes |

These rules are the target contract of M. The current C `REFERENCE` kind and
native ABI do not yet check the complete type and access rights of `@T`/`@mut T`;
the differences are described in [gc.md](../gc.md).

## 2.5 Arrays

**All arrays are dynamic.** `T[]` is a reference to a typed GC object whose length
can change. Length is not part of the type.

```m
let mut values: Word[] = [10, 20]
arrayPush(values, 30)
let count: UWord = 5
let mut buffer: Word[count] = []  // Type Word[], initial length 5.
buffer[0] = 7
arrayPush(buffer, 9)                   // Length 6.
let alias: Word[] = buffer        // The same reference.
let empty: Word[] = []            // Isolated empty object.
```

- `T[]` can be used for variables, fields, function parameters, and results. The type of elements is specified explicitly; it can also be a specific generic instance, for example, `Box<Word>[]`.
- `let arr: T[n] = [...]` — a special form of declaring a new array with an initial length `n`, rather than defining a separate type. It is valid with and without `mut`; the type of the variable is always `T[]`. `n` can be an expression evaluated at runtime. The full rules can be found at [3.2](03-declarations.md#array-with-initial-length).
- The size and alignment of the `T[]` value are 8 / 8: this is a reference, not
  embedded elements. Assignment, passing, and returning copy the reference;
  all aliases see changes to a single object. `==` and `!=` compare identity. `T[]` is non-null; `T[]?` explicitly permits `null`, and operations require narrowing or a checked conversion.
- A literal within the context of `T[]` creates a new array consisting of exactly the specified elements. `[]` represents a non-zero, empty array. In the declaration `T[n]`, omitted elements must have defaults: numbers are zero, `Bool` is `false`, `Type` is `typeof(Void)`, nullable references and functions are `null`, and defaultable structs are initialized recursively. Non-null elements must all be supplied explicitly. An inner non-null array has no implicit default; use `T[]?` for an initially missing inner array.
- `length(a)` returns the current length as a `UWord`, and `arrayCapacity(a)` — the capacity as a `UWord`.  The condition `length(a) <= arrayCapacity(a)` always holds.  Reserves are not available through indexing. The maximum length and capacity are `4_294_967_295`, with actual allocation limited by the instance's memory.
- The operations `arrayReserve`, `arrayResize`, `arrayPush`, `arrayPop`, `arrayInsert`, and `arrayRemove` are described in [7.4](07-runtime.md#dynamic-array-operations). Growth preserves the array's identity, may move the internal buffer, and can trigger GC.
- Recording elements and changing the length require a mutable access path:
  `let mut`, a mutable field or parameter `mut T[]`. `let a: T[]` and
  the parameter `T[]` provide access for reading through this name. This does not freeze
  the object: a mutable alias can change it. For example, assigning
  `let mut b: T[] = a` creates such an alias; `a` will see its changes.
  In the parameter `mut T[]`, you can modify the object, but not reorder the parameter.
- To access, an array and index are maintained, with current boundaries checked on each access. `@a[i]` creates a separate object with a copy of the element's value; it is not linked to its position within the array.
- The type of elements – any data type, except `Void`. Arrays can be nested; a struct can contain an array of `Node[]` of its own type, because it’s a reference. GC traverses live elements using metadata, including references within structs. Deleted elements do not retain objects.
- Native functions receive a descriptor `T[]`; the internal buffer is closed for the program (8.2).

### How to Read the Type

The suffix `[]` applies to **all types on the left**:

| Record | What is this? |
|---|---|
| `@UByte[]` | Array of `@UByte` references |
| `@(UByte[])` | Reference to an object with a `UByte[]` value |
| `String[]` | An array of strings, including the `args` parameter of the `main` function. |
| `UWord[][]` | Array of references to `UWord` arrays |

Inside a bracket type, the value is always empty. Non-empty `[n]` are only permitted as the last suffix in a special array declaration (3.2). Round brackets in a type only group.

The existing C API and VM instructions are described in [gc.md](../gc.md).
The M specification defines the target semantics; however, the M compiler has not yet been implemented.

## 2.6 Structures

```
type Header {
    kind:   UByte,
    length: UWord,
    next:   @Header,
    flags:  UHalf,
}
```

- Fields are separated by commas, with the final comma being optional. There must be at least one field. Field names within the struct are unique.
- A field can have any type except `Void` or the struct itself by value
  (directly or through other structs). References and arrays are allowed:
  `next: @mut Node`, `children: Node[]`.
- Structures are compared by name: two structures with identical fields—
  represent different types.

### Layout

Regarding how fields in a C-style struct are handled on a 64-bit platform. This refers to the layout of the value, which is used by the compiler, garbage collection metadata, and the typed executor API ([8.5](08-interop.md#85-accessing-objects-and-values)).

- The fields are ordered according to their declaration order; the compiler does not rearrange them.
- Each field is aligned to its own alignment, with a preceding indent added if necessary.
- Structure alignment – the maximum alignment of a field.
- Size – rounding to the alignment of the last field in the structure.

The fields `@T`, `@mut T`, `String`, and `T[]` contain object descriptors.

For structs with VM references, it additionally stores a reference map; this is not included in `sizeof` and does not alter field offsets. Copying such a value must preserve the information needed by GC. The `executor` writes reference fields via a typed API of the VM, not through `memcpy`.

| `Header` | Offset | Size |
|---|---|---|
| `kind` | 0 | 1 |
| Indentation | 1 | 3 |
| `length` | 4 | 4 |
| `next` | 8 | 8 |
| `flags` | 16 | 2 |
| Indentation | 18 | 6 |
| **Result** | | **24**, alignment 8 |

### `packed type`

```
packed type DiskHeader {
    kind:   UByte,      // offset 0
    length: UWord,      // offset 1
    next:   @Header,    // offset 5
    flags:  UHalf,      // offset 13
}                       // Size 15, alignment 1
```

- There are no indentation, structure alignment is 1, size is the sum of the sizes of the fields.
- The VM allows for unaligned memory access
  ([7.2](07-runtime.md#72-instance-memory)), therefore, an unaligned field is read and written as if it were aligned. Alignment describes the layout of values; objects with such fields remain accessible through the VM.

There are no bit fields: flags are constants and operators `|`, `&`, `<<`.

## 2.7 Enumerations

```
enum Key: UByte {
    None,          // 0
    Escape,        // 1
    F1 = 0x70,     // 0x70
    F2,            // 0x71
}
```

- The base type is an integer (`Byte` … `ULong`). Size and alignment – as defined by the type itself.
- An element without `=` is equal to the previous one plus 1, with the first element being 0. The value after `=` represents a constant expression; the count continues from this.
- Errors: the value does not fit within the base type (including values obtained automatically: `A = 255, B` for `UByte`); two elements with the same value; two elements with the same name.
- The element must be written with the type name: `Key.Escape`. If it lacks the type name (`Escape`), it’s an error.
- Conversion to the base type and back – only through `as`.  Reverse conversion is not checked: a value may end up in the `Key` variable that is not present in the list.
- The values of `enum` are compared using `==`, `!=`, `<`, `<=`, `>`, `>=` (as a basic type). There is no arithmetic operations on them.

## 2.8 Function Type

```
(a: Word, b: Word): Word
(): Void
```

- The marker – a signature without a function name, **with parameter names**. These names serve as documentation and do not affect the type: `(x: Word, y: Word): Word` and `(a: Word, b: Word): Word` are considered the same type.
- The value is a **function descriptor**, 8 / 8: the function number within the image,
  ([7.1](07-runtime.md#71-image-and-instance), not the code address in memory.
  You cannot read or modify the code through it.
- Function types are non-null by default; write `((args): Result)?` for an optional callback. Calling a nullable value requires narrowing or checked conversion; malformed null descriptors still cause a runtime trap.
- Calling via value checks the VM: if the descriptor does not point to a function, or if the function has a different type (e.g., a value obtained via `as` from a number), this is a trap, not a jump to arbitrary code.
- The function name is already a value of this type, without `&`: `let f: Action = foo`.
  This applies to `extern`-functions as well: the value can point to
  a native function.
- The value can be passed to a native function and received from it: native
  code can call it as a callback ([8.4](08-interop.md#84-calling-m-functions)).
- The result type is the entire type after `:`: `(): UByte[]` is
  a function returning an array. An array of functions requires parentheses,
  `((): Void)[]`, or a type alias.
- The value of this type provides a function name (including nested and `extern` functions), a function declared using `let mut`, and a function literal ([3.8](03-declarations.md#38-nested-functions-and-literals)).

The final parameter `args: ...` makes the function's type variable. This marker is considered when comparing types; the package rules and argument parsing – in [3.4](03-declarations.md#variable-parameters).

## 2.9 Aliases

```
type OpFunc = (left: Word, right: Word): Word
type Bytes  = UByte[]
```

- Alias – another name of the same type: `as` between them is not required.
- It is only declared at the top level. Using a pseudonym that refers to itself (`type A = *A`) is an error. Using `packed` with a pseudonym is also an error.
- “There isn’t a ‘new type’ that is compatible with the original.”

## 2.10 Compatibility and Implicit Conversions

Two types are considered equal if:

- this is the same struct or one `enum` (by declaration).
- these are instances of a single generic struct with canonically matching type arguments, for example, `Box<Word>` and `Box<Word>`.
- this is a scalar with a single name.
- this is the same built-in type, such as `String`, `Thread`, or `Type`.
- this is `Void` (in result types and descriptors `typeof`).
- these are references with the same access rights and matching content types.
- These are arrays with matching element types, regardless of length.
- these are function types with matching parameter types (in order), along with mutability of parameters (`mut T[]`), a return value, and variability.
- One of them is a pseudonym for the other.

There are three types of implicit conversions. Reading through a reference is applied only in the context of a value and does not alter the type matching rules: All of them only take away rights or do not change the representation at all.

| Source | Destination | Where |
|---|---|---|
| integer literal, `sizeof`, `alignof`, `offsetof` | any suitable type | everywhere where the type is known ([4.2](04-expressions.md#42-literals-and-constant-expressions)) |
| `@mut T` | `@T` | Everywhere; write permissions are removed, but the content type is preserved. |
| `@T`, `@mut T` | `T` | Context for a value, access to fields/elements, and operations on its content; single level, with `null` check. |

Everything else – through `as`
([4.6](04-expressions.md#46-casting-as)).

## 2.11 Dimensions

| Type | `sizeof` | `alignof` |
|---|---|---|
| `Byte`, `UByte`, `Bool` | 1 | 1 |
| `Half`, `UHalf` | 2 | 2 |
| `Word`, `UWord`, `Float` | 4 | 4 |
| `Long`, `ULong`, `Double`, `String`, `Thread`, `Type`, `T[]`, `@T`, `@mut T`, functions | 8 | 8 |
| `enum` | Similar to a basic type | Similar to a basic type |
| struct | according to layout (2.6) | according to layout |

mvm determines how values are passed between functions within a program:
This is not visible to either the program or native code. Structures are passed by value through the native ABI and export API ([8.2](08-interop.md#82-types-at-the-boundary)).

## 2.12 Strings

`String` is a separate reference type. The content of a string is immutable and contains valid UTF-8, excluding `U+0000`. An empty string is represented as `""`.
`null` indicates the absence of a string and is distinct from `""`.

```m
let original: String = "café"
let mut text: String = original
text += "!"
let length: UWord = length(text)    // 5
let letter: UWord = text[3]      // 0xE9
```

- Assignment, passing, and returning by reference are used. In the example, `original` stores the value `"café"`. The `+=` operator assigns `text` the result of concatenation.
- `length(s): UWord` – the number of Unicode code points; `s[i]: UWord` – a code point starting from index 0. A code point does not necessarily correspond to a displayed character: combined characters are treated separately. The index must be less than `length(s)`, otherwise – a trap. If the length does not fit into `UWord`, `length(s)` returns a trap `string`.
- The elements within the string are immutable, including for `let mut s: String`.
- `@s` creates an object with a string descriptor within it, while maintaining a reference to the same text.
- The `+` operator concatenates two strings; `==` and `!=` compare content.
- Unicode normalization is not present: different character sequences can render the same way while remaining unequal.
- There are no implicit conversions between `String`, numbers, references `@T`, and byte arrays; nor is `as` defined between these types. Encoding, decoding, and formatting are handled by the library.
- References to dynamic strings retain GC objects; literals are available throughout the lifetime of the instance. `sizeof(String)` is 8, representing the size of the reference, not the text itself. The size and structure of the string object are hidden from the program.
- `String` is non-null; `String?` explicitly permits absence. Comparing a nullable string to `null` does not access its contents, while `length`, indexing, and concatenation require a present value. A `String?` field may be omitted and defaults to `null`; a `String` field requires an initializer.

The syntax of `length(s)` and string operators is part of the language draft;
in mvm, these are provided by `STRING_LEN`, `STRING_GET`, `CONCAT`, `EQ`, and `NE`.
Its representation in the current C ABI is described separately in [gc.md](../gc.md).

## 2.13 Threads

`Thread` is a separate, opaque reference type. The value belongs to the overall runtime instance, where the OS-worker is created, and is not passed to another instance. Size and alignment of the handle – 8 / 8; representation is hidden.

- `Thread` is non-null; `Thread?` represents an optional handle. `join`, `detach`, and `interrupt` require a present handle, and malformed null values still cause a `thread` trap.
- Assignment copies the handle and preserves identity. `==` and `!=` compare identity; ordering and arithmetic are prohibited.
- Handles can be stored in fields and arrays; GC manages them in the usual way.
Releasing the last handle disconnects the worker without waiting for it.
- Thread creation and operations are provided by the
  [`std/thread.m`](07-runtime.md#threads) module, rather than built-in syntax.

## 2.14 Type Descriptors

`Type` – a built-in value type that describes complete static types of M.

The built-in operation `typeof(T)` or `typeof(expression)` returns a type descriptor. Its result has the `Type` type and is known at compile time.

```m
type Point {
    x: Word,
    y: Word,
}
type Position = Point

let p: Point = { .x = 1, .y = 2 }
let structureType: Type = typeof(Point)
let variableType: Type = typeof(p)
let same: Bool = structureType == variableType // true
let alias: Bool = typeof(Position) == typeof(Point) // true

let cell: @mut Point = @p
let view: @Point = cell
let referenceType: Type = typeof(view) // @Point descriptor, not Point.
let different: Bool = typeof(cell) != typeof(view) // true
```

- The size and alignment of the `Type` value – 8 / 8. Representation is hidden:
  the descriptor is not a program address, number, or string.
  Type metadata exists throughout the instance's lifetime and does not require
  allocation of a separate GC object for each `typeof` result.
- The `Type` can be assigned to a variable, parameter, function result, field, array element, and the content of a reference. Assignment, passing, and return copy the descriptor; `let mut` allows replacing it with another `Type` value, but it does not allow modifying the type metadata.
- The value of `Type` as `typeof(Void)` is used for:
  omitted fields in structs, uninitialized array elements, and global variables without an initializer.  A local variable `let mut t: Type` without an initializer also receives `typeof(Void)`.  Using `null` for `Type` is invalid. `typeof(Void)` is a complete type descriptor, not a missing or invalid descriptor.
- `==` and `!=` compare the described types using the matching rules in
  [2.10](#210-compatibility-and-implicit-conversions) and return `Bool`.
  Aliases produce the same descriptor. Different struct or `enum` declarations,
  including identically named declarations in different modules, produce different
  descriptors even when their layouts match. Generic struct instances are
  distinguished by their original declaration and canonical type arguments (2.15).
- `T`, `@T` and `@mut T` have distinct descriptors. Arrays consider the full type of their elements, but not their length; for functions, the parameter types, the modification rights for parameters (`mut T[]`), the return value, and variability are considered, but not the parameter names. The mutability of a variable (`let`/`let mut`) is not inherently included in its type.
- `typeof(expr)` determines the type of an expression before an implicit read of a reference or a write restriction caused by external context. The expression itself is not evaluated: calls, allocations, reading the contents of references, and boundary checks are not performed. However, names, types, and the validity of operations are checked in the usual way. Details and limitations can be found at [7.4](07-runtime.md#typeof).
- `typeof(Type)` describes the intrinsic type `Type`.
  If `t: Type`, then `typeof(t) == typeof(Type)`, regardless of
  the value stored in `t`. Therefore, `sizeof(t)` and
  `sizeof(typeof(Point))` are equal to 8: this is the size of the descriptor.
- The `Type` value cannot be used instead of a type: `let x: t`, `value as t`, and `type Alias = t` when `t: Type` are errors, even if `t` is a constant. The type name itself is not a value: to get a descriptor, you write `typeof(Point)`, not `Point`.
- Sorting, arithmetic, bitwise operations, and descriptor conversions to another type via `as` are not defined. `Type` does not grant the right to read arbitrary memory, change the value's type, or create types at runtime. Operations to retrieve the name, list of fields, and other content of a descriptor are not defined in this draft.
- Descriptors belong to an instance. Their internal numbers cannot be stored as stable type identifiers between images, or passed to another instance; boundary rules – [8.2](08-interop.md#82-types-at-the-boundary).

This is the target contract M. The current `MVM_KIND_TYPE`, `TYPEOF`, `IS`, and `LIT_TYPE` only differentiate between categories of `mvm_kind_t`, not the full types of M.

A separate `type_id` for a typed object describes the layout and GC map; however, it does not currently provide full type identity, as described above.

Examples can be found at [reflection.m](../../../mvm-c/examples/reflection.m).

## 2.15 Generic Types and Functions

Generic structures, aliases, and functions declare parameter types through `<...>`.

```m
type Box<T> {
    value: T,
}

type Items<T> = T[]

let identity<T>(value: T): T {
    return value
}
```

A specific type is represented as `Box<Word>` or `Items<String>`. The instantiation of a generic function is represented using `::<...>`:

```m
let box: Box<Word> = { .value = identity::<Word>(42) }
let operation: (value: Word): Word = identity::<Word>
```

`::` separates type arguments from the `<` and `>` operators in expressions. In the type position, only `<...>` is used. Nested lists like `Pair<Word, Items<String>>` are written without spaces between the closing `>`.

- Type arguments are mandatory, and their number must match the declaration. There is no output of arguments from parameters or expected results.
- The parameter `T` is a syntax type, not a `Type` value. You cannot substitute the value of the variable `t: Type` as `Box<t>`.
- Each instance is calculated during compilation. After the function body and fields are substituted, they are checked according to standard rules; for example, `increment::<Word>` is valid, while `increment::<String>` with a body like `value + 1` is an error.
- `Box<Word>` and `Box<String>` are distinct nominal types. Reusing an instance with canonically the same arguments uses the same definition and the same `Type`. Arguments are resolved before comparison.
- Generic alias, just like a regular one, doesn't create a new type: `Items<Word>` – is the same type as `Word[]`.
- `sizeof`, `typeof`, layout and GC metadata are calculated after substitution.
  Inside `identity::<Word>`, the expression `typeof(T)` means `typeof(Word)`.
- Recursion via reference is permitted: `next: @Node<T>` within `Node<T>`. Recursion by value is prohibited. Generating an infinite number of new instances is a compilation error, not a compiler memory overflow.
- Generic `enum`s, global variables, `extern` functions, `let mut` functions, nested functions, and functional literals with type parameters are not supported. A specific generic type is permitted in the `extern` signature according to standard ABI rules.
- The generic function is undefined and not exported by the executor.
- For the host API, specific wrapper functions are declared. The templates themselves can be exported and imported between M-modules.

MIR, MO, and mvm only receive specific instances; there is no generic runtime representation. A detailed explanation can be found at [GENERICS.md](../../.hidden/GENERICS.md), and an executable example is available at [generics.m](../../../mvm-c/examples/generics.m).


## 2.16 Union Types

`A | B` contains a value that is exactly one of the specified types. Union structures: the order of variants does not affect identity; nested unions are flattened; aliases are resolved; duplicates are removed. `Word | Word` is normalized to `Word`. `Void` and recursive aliases are prohibited.

```m
type Value = Word | String
let mut value: Value = 42
value = "answer"
switch variantof(value) {
    case typeof(Word): {
        let number: Word = value as Word
        break
    }
    case typeof(String): {
        let text: String = value as String
        break
    }
}
```

The exact type match is selected first. Otherwise, only the single option with implicit conversion M is allowed: `Word | Long = 42` is ambiguous; `42 as Word` specifies a single variant. The literal `null` also requires a single nullable variant. A typed `String?` with a value of `null` maintains the `String?` variant and its full type descriptor.

`U` is implicitly expanded to `V` if all possible variants of `U` are precisely contained within `V`.

The union type immediately following `as` performs grouping: `value as (Word | String)`, ensuring that `value as Word | mask` maintains the meaning of a bitwise expression.

Extracting `u as T` requires a separate variant `T` and performs validation during execution. Mismatches result in a runtime error. Arithmetic operations, comparisons, function calls, indexing, and field access all require a preceding extraction.

`variantof(expression)` evaluates its operand exactly once and returns the `Type`
of the active variant. For an ordinary value, it returns the complete static type
after evaluating the expression. Explicit references are not read automatically:
`variantof(r)` for `r: @U` returns `typeof(@U)`. `typeof(expression)` retains the
static type of the entire union and does not evaluate the expression.

A union consisting of multiple variants occupies 8 bytes with padding and represents a persistent GC-wrapped entity. Copying a union separates the wrapping; its identity is not accessible to the program. When a struct is packed and unpacked, it is copied by value; embedded arrays and explicit references maintain shared objects.

The union does not have a default value. Variables, including `let mut`, require an initializer. This rule applies to structures with fields initialized with a default value; the field cannot be omitted in a literal. Operations like `U[] = []`, `arrayPush`, `arrayInsert`, `arrayReserve`, and element deletion are permitted; however, creating `U[n]` and `arrayResize` with an uninitialized element are currently prohibited. `@U` and `U[]` are non-null by default; use `@U?` or `U[]?` for intentional absence.

A union within mutable containers is invariant: a `Word[]` does not become `(Word | String)[]`, but `@mut Word` becomes `@mut (Word | String)`. Unions are permitted in specific generic types and internal signatures of M, including indirect calls. Native/host ABI and variadic union packs are not supported, including nested paths through arrays, references, structs, and callback signatures.

Rationale and Implementation Details: [UNION.md](../../.hidden/UNION.md).
Executable Example: [union.m](../../../mvm-c/examples/union.m).
