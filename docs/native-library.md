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

# Native Library and Boundary with VM

[M drafts](M/README.md) · [Virtual Machine](M/07-runtime.md) · [GC mvm](gc.md)

This document outlines the division of responsibilities and the designed contract for library operations. The `std/bits.m` modules and buffer operations are not yet implemented. The input and output functions defined in `base.m` are registered with the host `mvm run` and do not require a separate native library.

## What Remains in the Language and VM

- The compiler generates a descriptor `Type` via `typeof`, calculates `sizeof`, `alignof`, `offsetof`, and processes the `args: ...` package through `vaCount` and `vaArg`.
- The VM executes calls, allocates objects, manages GC roots, and halts execution upon error, returning control to the host along with the reason for the halt.
- `@T`/`@mut T` are passed using typed descriptors; reading and writing
  of their contents are performed via the VM API, with type and permission checks.
  Creating `@value` and transparent access via a reference – these are language operations,
  not native library functions.
- Placement of objects, heap growth, its statistics, limits, and forced garbage collection are part of the VM host API.

## Bitwise Functions

The planned module is `std/bits.m`. It exports standard `extern` declarations; the host registers the corresponding native functions. Importing the module itself does not load a dynamic library. The implementation can be registered beforehand or added via `libraryLoad`; a call without it results in a runtime error.

In this language, there is no function overloading. Instead, the width is determined by the name: primary names work with `ULong`, a `32` suffix – with `UWord`, and `bswap16` – with `UHalf`. Signed values are explicitly converted using `as`.

| Functions | Parameters | Result |
|---|---|---|
| `clz`, `ctz`, `popcount` | `x: ULong` | `UWord` |
| `clz32`, `ctz32`, `popcount32` | `x: UWord` | `UWord` |
| `bswap` | `x: ULong` | `ULong` |
| `bswap32` | `x: UWord` | `UWord` |
| `bswap16` | `x: UHalf` | `UHalf` |
| `rotl`, `rotr` | `x: ULong, n: ULong` | `ULong` |
| `rotl32`, `rotr32` | `x: UWord, n: ULong` | `UWord` |

- `clz` counts leading zero bits; `ctz` counts trailing zero bits.
  For zero, both return the operand's width: 64 or 32.
- `popcount` counts set bits; for zero, the result is 0.
- `bswap` reverses the byte order.
- `rotl` and `rotr` perform a cyclic shift within the operand's width;
  the amount of shift is determined modulo 64 or 32. A shift of 0 or a full width shift returns the original value. The C implementation must separately consider the zero remainder, to avoid shifting by the type width.

Example of usage after the module has been introduced:

```m
import { popcount, rotl32 } from "std/bits.m"

let countFlags(mask: ULong): UWord { return popcount(mask) }
let rotate(value: UWord): UWord { return rotl32(value, 7) }
```

These are standard calls with standard function type and value rules.
The compiler does not require special rules for literals in these calls.
The initial implementation is through `NATIVE`; optimization of individual calls can be implemented later, when maintaining the library contract is ensured.

## Buffers and Arrays

The library provides operations for copying and filling byte buffers. Contract:

- The operation receives a buffer with a known VM length, an offset, and a byte count; copying receives these data for the source and destination. Simple pairs of C pointers and size are insufficient.
- Buffer ownership, mutability, and both range checks are verified before writing, including overflow checks during boundary calculations. The error should not leave a partially modified buffer.
- Copying allows for overlap, like `memmove`. The fill operation writes one `UByte` to all bytes within the range. An empty range at the end of the buffer is permissible; it does not invalidate the buffer's own validation.
- The roots maintain both buffers throughout the native-call. The implementation works through a validated VM API, and does not dereference the number passed by the program as a host address.
- Arrays containing references are copied **element by element**, with type checking and through VM operations. Arbitrary byte filling is prohibited. This preserves the ability to add write barriers in the future.

An M dynamic array has type `T[]`, stores its length, and is passed
as an instance descriptor. An array literal and the declaration
`let a: T[n] = []` create an object through `ARRAY_NEW`. The operations
`arrayCapacity`, `arrayReserve`, `arrayResize`, `arrayPush`, `arrayPop`, `arrayInsert`, `arrayRemove`
are built into M and the VM (section 7.4 of the draft). They do not require
host function registration. The C API provides the corresponding
`mvm_array_*`; a separate API for copying/filling ranges is not yet available.
The buffer library module's names and signatures still need to be finalized.
`REFERENCE` itself also contains no length and does not replace a validated buffer.

## Strings

The output functions accept the `String` directly:

```m
extern let print(text: String): Void
extern let println(text: String): Void
```

Strings retain their `MVM_KIND_STRING` kind when called with `NATIVE`.
They cannot be declared as parameters with `REFERENCE` or numerical addresses.
String operations within the VM perform comparisons, indexing, length calculations, and concatenation. Byte encoding and formatting are handled by a library.

In the target API, a native function receives a string descriptor and accesses
the text through the VM context. It keeps roots during allocation and
returns a validated string value. The current `libffi` ABI
passes the contents as `char *`; this parameter must be treated as read-only.
Lifetime and null-termination requirements apply to the C ABI and are described
in [gc.md](gc.md#strings-in-the-current-c-abi).

## Loading Libraries from M

The standard library module `base/native.m` exports:

```m
extern let libraryLoad(path: String): Bool
extern let libraryDispose(path: String): Bool
```

It loads the library via `mvm_load_native_library` and registers its entire `mvm_get_native_exports` table within the current VM instance. For example:

```m
import { libraryLoad, libraryDispose } from "base/native.m"
import { println, toString, eprintln } from "base.m"

extern let nativeTestAnswer(): Long

let main(): Word {
    if !libraryLoad("libanswer.dylib") {
        eprintln("Failed to load library")
        return 1
    }
    println(toString(nativeTestAnswer()))
    if !libraryDispose("libanswer.dylib") return 1
    return 0
}
```

Native import, for which image loading doesn't yet have an implementation, receives a delayed binding while preserving the signature. Calling before the appropriate function is registered results in a runtime error. After `libraryLoad`, the call utilizes an export with the same name; a signature mismatch is also a runtime error. However, the existing instruction IDs `NATIVE` remain unchanged.

Success returns `true`. Failure to open a library, an incompatible ABI, or a missing/invalid export table returns `false`; the program continues execution and may choose an alternative path or abandon native functions. An empty path also returns `false`. The loader writes the reason for failure to stderr, and if a handle was opened, it is closed, and the registration of this attempt is cancelled; previously loaded libraries are preserved. Errors in VM memory allocation and internal errors still halt execution – they are not masked as a typical loading failure.

At the C API level, `mvm_load_native_library` returns `MVM_RESULT_OK` on
success, `MVM_RESULT_RUNTIME_ERROR` on loading failure, and
`MVM_RESULT_VM_ERROR` on resource/internal errors or `mvm == NULL`.
The language wrapper converts `MVM_RESULT_RUNTIME_ERROR` into `false`.
Checking for `false` does not make a subsequent call to a missing extern function
valid: that call still stops the program.

`libraryDispose(path)` disables the exports from the most recent successful load
with exactly the same path string. It returns `false` for an empty or unknown path,
including a second disposal without another load. Repeated loads require matching
disposals. The C API is `mvm_dispose_native_library` with the same result codes as
the loader. Existing native IDs are preserved; calls to a disposed export fail
with a runtime error. Deferred imports and saved M function values can resolve
again after a successful reload.

Workers share the runtime's native table. Registry changes and call resolution
are synchronized; calls already in progress retain the library until they finish.
Cloned VM instances retain independent registrations and handle references. The OS
handle is closed after its last registration and active call are released, or at
VM destruction. Release library-owned resources and stop using pointers or static
strings returned by the library before disposing it. A library must stop its own
background threads before its final handle is released.

A library packed as a resource is not loaded directly from ZIP. The program
selects a resource using `native.getOS()` and `native.getArch()`, reads it through
`mvm://`, writes it into a private directory returned by `file.makeTempDir()`, and
passes the resulting filesystem path to `libraryLoad`. After disposal, it deletes
the file and calls `file.removeDir`. Selection, extraction, and cleanup are
ordinary application code; see [DYNAMIC.md](../../DYNAMIC.md) for the complete
pattern.

This explicit release plus automatic cleanup follows the resource lifecycle
pattern described in [Wren's foreign data documentation](https://wren.io/embedding/storing-c-data.html#freeing-resources).
The dynamic library executes with host process privileges and must be trusted.

## Current native ABI

mvm registers functions via `mvm_register_native` or loads exports via `mvm_load_native_library`. `NATIVE` checks argument types and calls a C function via `libffi`. Numerical bitwise functions are handled within this ABI without introducing new opcodes. `FUNC` is passed as `uint32_t`, and `THREAD` is passed as an opaque pointer.

Built-in operations requiring VM context are registered via
`mvm_register_builtin`. This function receives `(mvm_t *, args, result)` and
returns `mvm_result_t` directly; it does not use libffi. This mechanism utilizes [thread module](thread.md). Dynamic libraries are handled with a reference counter, as workers own copies of the natives table and share loaded libraries.

References are held by temporary roots. New objects and references that C code saves after a return require explicit root registration – a local C variable is not sufficient. Details – [gc.md](gc.md#roots-and-host-api).

The standard libffi ABI does not automatically pass the VM context and does not provide a general mechanism for returning a trap from an arbitrary function within a linked library. Instead, there’s a separate mechanism for built-ins. For buffer operations, validated descriptors and complete types are still required. Therefore, transferring library operations does not simply mean registering an existing `memcpy` as a safe M function.


Union support extends the public type descriptors; native libraries must be
rebuilt with `MVM_NATIVE_ABI_VERSION == 2`. Older libraries are rejected by
the existing ABI check. Union values, including nested unions in callback
signatures or managed containers, are not part of the native call ABI.
