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

# 7. mvm Execution Environment

[← Modules](06-modules.md) · [Table of Contents](README.md) · [Interaction with mvm →](08-interop.md)

What does the mvm virtual machine guarantee for a program on M? Where are the data located, and what checks are performed during execution? What built-in functions are available? How does the executor utilize this – [section 8](08-interop.md).

This is the target contract for the language and virtual machine. The current C implementation of mvm and its differences are described in [gc.md](../gc.md) and [functions.md](../functions.md); not all of the checks and APIs in this section are yet implemented.

## 7.1 Image and Instance

- **Image** – the result of compiling a program
([6.1](06-modules.md#61-file--module)): functions in mvm bytecode, initial content of the `rodata` and `data` segments, a string literal pool, metadata for complete types for descriptors `Type`, `bss` size, export table, import table (`extern`-functions with types), debugging information (files, lines, function names) and export doc-comments. The image is immutable. It can be saved to a `.mvm` file and then loaded without the source code.
- The image format and instruction set are described in the mvm documentation, not in this draft – they are independent of it.
- mvm **verifies the image upon loading**: bytecode cannot access memory except through verifiable instructions, or by jumping to the middle of another function or calling a function with incorrect arguments. Corrupted or manually assembled images are rejected, rather than executed.
- **Instance** – an image, with its own memory ([7.2](#72-instance-memory)) and native functions for all `extern`. Multiple instances can be created from a single image, and they do not share any memory.
- Creating an instance does not execute M code: mvm lays out the segments and
  creates objects from global dynamic-literal templates. These objects belong
  to the instance; allocation failure is an instantiation error.
- **Instance supports workers**: The main thread and each worker have their own stack and call state, but they share instance memory and natives. Different instances remain independent of each other, not sharing any resources.

## 7.2 Instance Memory

The instance possesses global values, a call stack, and a managed heap.
The program accesses these through variable names, struct fields,
array elements, and transparent access via references. Descriptor representation
and object placement are hidden from M.

| Area | Lifetime | Program Access |
|---|---|---|
| Constants and Global Values | Lifetime of Instance | According to `let`/`let mut` declaration |
| Local variables | Execution time of the call | By name and according to mutability |
| GC Objects | While Maintainable | Through Typed References and VM Operations |
| VM code and data | Throughout the instance's lifetime | Are not M data values |

- The numerical representation of values is little-endian. Unaligned fields in `packed` structures produce the same result as aligned (2.6).
- Each operation checks the type, whether it belongs to an instance, boundaries, and write permissions. An invalid reference, reading `null`, going out of array bounds, or writing without permission to modify – all result in a failed execution.
- `sizeof`, `alignof`, and `offsetof` describe the representation of a value, but do not provide access to the content of an object by a numerical offset.
- The memory limit includes globals, the stack, heap objects, and runtime
  metadata, including array-buffer capacity. The executor sets the limit.

### Data Stack

Local values and parameters belong to the call. The state of calls and VM auxiliary indices are not accessible to the program. Stack or call depth overflow results in `stack`.

`@local` creates a separate object with a copy of the value. Returning a reference from the function is safe: it maintains the object under GC, rather than a local location within a completed call.

### Heap

- Objects are created using `@expression`, array literals, variable declarations like `let a: T[n] = [...]`, string operations, and a verified executor API.
- GC releases unreachable objects and cycles. The basic implementation is a non-moving mark/sweep. Object identity is preserved regardless of its internal placement.
- Roots – these are live values in all calls, global values, and registered executor references. The VM stores metadata for reference fields, including fields within structs and `packed` values.
- The `@T`/`@mut T` object is traced by its content type `T`. References, strings, and arrays within the value maintain their corresponding objects. A raw byte buffer cannot replace a typed object.
- Assignment of a struct copies its fields by value. Assignment of a reference, string, or array copies the corresponding descriptor.
- The dynamic array `T[]` maintains a stable identity. Its buffer can be relocated as it grows; GC scans only the current length, considering the entire capacity in memory usage. Removed slots are cleared.
- New values are initialized before an object is published: numbers are zero, `Bool` is `false`, `Type` is `typeof(Void)`, nullable references, strings, arrays, and functions are `null`, and defaultable structures are recursively initialized by their fields. Non-defaultable values must be initialized explicitly; this does not change the rules for local variables without an initializer (3.2).
- `String` is immutable. The results of string operations are allocated in the heap; literals are allocated within the instance image. Operands are held during potential allocation and garbage collection.
- If there is insufficient memory, the VM performs garbage collection. If allocation is still impossible, the result is `allocation`; no partially initialized object is returned to the program.

This is the M contract. The existing C-style `REFERENCE`, RAM, and native ABI are not yet implementing all guarantees for typed references and limits. The state of the `executor` is described in [gc.md](../gc.md).

## 7.3 Traps

Trap – a program halt detected by the mvm during execution.

| Reason | When |
|---|---|
| `memory` | Invalid reference, attempting to read/write to `null`, out-of-bounds access, or permission violation |
| `allocation` | The VM cannot allocate an object after a build attempt: the memory limit has been exceeded, or the allocator failed. |
| `string` | Operation on a missing string, `null`, or invalid string passed to the executor |
| `call` | Calling `null`, an invalid function descriptor, or a function of a different type ([2.8](02-types.md#28-function-type)) |
| `stack` | Stack overflow, either due to excessive data usage or excessive call depth |
| `vaArg` | `vaArg` with an index greater than or equal to `vaCount` ([3.4](03-declarations.md#variable-parameters)) |
| `fuel` | Fuel has run out ([8.7](08-interop.md#87-limits-and-interrupts)) |
| `interrupt` | The executor interrupted execution ([8.7](08-interop.md#87-limits-and-interrupts) |
| `thread` | Worker finished with a trap; `join` was called after `detach` or operation on the thread failed |
| `host` | The native function returned an error ([8.3](08-interop.md#83-native-functions)) |

- A trap interrupts the entire call initiated by the executor: control returns to the executor along with the reason and trace – including functions and source code lines if the image contains debugging information.
- **Traps cannot be caught in M.** M code after a trap is not executed.
- After a trap, the instance remains usable: memory – as it was at the moment of the trap, and the data stack – as it was before the `executor` called the function. The `executor` can continue to call functions or discard the instance: the program might stop in the middle of modifying its data, and only the `executor` knows what is the most reasonable action.
- **No traps**: division by zero, overflow, shifting by the type width or more, operations with NaN and ∞, and converting NaN to an integer. All of these operations result in a defined value by the language ([4.6](04-expressions.md#46-casting-as)–[4.8](04-expressions.md#48-bitwise-operators-and-shifts)).

## 7.4 Built-in Functions

Built-in functions are called like regular functions, but their names are reserved:
You cannot declare your own function, variable, or parameter with these names.
You also cannot retrieve their value (e.g., `let f: ... = vaCount`).

| Function | Type |
|---|---|
| `typeof(T)`, `typeof(x)` | `Type`; Constant descriptor of the full static type |
| `sizeof(T)`, `sizeof(x)` | Non-typed constant |
| `alignof(T)`, `alignof(x)` | Non-typed constant |
| `offsetof(T, field)` | Non-typed constant |
| `length(s: String)` | `UWord`; Unicode character count; [2.12](02-types.md#212-strings) |
| `length(a: T[])`, `arrayCapacity(a: T[])` | `UWord`; current length and capacity |
| `arrayReserve(a, n)`, `arrayResize(a, n)`, `arrayPush(a, v)`, `arrayInsert(a, i, v)` | `Void`; Dynamic array operations, see below |
| `arrayPop(a)`, `arrayRemove(a, i)` | `T`; Removed element of a dynamic array |
| `vaCount(args)` | `ULong`; [3.4](03-declarations.md#variable-parameters) |
| `vaArg(args, i, T)` | `T`; Valid type for additional arguments, including `String` and `Type`; `i` is unsigned; [3.4](03-declarations.md#variable-parameters) |
| `breakpoint()` | `Void`; halts execution with `BREAKPOINT` and allows the executor to continue from the next instruction |

`typeof` and layout operations require type knowledge during compilation; `length` for strings and arrays performs a VM operation during execution; `vaCount` and `vaArg` work with a special argument package. Therefore, they remain built-in. A standard native call via C ABI does not replace these mechanisms.

### `typeof`

- It accepts exactly one argument: either a syntactic type `T` or an `expr` expression. It returns a `Type` value describing `T` or the static type of `expr` ([2.14](02-types.md#214-type-descriptors)). Variable names are resolved within their usual scope: a variable name is an expression, a type name is a type. Shadowed type names do not take precedence.
- The operand expression is not evaluated and does not receive a type from the external context of the result `Type`. Checks are performed for names and the validity of operations. `typeof(1)`, `typeof(null)`, `typeof([])`, and `typeof({ .x = 1 })` are errors: there is no context defined to specify the operand's type. `typeof(1 as Word)`, `typeof(1.0)`, `typeof(true)`, and `typeof("text")` are valid.
- The operand itself is not transparently dereferenced, and its write permission
  is not removed: for `r: @mut Point`, `typeof(r)` returns the descriptor
  `@mut Point`. Operations within the expression are type-checked as usual:
  `typeof(r.x)` describes the field's type, while `typeof(r as Point)` describes
  `Point`. The contents are not read, even when `r == null`.
- The call, object allocation, and access to array elements within an operand are not performed. For example, `typeof(makePoint())` does not call the function, `typeof(@p)` does not allocate an object, and `typeof(values[i])` does not read the array or index, nor does it check the boundaries. However, the type of the function, field, or element must be determinable.
- `typeof(Void)` is valid, just like `typeof(f())`, if the result of `f` is `Void`: the function is not called, and the result is a `Void` descriptor. This does not allow for variable, field, array element, or content dereferencing of types `Void`. The `args: ...` package of a standalone type does not exist: `typeof(args)` results in an error.
- The result is always constant, including for a mutable variable, parameter, or function call. It can be used in a global initializer and stored in the variable `Type`. Obtaining a descriptor does not execute code or trigger garbage collection.
- `typeof(t)` when `t: Type` describes the `Type` itself, not the type that is stored in the descriptor `t`. In `typeof`, `sizeof`, `alignof`, and `offsetof`, the value of `Type` is not substituted for the syntactic type: `sizeof(t)` represents the size of the descriptor itself, and `offsetof(t, x)` results in an error.

```m
let structureType: Type = typeof(Point)
let p: Point = { .x = 1, .y = 2 }
let variableType: Type = typeof(p)
let same: Bool = structureType == variableType // true
let descriptors: Type[] = [structureType, typeof(@Point)]
```

`Point` here refers to a previously declared struct. `typeof` is not a regular function: it cannot be used to retrieve its value or pass it as a callback.

Full M descriptors are not yet implemented: the existing mvm instruction `TYPEOF` only returns `mvm_kind_t` and reads a value from the stack. This differs from the unevaluated operand defined here.

### `sizeof`, `alignof`, `offsetof`

- The arguments `sizeof` and `alignof`— are either a type or an expression, similar to C. The expression is not evaluated; only its type is taken. Variable names— represent values, while type names— represent types: `sizeof(bar)`, `sizeof(bar[0])`, `sizeof(*p)`, `sizeof(s.f)`.
- The result is an **untyped constant**, like an integer literal: the context
  determines its type ([4.2](04-expressions.md#42-literals-and-constant-expressions)).
- The size of the array is the size of its reference, regardless of its current length:

  ```m
  let bar: String[2] = ["one", "two"]   // String[].
  let bytes: UWord = sizeof(bar)        // 8: Link size.
  let count: UWord = length(bar)        // 2: execution time value.
  ```

- `offsetof(T, field)` – only the type and only the direct field of the `T` struct.
- `sizeof(r)` and `alignof(r)` for `@T`/`@mut T` are 8: this is the size of the reference itself. There is no transparent reading in these operations.
- `sizeof` and `alignof` for `T[]`, including parameters, are equal to 8: this represents the size and alignment of the reference. The size of the elements cannot be obtained through `sizeof(a)`; instead, the number of elements is given by `length(a)`.
- `sizeof` on `Void`, with values without type (`sizeof(1)`, `sizeof(null)`), and within the `args: ...` package – is an error.

`sizeof` does not reveal the size of the GC header, the volume of reachable objects, or heap consumption; instead, it represents the size of a value according to the rules defined in section 2. `alignof` and `offsetof` also describe data layout, not the garbage collector mechanism.

### `length`

- The `length(s)` function calculates the length of the string `s` and returns a `UWord` – the number of Unicode code points. For `null`, the result is a `string` trap.
- For a dynamic array `T[]`, including the parameter `length(a)`, which calculates `a` and returns the current length as `UWord`; for `null`— a `memory` trap. The result is not a constant expression.
- For `@T`/`@mut T`, where `T` is an array or string, `length(r)` reads the contents of the reference and performs a standard length operation; `null` for an external reference yields `memory`. For scalars, structs, and values without a type, `length` is invalid.

### Dynamic Array Operations

These built-in operations work with `T[]`; `T` is defined as the type of array elements. These are specific rules for built-in operations, not generic functions M. New arrays are created using a literal or by declaring `let a: T[n] = [...]` with an initial length (3.2). Creation in bytecode is performed via `ARRAY_NEW`, and there is no separate built-in function for creating arrays.

| Expression | Result and Behavior |
|---|---|
| `arrayCapacity(a)` | `UWord`, current capacity, must be at least `length(a)` |
| `arrayReserve(a, n)` | `Void`, ensure sufficient capacity for at least `n` elements, while preserving the length. |
| `arrayResize(a, n)` | `Void`, set the length, either by adding zero-valued elements or removing the last elements |
| `arrayPush(a, v)` | `Void`, add the value `T` to the end |
| `arrayPop(a)` | `T`, remove and return the last element |
| `arrayInsert(a, i, v)` | `Void`, insert value `T` before index `i`, shifting the tail to the right |
| `arrayRemove(a, i)` | `T`, remove and return element `i`, shifting the tail left |

- `n` and `i` are unsigned integers; literals receive the `UWord` context.
  Lengths and capacities above `4_294_967_295` are invalid and cause a `memory`
  trap; exceeding available memory causes `allocation`.
- `arrayInsert` accepts `0 <= i <= length(a)`, including adding to the end;
  `arrayRemove` – only `i < length(a)`. `arrayPop` on an empty array and going beyond the bounds results in `memory`. All operations on `null` result in `memory`.
- All modifying operations require a mutable reference to the array, such as writing `a[i] = v`. These modify the object, preserving the reference and the type of its elements; their aliases immediately see the result. The passed values are copied according to standard rules of type `T`.
- `arrayReserve` does not reduce capacity; shrinking with `arrayResize`
  retains the reserve. `arrayResize(a, 0)` clears the array. The language does
  not specify the exact capacity; automatic growth is geometric, `arrayPush`
  has amortized complexity `O(1)`, and `arrayInsert`/`arrayRemove` are `O(length(a))`.
- Arguments are evaluated from left to right exactly once, after which the operation checks the current length and types. Before modification, all boundaries and necessary allocations are verified. If the operation itself fails, the elements, length, and capacity are preserved; any side effects of the argument calculation are not reverted.
- During possible garbage collection, the array, its previous elements, and the added value are retained. Deleted slots are cleared. The reference to the array remains stable; there is no direct access to its internal buffer from M.

In the current C-executor, boundary/type errors return `RUNTIME_ERROR`, and memory allocation failures return `VM_ERROR`. The named traps described above represent contract M.

### VM Library and Internal Operations

Bitwise operations `clz`, `ctz`, `popcount`, `bswap`, `rotl`, `rotr` belong to
[native-library](../native-library.md). These are standard, importable functions: their names can be declared, renamed upon import, and used as values directly. Separate opcodes for them are not required.

GC limits and diagnostics are available to the executor through the VM API.
The library provides byte-buffer operations with range checks and separate
operations for arrays of references. The compiler implements copying and zeroing
with internal VM operations that account for reference metadata; they do not
require identically named functions in the user namespace.

## 7.5 Memory Access Order

- Within a single execution context, the program sees memory accesses in the order they appear in the source text. Access by different workers to the same mutable objects requires synchronization; `join` also provides a boundary for worker termination.
- **A native call is a boundary**: before an `extern` call (or a call through
  a function value that may refer to it), all program writes are visible in
  memory; after return, the program rereads everything the native function
  could have changed.

### Threads

The module `std/thread.m` creates OS threads if the `executor` provides its `extern` functions. All workers run within a single mvm instance and share RAM, globals, natives, and the GC heap. Only the stack, frames, PC, and execution state are separate for each worker. GC stops all other contexts at instruction boundaries and scans all their stacks.

```m
import { spawn, join } from "std/thread.m"

let double(x: Long): Long { return x * 2 }
let thread: Thread = spawn::<(x: Long): Long>(double, 21)
let result: Long = join::<Long>(thread)  // 42
```

`Thread` is an opaque handle belonging to its instance. `spawn<F>` takes a
function of concrete type `F` and any number of arguments; the compiler uses
the parameters of `F` to validate and contextually type those arguments.
`join<R>` waits and returns a result of type `R`; repeated `join` returns the
saved result or the worker's trap. `detach` prohibits subsequent `join` calls.
`interrupt` requests cooperative termination without interrupting a native
function already running. Destroying the last handle detaches the worker;
destroying the instance waits for all workers before freeing shared memory.

The current C API and GC synchronization are described in [thread.md](../thread.md).

## 7.6 `mvm run`

```
mvm build [-I directory]... file.m -o file.mvm
mvm run   [-I directory]... file.m|file.mvm [arguments...]
```

`mvm build` compiles the program into an image. `mvm run` executes the following steps:

1. Compiles the program (or loads a pre-built image).
2. creates an instance with a prelude `mvm run` (below);
3. creates `String` values and an array of strings via the VM API;
   `args[0]` is the path to the program. The executor retains them as the root of its GC during the `main` call.
4. calls the root file's `main`
   ([3.4](03-declarations.md#main)): `main(args)` or `main()`;
5. The result of `main` becomes the exit code of the process.

Trap: `mvm run` prints the reason, code, and trace to stderr and exits with code 101. There is no `main` or it has a different signature – an error before execution.

### `mvm run` prelude

These functions are visible in every file without `import`.
([8.6](08-interop.md#86-prelude)):

```
extern let print(s: String): Void                       // In stdout, as is
extern let println(s: String): Void                     // in stdout and `\n`
extern let boolToString(x: Bool): String
extern let longToString(x: Long): String
extern let floatToString(x: Float): String
extern let doubleToString(x: Double): String
extern let stringToByte(s: String): Byte
extern let stringToUByte(s: String): UByte
extern let stringToHalf(s: String): Half
extern let stringToUHalf(s: String): UHalf
extern let stringToWord(s: String): Word
extern let stringToUWord(s: String): UWord
extern let stringToLong(s: String): Long
extern let stringToULong(s: String): ULong
extern let stringToFloat(s: String): Float
extern let stringToDouble(s: String): Double
extern let eprint(s: String): Void                      // in stderr
extern let eprintln(s: String): Void
extern let readLine(): String                          // Input string
```

To display numbers, use `println(toString(value))`. The `toString` function, from `mvm-lib/base/string.m`, accepts `Bool | Long | Float | Double | String` and calls the appropriate conversion function from the prelude. Integer numbers are converted to decimal notation, and floating-point numbers are converted to a notation with enough significant digits for accurate reverse conversion.

The `stringTo...` functions process strings entirely. Integer functions use decimal notation: signed integers can optionally include a `+` or `-`, while unsigned integers only accept `+`. `stringToFloat` and `stringToDouble` utilize the `strtof` and `strtod` rules, but they do not accept leading or trailing whitespace characters. Empty or invalid strings, as well as exceeding the target type range, including overflow and underflow for floating-point numbers, trigger a trap in the executor.

- `s` — represents a `String` value. The output functions accept the existing string, and passing `null` results in a `string` trap.
- `readLine` returns a new string without a trailing newline;
  `null` at the end of input. An empty input string is returned as
  `""`. Invalid UTF-8 in input results in an executor error.

This prelude is provided by the `mvm run` runtime environment. The composition of the prelude is determined by the mvm configuration during program compilation ([8.6](08-interop.md#86-prelude)).

```
let main(args: String[]): Word {
    println("Hello, World!")
    return 0
}
```

## 7.7 Implementation Guarantees

- **Determinism.** For a program without `std/thread.m`, the same mvm build,
  image, arguments, and native-function responses produce the same results and
  memory contents on every platform. The exception is the bit patterns of NaN
  values ([2.2](02-types.md#22-scalar-types)). Worker scheduling and native
  functions' synchronization of shared memory are outside this guarantee.
- **Isolation.** The program cannot read or modify anything outside of its own memory, and it can only call functions provided by the `executor`. Files, network, time, and threads are inaccessible without the corresponding modules explicitly provided; `std/thread.m` creates workers only within its own `instance` and does not access GC memory from other instances.
- **Default limits**, which the executor can modify
  ([8.7](08-interop.md#87-limits-and-interrupts)): data stack – 1 MiB,
  call depth – 10,000, memory – 4 GiB, fuel is unlimited.
- The compiler must accept at least 255 parameters for a function and a nesting depth of at least 256.


### Union Values

Packing a `union` and extending to another `union` can allocate a GC-object.
Extracting a `struct` creates a separate copy. `variantof` only reads the descriptor of the active type; the `Type` result does not retain the payload.
Temporary values during packing, copying, field and array creation are held by GC until the operation is complete. An incomplete `union` is not observed by the program. `arrayResize` and creating an array with an initial length require an element with a default value; for `union`s, this is prohibited in the current version.
