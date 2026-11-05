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

# 8. Interaction with the mvm

[← Runtime Environment](07-runtime.md) · [Table of Contents](README.md) · [Diagnostics →](09-diagnostics.md)

M — a statically-typed language for the mvm virtual machine.
This section defines the target contract for loading and running programs,
linking native functions, and accessing values through the mvm API.
The specific names of API functions are determined by the mvm implementation.

## 8.1 From Source to Call

```
source ──compile──▶ image ──instantiate──▶ instance ──call──▶ result
   + prelude                       + native functions or trap
                                    + Limits
```

1. **Compilation.** The mvm compiler builds a program from source files or
   strings provided by the loader ([6.3](06-modules.md#where-to-search-for-the-file)),
   with or without a prelude (8.6). The result is an image or a list of errors
   and warnings (file, line, column, message). The build command is `mvm build`
   ([7.6](07-runtime.md#76-mvm-run)); diagnostics are described in [section 9](09-diagnostics.md).
2. The **image** can be saved as `.mvm` and loaded later without
   a compiler or source files ([7.1](07-runtime.md#71-image-and-instance)).
3. **Instance Creation.** The executor associates each `extern` image import with a native function and sets limits (8.7).
   **Instance Creation Errors:**
   - There is no native function for importing.
   - The function exists, but its types do not match the declaration (8.2).
   - Segments and the stack require more memory than the limit allows.

   Native functions that an image does not import are not an error: one set of functions is suitable for different programs.
4. **Execution.** `mvm run` invokes `main` and uses its result as the exit code of the process ([7.6](07-runtime.md#76-mvm-run)).  The mvm executor API can also call other exported functions ([8.4]) and access instance values ([8.5]).

## 8.2 Types at the Boundary

The following types are permitted in the parameters and results of `extern`-functions and functions exported by the root file:

| M | Boundary Representation for mvm |
|---|---|
| `Byte`, `UByte`, `Half`, `UHalf` | Signed and unsigned integers with 8 and 16-bit widths |
| `Word`, `UWord`, `Long`, `ULong` | Signed and unsigned integers with 32 and 64-bit widths |
| `Bool` | Logical value: `false` or `true` |
| `Float`, `Double` | IEEE 754 binary32 and binary64 |
| `String` | Instance string descriptor |
| `Type` | opaque descriptor of a full instance type |
| `enum` | base integer type |
| `mut T[]`, mutable `T[]` | array descriptor |
| `@T`, `@mut T` | Object descriptor with verifiable access rights |
| function type | function instance descriptor |
| `Thread` | An opaque handle representing the thread associated with an instance |
| `Void` (result) | Absence of value |

- Structures by value through the boundary do not propagate; they transmit `@T` or `@mut T`. Fields and content are accessible to the executor via a typed VM API.
- The reference belongs to a specific instance. The VM verifies the full type of the content, write permissions, the absence of `null` upon access, and lifetime. Descriptors of another instance or an incompatible type are rejected.
- For arrays, the type of elements and mutability are checked. The VM performs reading, writing, and modification of length, with boundary checks. The internal buffer is not part of the ABI.
- `String` is passed as a string value. Text is accessed through the string API.
  UTF-8 is validated, excluding `U+0000`; `null` denotes an absent string.
  String contents are immutable.
- The `Type` is checked for both its association with an instance and its validity as a descriptor. The executor obtains it through the VM type API or as a result of M, rather than creating it from a random number or address. Metadata persists until the instance is destroyed; for a saved `Type`, a separate GC root is not required. Passing a descriptor to another instance is rejected, even if that instance was created from the same image.
- `Thread` belongs to the instance that created the worker. It can be stored
  and used only with that instance; passing it to another instance is rejected.
  Releasing the handle detaches the worker without waiting for it.
- References, arrays, and strings stored by the executor between calls or allocations require established roots. A descriptor itself does not create a persistent root.
- The VM checks parameter counts and order, complete reference types,
  access rights, and variadic status. `enum` is represented at the boundary
  by its underlying number. Function descriptors are checked against their signature.
- A variadic packet is passed through the VM adapter with types and roots
  for reference elements (3.4); an array of numeric slots without metadata
  is not a substitute for it.

This is the target ABI M. The current C ABI via `libffi` only checks for `mvm_kind_t` and does not automatically pass context; VM-aware built-ins receive `mvm_t *` through a separate registration. Full `Type` descriptors, types, and `@T`/`@mut T` rights still require an adapter. The implemented part is [gc.md](../gc.md) and [native-library.md](../native-library.md).

The `union` is not transmitted via native/host ABI, including within `@T`, `T[]`, structs, and callback signatures. Internal imports for M-modules support `union`. To call a native function, use an M-wrapper with variant parsing.

## 8.3 Native Functions

```
extern let log(msg: String): Void
```

- The native library or mvm runtime provides a function, named with full parameter and result types.
- When creating an mvm instance, it associates each `extern` with this name.
- The native function receives arguments and **call context**: access to the instance's memory (8.5) and the ability to call its functions.
- It returns a result or an **error**. An error stops the program with a `host` trap, including a native function message ([7.3](07-runtime.md#73-traps)).
- **Recursive calls are allowed:** the native function can call an exported function or a function value of the same instance. Nested calls are handled on the same stack and are counted as part of the call depth. A trap is triggered in the nested call and presented to the native function as an error; if the native function returns, the trap continues upwards.
- The descriptor passed to the program is checked by the VM API: its type, instance, write permissions, and boundaries are verified upon accessing the value (8.5).
- Native library functions are linked in the same way as other `extern` functions. Importing a function does not grant it any special privileges and does not reserve its name
   ([native-library](../native-library.md)).
- The VM keeps reference arguments rooted until the call finishes,
  including nested calls and error returns. A C function must root a new object
  before the next allocation; a descriptor passed to it does not automatically
  remain rooted after the function returns.

## 8.4 Calling M Functions

- The executor can view **root file exports**.
  ([6.4](06-modules.md#64-program-and-image)). The image export list contains the name, type (function or variable), types, and doc-comments.
- The executor calls a **function** by name with arguments. mvm checks
  argument counts and types, executes the function, and returns a result or trap.
- **Callback.** The program can pass a function value to a native function – either as an argument to an `extern`-function, or as a result of a callback. The native function can store it and call it later, while the instance is alive. The checks are the same as when calling by name.

  ```
  extern let onEvent(name: String, handler: (code: UWord): Void): Void

  let setup(): Void {
      onEvent("resize", (code: UWord): Void { relayout() })
  }
  export { setup }
  ```

- The executor obtains a **variable** by name as a typed export. The API
  reads its value and allows writes only for `let mut`. A function variable
  `let mut f(...)` can thus be replaced by writing a function descriptor.
- **No initialization**: when an instance is created, the code is not executed. ([7.1](07-runtime.md#71-image-and-instance)). If the program requires preparation, it performs it in `main` or exports a function like `init`, which the executor calls first.
- The `main` function for the executor is a standard exported function
   ([3.4](03-declarations.md#main)).

## 8.5 Accessing Objects and Values

- The executor retrieves and modifies values via the VM API: exported variables – by name, objects – through typed descriptors, structures – by fields, arrays – by indices, and strings – through a string API. Type, write permissions, instance, bounds, and lifetime are all checked.
- The API is accessible between M calls and during the execution of a native function.
  During this time, M is not executed in parallel within the same instance (7.5).
- To store an object between calls and allocations, it registers the GC root. This applies to the reference returned by the program as well. The root is released when the value is no longer needed.
- The borrowed byte representation for the executor is only obtained for the `UByte[]` buffer, during a single access without nested execution M. After the program call, the representation is obtained again: the buffer of the array might have changed.  Byte-by-byte references are neither read nor written.
- To pass an object, the executor creates it with metadata via the VM, initializes fields, registers the root, and passes `@T`/`@mut T`.
  Links are recorded through typed operations, so GC can see all connections and maintain write barriers.
- To transmit bytes, the executor creates a `UByte[]`, fills it via a verified API, and then passes the array. Raw data does not contain references to M objects.
- To pass text, the executor creates a `String` through the VM API, validates
  UTF-8, and keeps a root. A string returned by a native function must belong
  to the instance or have a registered lifetime.

This is an API project; the current executor interface and its limitations are described in
[gc.md](../gc.md#roots-and-host-api).

## 8.6 Prelude

The prelude is an M file that the executor passes to the compiler together with
the program. **Everything it exports is visible in every program file
without `import`.**

```
// prelude.m environments for mvm
extern let print(s: String): Void
extern let readLine(): String
export { print, readLine }
```

- The name from the prelude behaves like a name declared **outside of the file**: its top-level declaration or import with the same name hides it without error or warning. A local name shadows it, just like any top-level name ([3.7](03-declarations.md#37-scope-and-order)).
- Typically, the prelude includes `extern`-declarations for native functions, but it can also contain functions on M, types, and constants – they are included in the image as another program file. Its export, viewed by the executor, is not visible; the executor only sees the exports of the root file.
- Prelude can import other files according to the rules [6.3](06-modules.md#63-import) relative to its own path.
- Unused prelude name – without warning.
- Without a prelude, there are no implicit names. The `mvm run` prelude is
  described in [7.6](07-runtime.md#mvm-run-prelude).

## 8.7 Limits and Interrupts

The executor sets upon instantiation:

| Limit | What is being limited | Upon exceeding |
|---|---|---|
| Memory | Total memory used by segments, stack, and heap objects, including overhead data for objects | When allocating memory, the VM attempts to collect garbage. If there is still not enough space, a `allocation` trap is triggered. If there is not enough space when creating an instance, a creation error occurs. |
| Data Stack | Data Stack Size | `stack` Trap |
| call depth | the number of nested calls, including through native functions | `stack` trap |
| Fuel | Work done | Trap `fuel` |

- **Fuel** is a work counter: every VM instruction consumes at least one unit.
  mvm defines the costs, which are identical on all platforms, making the `fuel`
  trap deterministic. The executor may add fuel between calls. Without a fuel
  limit, the amount of work is unlimited.
- **Interruption.** The executor can request that an instance stop from another
  thread. mvm checks the request in loops and at calls, stopping the program with
  an `interrupt` trap within a bounded number of instructions. This lets the
  executor set a timeout based on elapsed time rather than work performed.
- The performance timings for native functions do not account for limits.

## 8.8 Security

- The program can only calculate, modify its memory, and call functions provided by the executor ([7.7](07-runtime.md#77-implementation-guarantees)).
- **No program or image that has been checked will cause undefined behavior in the executor.** Anything that would be UB in C – in M, either produces a defined result or triggers a trap.
- Limits stop infinite loops and unbounded memory growth (8.7).
- The executor is responsible for its functions: determining what to allow the program to do.
- Data from the instance's memory for the executor is considered untrusted.
