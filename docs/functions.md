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

# Functions as mvm values

This document describes the current C implementation of the VM and item 8 in
[OPSLEFT.md](../.hidden/OPSLEFT.md). As with a direct `CALL`, the call target
is an absolute address in shared RAM.

## Instructions

| Opcode | Operand | Stack | Result |
|---|---|---|---|
| `LIT_FUNC` | `uword address` | `→ fn` | `MVM_KIND_FUNC`, `as.func = address` |
| `CALL_INDIRECT` | `ubyte argc` | `args… fn → result` | Call a function from the stack |

The address is encoded as four little-endian bytes. `LIT_FUNC` checks that it
is within RAM. Address `0` is valid, as with `CALL`; an absent function
is represented by a separate `MVM_KIND_NULL` value, which cannot be called.
Before `CALL_INDIRECT`, push arguments in parameter order,
then the function value. The instruction checks that the current frame contains
the arguments and function, verifies the `FUNC` type, address bounds, and call-depth limit.
The function value is popped; the first argument becomes the
`LOCAL_GET 0` slot. Arguments themselves remain on the stack until return.

Frames, limits, and `RETURN` are shared with `CALL`: direct and indirect calls can
be mixed, including in recursion. `RETURN` replaces arguments and local
variables with a single result; a function without a result requires
`LIT_NULL` before it. Even with `argc = 0`, the function must push a result.

A truncated operand, incorrect type, address outside RAM, insufficient arguments,
or excessive call depth produces `MVM_RESULT_RUNTIME_ERROR`. Failure to allocate memory
for frames produces `MVM_RESULT_VM_ERROR`. On error, the instruction preserves the stack,
frames, and RAM; `pc` remains immediately after the opcode. On success, return resumes
execution immediately after the `argc` byte.

Bytecode example: `add(20, 22)` returns `42`.

```c
const uint8_t program[] = {
    MVM_OP_LIT_UBYTE, 20,          // 0: arg 0
    MVM_OP_LIT_UBYTE, 22,          // 2: arg 1
    MVM_OP_LIT_FUNC, 12, 0, 0, 0, // 4: add
    MVM_OP_CALL_INDIRECT, 2,       // 9
    MVM_OP_RETURN,                // 11
    MVM_OP_LOCAL_GET, 0, 0,       // 12: add(a, b)
    MVM_OP_LOCAL_GET, 1, 0,       // 15
    MVM_OP_ADD,                   // 18
    MVM_OP_RETURN,                // 19
};
```

## Storage and types

`FUNC` can be passed as a parameter, returned, copied through stack
operations, and stored in local and global variables and `FUNC` arrays.
Elements of such arrays are initially `NULL` and accept `NULL` assignments.
`EQ`/`NE` compare function addresses; values with different `kind` values are unequal.
Ordered comparisons and function arithmetic are invalid.
`TYPEOF`, `IS FUNC`, and `LIT_TYPE FUNC` work as usual.
`CAST FUNC` preserves an existing `FUNC` value; conversions between
functions and numbers, `REFERENCE`, or `NULL` are not yet available.

`FUNC` contains no host pointer and requires no GC object. The collector does not
trace it; an array of functions itself remains an ordinary GC object.
`LOAD`/`STORE` do not support `FUNC`. The `NATIVE` ABI passes and returns
`FUNC` as `uint32_t`; VM-aware built-ins receive the original `mvm_value_t`.
Full signature validation at the boundary is not yet implemented.

The compiler automatically creates a bytecode wrapper when an `extern` name
is used as a value. Such values can be stored in global and
local variables, struct fields, and arrays, returned, and passed as
arguments. The wrapper preserves the signature and forwards the variadic packet
through `VA_FORWARD`; direct `extern` calls use `NATIVE`.

## Relationship to the language draft

The current address format follows the `CALL` contract in OPSLEFT. It does not yet have
function tables, instruction-boundary checks, or signature checks: `argc` specifies
the number of passed slots but is not checked against the function declaration.
The validated image descriptors described in the
[M draft](M/02-types.md#28-function-type) will require a function table
and type metadata. Until then, the bytecode generator is responsible for
function entry addresses and matching parameters.

M nested functions and literals
[do not capture local variables](M/03-declarations.md#no-capture),
so an ordinary `FUNC` value is sufficient for them.
