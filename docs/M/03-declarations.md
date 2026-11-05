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

# 3. Declarations

[← Types](02-types.md) · [Table of Contents](README.md) · [Expressions →](04-expressions.md)

## 3.1 File Structure

File – a sequence of top-level declarations:

| Declaration | Example | Section |
|---|---|---|
| Import | `import { puts } from "externs.m"` | [6](06-modules.md) |
| Export | `export { colorRed, Color }` | [6](06-modules.md) |
| Structure | `type Color { r: UByte, g: UByte, b: UByte }` | [2.6](02-types.md#26-structures) |
| Alias | `type OpFunc = (a: Word, b: Word): Word` | [2.9](02-types.md#29-aliases) |
| Enumeration | `enum Cause: UWord { ... }` | [2.7](02-types.md#27-enumerations) |
| Global Variable | `let PAGE_SIZE: UWord = 4096` | 3.3 |
| function | `let add(a: Word, b: Word): Word { ... }` | 3.4 |
| Function variable | `let mut onKey(key: UByte): Void { ... }` | 3.8 |
| Native Function | `extern let log(msg: String): Void` | 3.5 |

`let` introduces both variables and functions: after the name comes a `:` which indicates a variable, and a `(` which indicates a function. `let mut` and `(` after the name represent a function variable along with its body, which it initially points to (3.8).

## 3.2 Variables

```
let x: T = expression        // immutable
let mut x: T = expression    // mutable
let mut x: T                // Modifiable, without an initial value.
```

- The type must always be specified. `let x = 5` – is an error.
- `let` without `mut` cannot be modified after declaration: neither the variable itself, nor its fields and elements. For a reference value, the right to access the object is determined by the type: `let r: @mut T` allows modification of the content through `r` (2.4).
- `let mut` can be modified entirely and in parts.
- **`let mut x: T` without `=`** is valid only for a type with a zero value.
  Unions and structs containing union fields by value require an initializer
  ([2.16](02-types.md#216-union-types)). For other types, this declares a variable
  without an initial value. Local numeric fields may contain data left by previous
  calls. The VM always initializes reference fields to `null` before a possible
  collection so GC does not traverse garbage; `Type` values, including fields,
  are initialized to `typeof(Void)`. This form is useful for buffers that will
  be overwritten anyway. Reading before the first write produces a warning
  when the compiler can detect it.
- **`let x: T` without `=` or `mut`** – error: the variable will never have a value.
- A structure literal with missing fields fills them with zero values; fields without default values cannot be omitted. Examples: `{ .b = 0xFF }`, `{}`
- **`T[]` — array type.** A literal creates a new object from the specified elements, while assigning another array copies the reference. It can be initialized by the result of a function that returns `T[]`.

  ```m
  let mut bar: String[] = ["one", "two"]
  arrayPush(bar, "three")
  for i: UWord in 0..length(bar) { println(bar[i]) }
  ```

  Without an initializer, `let mut bar: String[]` is invalid because a non-null array has no default value. Use `String[]?` for intentional absence or `= []` for a present empty array.

- A local `let` of a scalar type or `Type` with a constant initializer – is a **constant** ([4.2](04-expressions.md#42-literals-and-constant-expressions)):
  The compiler can substitute the value directly into the code without needing a separate slot in
  the frame. The `case` or `by` labels require a type that is suitable for them; `Type` is not suitable.

  ```m
  let step: UWord = 2
  for i: UWord in 0..16 by step { println(toString(i as Long)) }
  ```

### Array with Initial Length

The `T[n]` form requires a default value for `T` only when elements are omitted. Non-null reference-like elements are allowed when the literal supplies all `n` values; otherwise use nullable elements or the standard `T[]` literal with explicit additions. Unions and structures containing non-defaultable fields cannot be created implicitly.

```m
let n: UWord = 5
let mut arr: Word[n] = []       // n zero-initialized elements; type `Word[]`.
let head: Word[n] = [10, 20]    // The first two are defined, the rest are zeros.
let empty: Word[0] = []         // New empty array.
let values: Word[] = [1, 2]     // Initial length based on the number of elements: 2
```

`T[n]` in this declaration defines the **initial length**, not the type or capacity. After creation, the array has a standard `T[]` type, and its length can be modified. For example, `arr = []` then replaces the link with an empty array; the previous `n` is no longer used.

- Writing is permitted only within a variable declaration (`let`/`let mut`), including global ones. After `[n]` you must include `=` with an array literal: `[]` or `[v1, v2, ...]`. To copy a reference, use `T[] = other`; `T[n] = other` is an error, as it prevents modification of another array.
- `n` is any unsigned integer type; an untyped literal or constant expression receives the `UWord` context. Locally, `n` can be calculated at runtime and equal zero.
- Initially, `n` is calculated only once. Before allocating and calculating elements, the range `0..4_294_967_295` (inclusive) and the condition “the number of literal elements is not greater than `n`” are checked. Violation results in a `memory` trap; if it’s known at compile time, it results in an error.
- Then, the VM fills the array from left to right. Omitted elements receive their defaults; a fully initialized non-defaultable array is created empty and populated by pushes, so no invalid null element becomes observable. The array and calculated references are held by the GC until completion. Failure to allocate results in `allocation`; a partial result is not assigned to the variable.
- For fields of structures, parameters, function results, aliases, and in operations on types (`sizeof`, `as`, etc.), only `T[]` is used. For example, `let f(a: Word[n]): Void` and `type Buffer = UByte[64]` are incorrect. The initial size of an object is not part of its signature.
- Only one size is permitted—the last suffix of the declaration. For example,
  `let mut rows: Word[]?[n] = []` creates `n` nullable references to `Word[]`, initially
  equal to `null`. The type of the variable is `Word[]?[]`. Non-null inner arrays must be supplied explicitly. `Word[n][m]` is not allowed:
  nested arrays are created separately to explicitly define their lengths and
  avoid implicit concatenation of strings.
- The function type is grouped within parentheses or given a nickname as an element. For example: `let callbacks: ((): Void)[n] = []`.  An empty suffix after a non-grouped function type is prohibited to prevent confusion between creating an array of functions and describing a function's result.

This notation preserves `T[]` as a single type of array and uses `[n]` only when a new object is created. The size expression does not affect type comparison, assignment, or function calls.

## 3.3 Global Variables

Variable declared at the top level.

- **Initializer – only constant expressions**
  ([4.2](04-expressions.md#42-literals-and-constant-expressions)). When creating an instance, no M code is executed: mvm copies ready values and creates objects based on array and expression templates `@`. Each literal creates a separate array within each instance; references to another global variable-array preserve a common object within the instance. Calling a function within the initializer is an error. Reading the value of `let mut` is an error. The `typeof`, `sizeof`, or `alignof` operands are not evaluated and can contain calls and names (7.4). The size `[n]` in a global declaration must also be constant: it is preserved in the template for the created array.
- The function name and function literal are constant descriptors. `@value` with constant content defines a template for a specific GC object: each instance receives its own object. `@global` copies the initial value of the global constant, rather than linking to its slot.

  ```
  let current: @mut Color = @{ .g = 0xFF }
  let currentView: @Color = current
  let onTick: (): Void = tick
  ```

- **Global `let mut X: T` without `=` requires a defaultable type** and is initialized to zero when an instance is created. Nullable reference-like values default to `null`; non-null ones require an explicit initializer. For `Type`, the zero value is `typeof(Void)`.
- A global `let` without `mut` and without `=` is an error, just like a local one.
- If a program needs to perform some calculation before execution, it exports a initialization function to be called from `main` or via the mvm API ([8.4](08-interop.md#84-calling-m-functions)).

Where does the compiler place global variables
([7.2](07-runtime.md#72-instance-memory)):

| Declaration | Segment |
|---|---|
| `let X: T = constant` | `rodata` (read-only); if `X` is not exported, its value is inserted into the code, and there may not be space in memory. |
| `let mut X: T = constant`, at least one byte is nonzero | `data` |
| `let mut X: T = constant`, all bytes are zero, or without `=` | `bss`: not stored in the image |
| `let mut f(...): R { ... }` | The variable is `data`, and the body is a function for an image. |

## 3.4 Functions

```
let colorMix(a: Color, b: Color): Color {
    let c: Color = { .r = a.r | b.r, .g = a.g | b.g, .b = a.b | b.b }
    return c
}
```

- The result type is always specified, including `Void`.
- Parameters are separated by commas, with the optional final comma. Parameter names are unique.
- **Parameters are immutable**, similar to `let`: their value cannot be replaced.
  To modify a local value, it is copied to `let mut`. The parameter `mut T[]` separately allows modification of the array object.
- The parameter `T[]` receives a reference to a dynamic array for reading,
  `mut T[]` – with the ability to modify its elements and length. The mutable parameter
  accepts only a mutable path of access to the array (2.5).
- Structures are passed and returned by value. The exact method is determined by the mvm ([2.11](02-types.md#211-dimensions)). An exception exists for the boundary: for `extern` functions and functions visible to the executor, only the types listed in the boundary contract ([8.2](08-interop.md#82-types-at-the-boundary)) are allowed.
- **`return`**: In a function returning a value of type `T`, use `return expression` of type `T`. In `Void` functions, use `return` with or without a `Void` expression. If a function with a return value can reach a `}` without a `return`, this is an error ([5.10](05-statements.md#510-return)).
- The function can be declared and defined within another function, as well as written using a literal (3.8).
- Recursion is allowed.
- A top-level function without `mut` can have type parameters:
  `let identity<T>(value: T): T`. Its specific instance is selected via
  `identity::<Word>`. The rules can be found in [2.15 - Generic types and functions](02-types.md#215-generic-types-and-functions).
  Nested generic functions and generic literals are not supported.

### Variable Parameters

```
let sum(initial: Word, args: ...): Word {
    let mut total: Word = initial
    for i: ULong in 0..vaCount(args) total += vaArg(args, i, Word)
    return total
}

let example(): Word { return sum(10, 20, 30) }  // 60
```

- `name: ...` – a named batch of remaining arguments. This parameter always appears last and can be the only one. There can be any number of additional arguments, including zero.
- Additional arguments include integers, `Bool`, `Float`, `Double`, `String`, `Type`, `Thread`, `enum`, structs, unions, arrays, references `@T`/`@mut T`, and functions. `Void` is not allowed. The package preserves the complete type of the content and access rights for verification of `vaArg`. Regular parameters and results allow `T[]`.
- An integer literal receives the type `Word` if it fits, otherwise `Long`, otherwise `ULong`. If the first non-variadic parameter of a function is a function type, it defines the context for additional arguments and their count; this utilizes `std/thread.m`. A literal outside the range of the chosen type is an error. A floating-point literal without the `f` suffix receives `Double`, with `f` – `Float`. `null` without a context type among the additional arguments is an error: it is first assigned to a variable of an explicit reference type. The remaining values maintain their type; `Float` remains binary32.
- Each additional argument occupies one 8-byte cell within the package: integers, `Bool`, and `enum` are expanded to 64 bits based on their type, `Float` uses 32 bits in the lower half, `Double` uses 64 bits, references, strings, functions, and `Type` as descriptors. The type itself is not stored within the cell, but the VM separately stores information about references for GC. The package retains the passed objects until the original call completes; the package redirection preserves this information.
- `vaCount(args): ULong` returns the number of additional arguments.
  `vaArg(args, index, T): T` reads an argument at index `index` (any
  unsigned integer) starting from zero. `T` is one of the valid types
  for additional arguments, including `String` and `Type`. This is a read of a representation,
  not a numerical conversion; `vaArg(args, i, Float)` does not convert an integer
  to `Float`. Type and boundary checks are performed by the calling function:
  the index must be less than `vaCount(args)`, and the type must match
  the passed value. Integers allow reading the same representation with a different sign and truncation to the least significant bits. An index outside the range — a trap.
- The package is immutable and persists until the original call returns. It cannot be declared as a regular variable, field, result, or type alias. Passing the package as the **sole additional argument** redirects it without copying: `sum(0, args)`. Mixing the package with other additional arguments is not allowed.
- Flexibility – a part of function types:
  `(initial: Word, args: ...): Word` differs from `(initial: Word): Word`.
  Nested functions, literals, and variable functions are supported.
- A package is a collection of cells, their count, and auxiliary information about references;
it is not equivalent to C `...` or `va_list`. The executor receives it through a VM adapter,
which maintains the roots ([8.2](08-interop.md#82-types-at-the-boundary)).
The standard C ABI `NATIVE` of the current implementation does not currently support such a package. `vaCount` and `vaArg` remain as built-in operations:
the language lacks a generic type for heterogeneous arrays of arguments.

### `main`

```
let main(args: String[]): Word { ... }
let main(): Word { ... }
```

- To call exported functions of the `main` image, the mvm executor does not require:

It can call any exported function from the root file.
([8.4](08-interop.md#84-calling-m-functions))
- `main` is needed to run the program using the `mvm run` command
  ([7.6](07-runtime.md#76-mvm-run)). This is a standard function that is called with command-line arguments, and its result becomes the exit code.
- `args[0]` contains the path to the program, and the remaining elements are its arguments.
  The number of elements is available through `length(args)`.
- If arguments are not needed, the parameters can be omitted: `let main(): Word`.
  There are no other signatures.
- **The root file's `main` is exported automatically**; `export { main }`
  is unnecessary (it is allowed and changes nothing). This is the only exception
  to explicit export ([6.2](06-modules.md#62-export)). In other files, `main` is an ordinary name.

## 3.5 `extern`

Declares a native function: it has no body on M, providing its implementation via a native library or the mvm runtime environment ([8.3](08-interop.md#83-native-functions)).

```
extern let log(msg: String): Void
extern let now(): ULong
extern let format(fmt: String, args: ...): String
extern let threadSpawn<F>(entry: F, arguments: ...): Thread
```

Generic `extern` instances according to the same rules as a generic function:

`threadSpawn::<(value: Long): Long>` creates an announcement with a specific signature.

Polymorphic built-ins within the virtual environment can associate such signatures with a single implementation.

- The executor finds a function by its name from the declaration. During import, the name can be changed using `as` ([6.3](06-modules.md#63-import)), but only within the importing file: the executor sees the original name.
- Supported parameter and result types – only those that pass through the boundary with the executor ([8.2](08-interop.md#82-types-at-the-boundary)): scalars, `String`, `Thread`, `Type`, `enum`, dynamic arrays, references, functions, and `Void` as a result.
- Value-based structures – an error; use `@T`/`@mut T`.
- `extern` with `args: ...` receives a batch of cells (3.4).
- A single `extern` name across different files in a program refers to the same native function. The types of parameters and the return type in the declarations must match; otherwise, an error occurs. Polymorphic built-ins are an exception: the environment explicitly allows them to have different concrete signatures and checks the values itself.
- Does this function exist within the `executor`? The compiler doesn't know. If the `executor` doesn't provide it, or provides it with different types – this is a **instance creation error**, not a compilation error ([8.1](08-interop.md#81-from-source-to-call)).
- `extern` declarations can be exported: the importing file will receive the declaration ([6.2](06-modules.md#62-export)).  Furthermore, declarations for native functions are collected in a single file.
- There are no external variables. Data is exchanged with the executor through functions and instance memory ([8.5](08-interop.md#85-accessing-objects-and-values).

## 3.6 Type Declarations

`type`, `packed type`, `type ... =` and `enum` are described in
[section 2](02-types.md). All of them are described at a high level.

## 3.7 Scope and Order

- **Top level – in any order.** The function can call a function below, a field can have a type declared below, and a constant can refer to a constant below. There are no preliminary declarations.
- Top-level errors:
  - A loop within initializers (`let A: UWord = B`, `let B: UWord = A`).
  - A structure that contains itself by value.
  - Alias that refers to itself.
- **Top-level names in the file must be unique**, along with imported ones:
  Importing `mix` and declaring your own `mix` – an error.
- **Inside the function – in order:** a local variable is visible from its declaration until the end of the `{ }` block in which it was declared.
- Each block, loop body, and `case`-block has its own scope.
  The `for` variable is only visible within the loop body.
- **Shadowing works as in C.** A name in a nested block hides an outer name
  until the end of the block: a local variable, parameter, nested function, or
  top-level name (global variable, function, type, or imported name).
  The outer name is still visible in the initializer:

  ```
  let size: UWord = 4
  if wide {
      let size: UWord = size * 2  // Right – external size; this – up to ‘}’
      use(size)                   // 8
  }
  use(size)                       // 4
  ```

- **In a block, a name must be unique:** two identical names within a single block –
  an error. Parameters and the function body constitute a single block, similar to C: a local variable with a parameter name can only be declared within a nested block. After the end of the block, the name becomes available:

  ```
  for i: UWord in 0..n { ... }
  for i: UWord in 0..m { ... }    // It is possible: the first `i` is no longer visible.
  ```

- Shadowing a local variable, a parameter, or a nested function –
  **Warning** (8.1): it’s very easy to confuse two variables.
  Shadowing a top-level name – without a warning.
- Shadowing is possible for both type and name: from the beginning of a block to the end, and not to write the type as `let c: Color` (error), but `sizeof(Color)` – the size of the variable ([7.4](07-runtime.md#sizeof-alignof-offsetof)), as in C.
- **Nested functions and literals** do not see local variables and parameters of the outer function (3.8), so their parameters and variables can be named the same way, without warnings. If the outer function shadows a global name, the global name is visible within.

## 3.8 Nested Functions and Literals

The function can be declared within another function in three ways:

```
let main(args: String[]): Word {
    let n: UWord = 10

    // nested function
    let square(n: UWord): UWord {   // It is possible: the external `n` here is not visible.
        return n * n
    }

    // variable function: `let mut func: ()::Void` and the body it points to.
    let mut func(): Void {
        puts("foo\n")
    }
    func()                          // "foo"
    func = bar
    func()                          // "bar"

    // function literal
    func = (): Void { puts("baz\n") }
    onEvent("resize", (code: UWord): Void {
        puts("resized\n")
    })

    return square(n) as Word
}
```

### No Capture

**A nested function or function literal cannot see local variables or parameters
of the enclosing function.** The following names are visible inside:

- its own parameters and local variables
- All top-level: global variables, functions, types, `enum`, imported names – even if an external function shadows the name of its local (3.7);
- Nested functions (without `mut`), declared above in the encompassing blocks of outer functions, and the function itself – for recursion. This is not a capture: they do not need data from the outer function.

The following is not visible: local variables and parameters of external functions, including `let mut` variables (which are also local variables).

```
let mut count: UWord = 0
let bump(): Void {
    count++         // Error: 'count' is a local variable of 'main': a nested function cannot capture it.
}
```

Therefore, the value remains a simple descriptor of the function
([2.8](02-types.md#28-function-type)): it can be passed to the `executor` as
a callback or placed in a handler table. If the function needs data from the caller, it is passed as a regular parameter or via a link `@T`/`@mut T` with explicit context type ([2.4](02-types.md#24-references)).

A nested function within the runtime has no cost: it’s a regular function, whose name is only visible within the encompassing block.

### Nested Function

```
let name(parameters): R { body }
```

- Syntax and rules – as in the top-level function (3.4).
- **The variable name is visible from its declaration until the end of the block** (as in 3.7).

No scoping occurs: it’s not possible to call a nested function after its declaration, and two nested functions cannot call each other. To achieve this, functions are moved to a higher level.
- The name within its body is visible: recursion is allowed.
- The name – a local name for an external function; it cannot match any other visible names in the same scope (3.7).
- This is a function, not a variable: it cannot be assigned to, but it can be called directly.
- The value is passed through its descriptor.
- The `return`, `break`, and `continue` statements within the body apply to the deepest nested function: `break` does not exit the loop of the outer function.
- Nesting — to any depth.
- Exporting inner functions is not possible: `export` is only for top-level names.
- Before a nested function, you can use `///`.

### Variable Function: `let mut`

```
let mut name(parameters): R { body }
```

The same as

```
let body_name(parameters): R { body }      // The body name is nowhere visible.
let mut name: (parameters): R = body_name
```

- `name` is a **mutable variable** of function type: it can be called,
  assigned a different function, or assigned `null`. The expression `@name`
  creates a separate object with a copy of the current descriptor.
  Calls are indirect, through the variable's current value.
- **Inside the function**, this is a local variable. Its body is a nested function without a name, so `name` within the body is not visible – it’s the local name of the outer function, and there’s no capture. It has no recursion.
- **At the top level**, this is a global variable in `.data`, initially pointing
  to the body. Global names are always visible, so `name(...)` in the body calls
  the variable's **current** value: after replacement, it calls a different function.
  It is exported like any other global variable.

  ```
  let mut onKey(key: UByte): Void {   // default handler
      puts("unhandled key\n")
  }

  let main(args: String[]): Word {
      onKey(0x41)                     // "unhandled key"
      onKey = myKeyHandler
      onKey(0x41)                     // myKeyHandler
      return 0
  }
  ```

- If the variable is never assigned a different value, a warning is issued,
  as for any `let mut`: `let` would have been sufficient.
- `extern let mut name(...)` is not supported: `extern` is a native function only. If the executor needs to override the handler, the program exports the `let mut name(...)` variable, and the executor writes the function's value into it ([8.5](08-interop.md#85-accessing-objects-and-values)).

### Function Literal

```
(parameters): R { body }
```

- This expression represents a function type with its body appended. The literal type – is the type of function that is stored within it; the result type is always written, with no type output.
- A literal can be written anywhere where a value of a function type is needed:
  initializer, assignment, argument, `return`, element of an array or field of a literal struct.
- Inside, the rules are the same as those for a nested function, but there’s no name, and therefore no recursion.
- **Each literal is a separate function.** Two literals with the same text are different, and they are not equal using `==`.
- **Literal descriptor – a constant**, similar to a top-level function descriptor ([4.2](04-expressions.md#42-literals-and-constant-expressions)). Therefore, a literal can be written both within functions – in the initializer of a global variable:

  ```
  let handlers: Handler[2] = [
      (code: UWord): Void { puts("zero\n") },
      onOne,
  ]
  ```

- `(` at the beginning of an expression, followed by `)` or `name :`, indicates the start of a function literal; otherwise, it represents an expression within parentheses. This rule is the same one that distinguishes a function literal from expressions within parentheses ([10](10-grammar.md)).
- In the `if`, `while`, `for` and `switch` conditions, and within the `switch` expression, the function literal is written only within parentheses: `{` marks the beginning of the body.
- A literal on a separate line is an error, as is any expression that is not a call (5.1).
