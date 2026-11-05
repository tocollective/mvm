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

# M language drafts

Working drafts of the syntax, types, and runtime.

M is a statically typed language for the **mvm** virtual machine.
The compiler translates a program into an mvm image; the virtual machine
loads the image, links native functions, and executes the program.
Build programs with `mvm build` and run them with `mvm run`.

Types follow names; variables are declared with `let` and
`let mut`, and exports are explicit. Structs are values with a fixed
layout. `@T` and `@mut T` are typed references to GC objects;
`@value` creates a new object containing a copy of the value; a reference is transparently
used in operations on its contents. Arrays `T[]` are always dynamic and passed by reference.
Reference values occupy 8 bytes; their internal representation is hidden.
[`Type`](02-types.md#214-type-descriptors) is an opaque descriptor of a complete static type;
`typeof(Point)` and `typeof(value)` return such values without
evaluating the operand. Descriptors can be stored, passed, and compared.
Examples: [reflection.m](../../../mvm-c/examples/reflection.m).

A program operates on its instance's values and native functions
provided by the mvm runtime. An access error, invalid
index, or call to a nonexistent function stops the current call
with a diagnostic.

The drafts describe the target language contract. The current C implementation of
mvm is described in [GC and arrays](../gc.md) and [functions](../functions.md); its RAM, references, and
native ABI do not yet implement all draft guarantees. The division between
built-in operations and the planned standard library is described in
[native library](../native-library.md).

```
extern let log(msg: String): Void       // native function provided by the mvm runtime

type Color {
    r: UByte,
    g: UByte,
    b: UByte,
}

/// The executor calls this function by name.
let colorMix(a: @Color, b: @Color, out: @mut Color): Void {
    out = { .r = a.r + b.r, .g = a.g + b.g, .b = a.b + b.b }
}
export { colorMix }

let main(args: String[]): Word {
    for i: UWord in 0..length(args) log(args[i])
    return 0
}
```

## Contents

| Section | Topics |
|---|---|
| [1. Lexical structure](01-lexical.md) | source text, comments, identifiers, reserved words, literals, tokens |
| [2. Types](02-types.md) | integers, `Bool`, `Float`, `Double`, `String`, `Type` and `typeof`, references `@T` and `@mut T`, arrays, structs and their layout, `enum`, function types, aliases, generics and monomorphization, implicit conversions |
| [3. Declarations](03-declarations.md) | variables, global variables, functions, `main`, `extern`, visibility and order, nested functions and function literals |
| [4. Expressions](04-expressions.md) | precedence, literals and constants, calls, indexing, fields, `as`, wrapping arithmetic, shifts, comparisons |
| [5. Statements](05-statements.md) | assignment, `if` without parentheses, `while`, `for` with ranges and array/string iteration, `break`/`continue`, `switch`, `return` |
| [6. Modules](06-modules.md) | `import`, `export`, file lookup, program and image |
| [7. mvm runtime](07-runtime.md) | image and instance, memory and GC, traps, built-in functions, `mvm run` |
| [8. mvm interoperability](08-interop.md) | loading and execution, native functions, calls through the mvm API, boundary types, memory, prelude, limits |
| [9. Errors and warnings](09-diagnostics.md) | summary of everything the compiler and mvm reject or warn about |
| [10. Grammar](10-grammar.md) | EBNF |

## Reading the drafts

- **Error** — the compiler rejects the program. **Warning** —
  it reports the issue but compiles. **Instantiation error** — the image
  is built, but the executor configuration is unsuitable. **Trap** — the current
  call stops at runtime and control returns to the executor.
- **Executor** — the mvm component that loads the image, links
  native functions, manages the instance, and calls program functions.
  `mvm run` uses the executor to launch a program from the command line.
- Examples in the sections are fragments illustrating the target syntax.
  [examples/](../../../mvm-c/examples/) contains working program sketches;
  the presence of an example does not mean the compiler and all its libraries
  have already been implemented.
- [gc.md](../gc.md), [functions.md](../functions.md), and
  [OPSLEFT.md](../../.hidden/OPSLEFT.md) describe the existing C code and VM tasks.
  [native-library.md](../native-library.md) describes planned
  library contracts and current native ABI limitations.

## Dynamic arrays

All arrays are dynamic and have type `T[]`. The `[]` literal creates
an empty array, while `let mut a: T[n] = []` creates a new array
with initial length `n` and zero-valued elements; the variable's type is `T[]`.
Indexing, `length`, `arrayCapacity`, `arrayReserve`, `arrayResize`,
`arrayPush`, `arrayPop`, `arrayInsert`, and `arrayRemove` are available. Assignment copies the reference; growth
preserves object identity. Length is not part of the type.
Semantics: [2.5](02-types.md#25-arrays); built-in operations:
[7.4](07-runtime.md#dynamic-array-operations); examples:
[examples/arrays.m](../../../mvm-c/examples/arrays.m).

mvm implements the operations through the C API and bytecode. There is no M compiler yet;
`.m` examples describe the language contract rather than executable tests.
Explicit references `@T`/`@mut T` and object creation with `@value` are described
in [2.4](02-types.md#24-references); examples:
[references.m](../../../mvm-c/examples/references.m).

The [std/thread.m](../../../mvm-c/examples/std/thread.m) module describes the implemented
C thread runtime ([thread.md](../thread.md)). Library modules for buffers,
string conversions, and bit operations still need to be implemented.

## License

Copyright 2026 tocollective contributors.

Licensed under the Apache License, Version 2.0. See [LICENSE](../../LICENSE) for the full license text.

Unless required by applicable law or agreed to in writing, software distributed
under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied. See the License for the
specific language governing permissions and limitations under the License.
