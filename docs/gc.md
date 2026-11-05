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

# GC Heap, Arrays, Records, and Typed Values for mvm

This document describes the current C implementation of the VM, and sections 7, 9, and 10 from [OPSLEFT.md](../.hidden/OPSLEFT.md). The runtime draft also uses GC ([7.2](M/07-runtime.md#72-instance-memory)), but its typed references `@T`/`@mut T`, full access checks, and host API are not yet fully implemented. The C functions and opcodes described below already exist.

## Memory Budget

```sh
bin/mvm --min-memory 1MB --max-memory 64MB program.mvm
```

mvm flags must precede the program path. Everything after the path is passed
to `main(args)` as arguments to the M program itself.

`--min-memory` sets the initial reserve of the shared VM pool (1 MiB by default),
while `--max-memory` sets its hard limit (256 MiB by default). Without a suffix,
the size is in bytes. `KB`, `MB`, `GB`, `KiB`, `MiB`, `GiB`, and the short forms
`K`, `M`, `G` use a multiplier of 1024 and are case-insensitive.
A zero minimum is allowed; the maximum must be positive and at least the minimum.
Fractions, signs, spaces, and size overflow are rejected.

The flags `--ram` and `-r` are no longer available. The bytecode buffer and `rodata` are allocated based on the actual size of the linked image. This includes using a common budget for GC objects, array buffers, value stacks, the main thread’s frame, and all workers’ frames, global values, type metadata, and natives, as well as temporary loader and built-in input buffers. This encompasses pool headers, alignment, unused reserves, and temporary copies during buffer reallocation. Consequently, the amount of usable data is less than the maximum; fragmentation can also lead to allocation failures.

A non-relocating pool, synchronized between threads, and growing in blocks up to a maximum. Any freed blocks are consolidated and reused. Completely free additional blocks are returned to the system. An initial reserve is maintained until the instance is destroyed. This reserve represents allocated address space – the OS can physically provide pages later. This is the VM's allocation budget, **not a process RSS limit**. It does not include executable code or C runtime memory, thread system stacks, internal ZIP/TOML/libffi allocations, loaded libraries, and allocations of third-party natives, including shared handles between instances.

Upon failure to allocate a GC-object or a VM array buffer, the VM collects garbage and retries. If memory is still unavailable, the interpreter returns `MVM_RESULT_OUT_OF_MEMORY`; CLI prints `mvm: out of memory` with the instruction address and maximum budget, and then exits with a non-zero exit code. Errors in creating instances, including insufficient loader budget, also correctly release the reserve. Direct low-level C operations returning `MVM_RESULT_VM_ERROR` upon allocation failure maintain this contract; `mvm_memory_failures()` enables distinguishing such allocation failures from argument errors within the current thread.

C API: `mvm_create(path, minimum_memory, maximum_memory, frame_limit)`.
For default settings, `MVM_MEMORY_DEFAULT_MIN` and `MVM_MEMORY_DEFAULT_MAX` are used. `mvm_memory_stats(vm->memory, &stats)` returns the minimum, maximum, current reserve of `committed`, and the utilized portion of `used`;
`gc_bytes` describes only GC-objects and does not replace general accounting. When bytecode is manually loaded into an instance without a host image, `mvm_ram_resize(vm->ram, code_size)` is called **before** execution. After linking, the buffer is not relocated: string literals contain pointers within it.

## Garbage collection

The heap utilizes a non-moving mark/sweep: the address of a live object does not change.

Objects include raw bytes, UTF-8 strings `CONCAT`, typed arrays, dynamic records, values with metadata, variadic argument packs, unions, and thread handles, mutexes, condition variables, and atomics.

Garbage collection is initiated
before allocation when a threshold is reached, and again upon failure of allocation
from a common pool. The threshold after garbage collection is twice the size of live objects,
minimum 1 MiB; this threshold is not a memory limit.

`gc_bytes` considers headers and the contents of objects, including the entire capacity of separately allocated array buffers. `mvm_gc_collect(vm)`
explicitly initiates garbage collection and returns the number of objects freed;
`mvm_destroy` frees the entire heap.

Roots encompass all active slots of the main stack and worker stacks, `globals`,
including saved results from threads and registered ranges `mvm_value_t` from C.
When multiple execution contexts are present, a stop-the-world collector halts all other threads at instruction boundaries. Local variables and arguments for all frames reside in the value stack; the frames themselves only contain indices. Array, record, and typed value traversal is iterative, without recursion or memory allocations.
Unreachable cycles are collected. Strings in RAM and host external memory are not owned by the collector.

## Instructions

| Opcode | Operand | Stack | Result |
|---|---|---|---|
| `ALLOC` | — | `size → ref` | `REFERENCE` to `size` zero-initialized bytes |
| `ARRAY_NEW` | `ubyte kind` | `len → arr` | `ARRAY` of `len` elements |
| `ARRAY_GET` | — | `arr i → v` | Copy of an element value |
| `ARRAY_SET` | — | `arr i v →` | Write an element |
| `ARRAY_LEN` | — | `arr → len` | Length as `UWORD` |
| `ARRAY_CAP` | — | `arr → capacity` | Capacity as `UWORD` |
| `ARRAY_RESERVE` | — | `arr capacity →` | Ensure capacity without changing length |
| `ARRAY_RESIZE` | — | `arr len →` | Modify length, initialize new slots |
| `ARRAY_PUSH` | — | `arr v →` | Add element to the end |
| `ARRAY_POP` | — | `arr → v` | Remove the last element |
| `ARRAY_INSERT` | — | `arr i v →` | Insert before `i`, including `i == len` |
| `ARRAY_REMOVE` | — | `arr i → v` | Remove element `i`, shift the tail |
| `FIELD_GET` | `uhalf index` | `obj → v` | Copy of a scalar or reference field |
| `FIELD_SET` | `uhalf index` | `obj v →` | Write field with type checking |
| `FIELD_ADDR` | `uhalf index` | `obj → ref` | Internal reference to a field of a typed value |
| `TYPED_ALLOC` | `uword type` | `→ ref` | Zero value with type layout |
| `VALUE_COPY` | `uword type` | `dst src →` | Copying `sizeof(type)` bytes |
| `VALUE_ZERO` | `uword type` | `dst →` | Zero the value |

Array sizes and indices accept any integer type; negative values and values exceeding `size_t` are invalid. The length of an array is limited to `UINT32_MAX`; empty arrays and `ALLOC 0` are allowed; zero allocation returns a distinct, non-zero reference, which cannot be dereferenced. Raw blocks are aligned to `max_align_t`. Their contents are not treated as references and are not suitable for `FIELD_GET`/`FIELD_SET`. `LOAD`/`STORE` accept offsets in RAM; access to raw blocks is currently available to the host via `REFERENCE`.

`ARRAY_NEW` accepts all `mvm_kind_t` except `VOID` and invalid tags.

Numbers start at zero, `BOOL` at `false`, `TYPE` at `VOID`, `NULL` at `NULL`.

The `STRING`, `ARRAY`, `REFERENCE`, `FUNC`, `THREAD`, `MUTEX`, `CONDITION`, and `ATOMIC` slots initialize with `NULL` and allow `NULL` writes. All other types require exact type matching, without casting. Nested arrays can have any element type. `EQ`/`NE` compare arrays and `REFERENCE` and all handle types by identity; strings by content. `FUNC` stores the bytecode address, is compared by this address, and is not a GC-link; function arrays are processed in the standard way. Thread handles belong to their VM; when collected, they detach the worker without waiting for it.

The host creates records with `mvm_record_new`; a separate bytecode opcode for
creating records is not required. A record value has kind `REFERENCE`, but its
GC object kind is checked: a raw block, an interior pointer, or an external pointer
is not a record.
Fields are indexed from zero, start as `NULL`, and accept values of any type.
`mvm_record_get` and `mvm_record_set` provide the same access from C. GC traverses
all fields, so records can form cycles and retain arrays, strings, raw blocks,
and other records.

## Dynamic Arrays

`ARRAY_NEW`/`mvm_array_new` create growing arrays. The value of `ARRAY` contains a reference to a stable header; a separate private buffer has a length and capacity. Copying the value preserves the object's identity, and all aliases see the changes. Changing the length does not modify the element tag. There is no access to element addresses in the C API; the buffer can be relocated.

| C API | Contract |
|---|---|
| `mvm_array_new(vm, kind, length, out)` | New array with zero-initialized elements |
| `mvm_array_get/set` | Access within the current length |
| `mvm_array_length/capacity(vm, array, out)` | Length / Capacity |
| `mvm_array_reserve(vm, array, capacity)` | Reserve must be at least as large as requested; the length does not change. |
| `mvm_array_resize(vm, array, length)` | New length; newly added elements receive initial values |
| `mvm_array_push(vm, array, value)` | Append |
| `mvm_array_pop(vm, array, out)` | Remove the last element |
| `mvm_array_insert(vm, array, index, value)` | Insert before `index`; an index equal to the length is allowed |
| `mvm_array_remove(vm, array, index, out)` | Removal while preserving order and returning an element |

Geometric growth is automatic: `arrayPush` occurs with amortized complexity of `O(1)`, while insertion/deletion in the middle has a complexity of `O(length)`. `arrayReserve` does not reduce the reserve; shortening the length preserves the buffer. To clear, use `arrayResize(..., 0)`. Capacity and length are limited by `UINT32_MAX`; byte-size overflow is checked separately. Access to reserved slots is prohibited.

GC scans only the current length. Removed slots receive initial values; removing references allows for the collection of unreachable objects, including cycles. The buffer is freed along with the header during garbage collection and `mvm_destroy`. Its full capacity is considered in `gc_bytes` and the garbage collection threshold.

Before any modification, the VM checks types, array ownership by the VM, bounds,
and required allocations. On error, elements, length, capacity, stack, and output
parameters remain unchanged. Empty `arrayPop`, invalid indices, and lengths/capacities
above `UINT32_MAX` produce `RUNTIME_ERROR`; byte-size overflow or allocator failure
after collection and retry produces `VM_ERROR`. Collection itself may free other
unreachable objects. Insertion keeps the inserted value rooted during allocation;
growth operations keep the target array rooted for the duration of the call.

The host must register roots for values that exist between allocations. The returned `arrayPop`/`arrayRemove` element is no longer held by the array; register it until the next allocation. The `get`/`set`, length/capacity reading, `arrayPop`/`arrayRemove`, and length shortening operations do not allocate memory.

In M, all arrays have the type `T[]`. Declaring `let a: T[n] = []` creates an array with an initial length of `n`; this is not a separate type.

Built-in operations are described in
[7.4](M/07-runtime.md#dynamic-array-operations).

The M compiler is not yet available in the repository: the `.m` examples are specification-oriented. The current C array stores `mvm_value_t` and checks `mvm_kind_t`, not the full M types. Nested arrays have a common tag `ARRAY`. Storing structs by value within a dynamic array will require support for composite types in the compiler and VM.

New opcodes have been added to the end of the list; the numbers of the previous ones have been preserved.

## Typed Values

`mvm_type_register` verifies and copies `mvm_type_desc_t`, returning a `uword` identifier for the type. The descriptor defines size, alignment, fields, and a flat map of references. The fields and references are ordered by offset; scalar fields must match the representation of their `mvm_kind_t`. The `VOID` type field represents an embedded aggregate value: it cannot be read as a single `mvm_value_t`, but it can be accessed via its address and copied using a separate type. The map includes offsets for `STRING`, `ARRAY`, `REFERENCE`, `THREAD`, `MUTEX`, `CONDITION`, and `ATOMIC`, including offsets from embedded structures, arrays, and `packed` values.

`mvm_typed_alloc` and `TYPED_ALLOC` create a zero-initialized value of the exact type.
`mvm_typed_field_get`/`set` and `FIELD_GET`/`SET` use object metadata for whole
objects and full type information for interior aggregate values. `FIELD_ADDR`
and `mvm_typed_field_address` return an interior reference.
`VALUE_COPY` and `VALUE_ZERO` work with both whole objects and such interior references.
Scalars are stored in little-endian format and support unaligned offsets.
Reference, function, and handle fields occupy 8 bytes and start as `NULL`;
`FUNC` is not a GC root, while the other handles retain their objects.

GC traverses the type graph of a typed object without recursion.
Internal references maintain the entire object. Cycles between typed values, records, and arrays are detected in the standard way. The current representation of fields is determined by C ABI. Types and permissions `@T`/`@mut T` from M require separate verification; the internal reference within C API to a field does not match the expression `@field`, which in M creates a separate object with a copy of the value.

An invalid type, index, size, or truncated instruction results in `RUNTIME_ERROR`.
A memory allocation overflow or allocator failure results in `VM_ERROR`.
Checks are performed before the stack, elements, and fields are modified; if an error occurs, `pc` indicates immediately after the opcode. All operations consider the current frame's boundary.

`CONCAT` allocates a string directly in the general heap. Its operands remain rooted on the stack during allocation. `LITERAL_STRING` continues to reference RAM. String operations verify UTF-8; reading a string from RAM or a GC-object is limited by the remaining bytes of the corresponding area.

## Strings in the Current C ABI

At the language level, a string has the type `String`, while at the VM level, it’s represented as `MVM_KIND_STRING`. Assignment copies the reference. Strings are compared by their UTF-8 content; length and index are measured in Unicode code points. `STRING_GET` returns `UWORD`, and `STRING_LEN` returns the length as `UWORD`. An out-of-bounds index, an incorrect type, and invalid UTF-8 result in `RUNTIME_ERROR`. Concatenation returns a new string, preserving both operands.

In the C-representation, `mvm_value_t.as.s` stores `char *`, and `NATIVE`
transfers it via `libffi`. The length of the bytes is determined by the null terminator;
an embedded null byte terminates the text, so `U+0000` is not currently supported by the ABI. The draft of string literals
rejects such values. There is currently no full public API for creating
rooted strings from text with length; `string_rooted` from
the draft of embedding represents a future API.

The C function should treat text as immutable. It can return static text or a string from a GC object with proper lifetime. A pointer to a C temporary buffer after returning is invalid. Assigning `as.s = NULL` when `STRING` is encountered does not create an empty string: string operations reject such a value. An empty string is a pointer to the text `""`. The absence of a value in array slots is represented by a specific type `MVM_KIND_NULL`.

## Roots and Host API

`mvm_alloc`, `mvm_array_new`, and `mvm_record_new` may trigger garbage collection.
A reference held only in a local C variable is not a root. Before the next
allocation, place the value on the stack, in globals, or in a
registered range:

```c
mvm_value_t held = {.kind = MVM_KIND_NULL};
mvm_gc_root_t root;
if (!mvm_gc_root_register(vm, &root, &held, 1))
  return MVM_RESULT_VM_ERROR;

mvm_result_t status = mvm_array_new(vm, MVM_KIND_STRING, 4, &held);
if (status == MVM_RESULT_OK) {
  // The variable holds an array; further allocations can be performed here.
  size_t length;
  status = mvm_array_length(vm, held, &length);
}
mvm_gc_root_unregister(vm, &root);
return status;
```

The collector reads the current contents of the range during each collection.
`mvm_gc_root_t` and the range itself must remain at their original addresses until
unregistration. A descriptor must not be copied or registered in multiple VMs
simultaneously. Roots can be unregistered in any order.
`mvm_array_get`, `mvm_array_set`, `mvm_array_length`, `mvm_array_capacity`,
`mvm_array_pop`, `mvm_array_remove`, `mvm_record_get`, and
`mvm_record_set` check that the object belongs to this VM; they do not allocate
memory. API output parameters remain unchanged on error.

The arguments `NATIVE` are copied to a stable temporary buffer and registered as roots throughout the C call, including on error exit. `STRING` is passed as `char *`, `REFERENCE` as `void *`, `ARRAY` as an opaque `mvm_array_t *`, handles as opaque pointers of the corresponding type, and `FUNC` as `uint32_t`.

New objects created within the C function must be rooted manually until the next memory allocation. After the function returns, the result is held in the VM stack. To preserve the argument after the call completes, the host must register its long-lived root.

References `STRING`/`REFERENCE` within the GC object also retain the entire object – for example, the string suffix returned by a C function. A pointer beyond the object's boundary is not rooted. External C pointers are not freed; their validity, lifetime, and NUL-termination of external strings remain the responsibility of the host.

## Boundary with Draft Language

- `mvm_alloc` and `ALLOC` are allocated in the GC heap. `mvm_gc_collect` and `gc_bytes` relate to API/host state, not to the built-in functions of M.
- RAM for the current VM and its GC objects are separate storage areas. `LOAD` and `STORE` accept a numerical offset within RAM; `REFERENCE` stores a C pointer. This is an internal C ABI that currently does not guarantee type-safe references `@T`/`@mut T` from M. The system does not provide numerical access to objects or conversion of descriptors into numbers in the language.
- `String` already possesses its own representation and string opcodes. Immutability of strings at the boundary with C is currently ensured through an agreement with the host: the ABI passes a standard pointer. The draft describes validated string descriptors. `LOAD`/`STORE` of the current VM also allow modification of the literal bytes in RAM; therefore, string operations re-verify the text.
- Dynamic arrays in the C API currently only check the element tag `mvm_kind_t`. The full type of the nested array and composite values within Draft M still require implementation.
- `MVM_KIND_TYPE` stores only `mvm_kind_t`. `TYPEOF` returns the category of a value, `IS` checks the category, and `LIT_TYPE` creates its descriptor.
This is not yet the full [`Type` of M](M/02-types.md#214-type-descriptors).
The `type_id` of a typed object refers to a registered layout;
the names of types, reference target types, and complete signatures in these metadata are not yet represented. Unlike `typeof(expr)` in M, the `TYPEOF` instruction consumes a value already calculated from the stack.
- GC traverses typed values in the heap, array elements, and record fields.
  Reference maps for these objects are implemented; dynamic records still store
  `mvm_value_t`. A byte-based data stack with maps for local and global values
  and variadic-packet metadata is not yet implemented. References cannot be stored
  inside a raw `ALLOC` block with the expectation that GC will discover them.
- The current allocation failure returns `MVM_RESULT_VM_ERROR`; a separate
  trap `allocation` – a target contract, not existing code result.
  The call depth limit already exists, but the general memory limit from section 8
  still needs to be implemented.
- Native modules for bitwise functions and safe operations on buffers
are designed in [native-library.md](native-library.md). Regular
C functions do not automatically receive VM context; the API for these modules
is yet to be implemented.
