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

# mvm threads

This document describes the current OS thread implementation in the C runtime and the interfaces
[std/thread.m](../../mvm-c/examples/std/thread.m) and
[base/thread.m](../../mvm-lib/base/thread.m). Executable checks use the C API,
bytecode, and M programs.

## Execution model

`mvm_t` is a shared runtime instance for the main thread and all workers.
RAM, globals, type metadata, the natives table, dynamic libraries, and the GC heap
are shared. `mvm_thread_spawn` does not create or copy another mvm.

Each worker receives only a separate execution context: a value stack, frames,
PC, call-depth limit, `working` flag, and native-call error state. A `FUNC`
address refers to shared RAM. Passed references, strings, arrays, unions, and handles
remain values of the same runtime.

GC uses stop-the-world mark/sweep. Before collection, one collector stops
the other execution contexts at instruction boundaries and scans the main stack, all
worker stacks, globals, registered roots, and saved thread results.
Concurrent collection requests select one collector; the others wait at a safe
point. While blocking, `join` places the calling context in a safe state.

Shared data does not automatically synchronize an arbitrary program.
Native functions called concurrently by multiple workers must be
thread-safe. The host must finish registering types/natives and loading libraries
before starting workers.

## C API

```c
mvm_result_t mvm_thread_spawn(mvm_t *vm, mvm_value_t entry,
                              const mvm_value_t *arguments,
                              size_t argument_count, mvm_value_t *result);
mvm_result_t mvm_thread_join(mvm_t *vm, mvm_value_t thread,
                             mvm_value_t *result);
mvm_result_t mvm_thread_detach(mvm_t *vm, mvm_value_t thread);
mvm_result_t mvm_thread_interrupt(mvm_t *vm, mvm_value_t thread);
mvm_result_t mvm_thread_register(mvm_t *mvm);
```

- `spawn` requires a `FUNC` with an address inside RAM. Arguments are copied into the worker's local
  slots starting at 0; an empty list is allowed. If the image contains function
  metadata, argument counts and full types are checked before launch. The limit is
  `UINT8_MAX` arguments.
- A worker runs until the top-level `RETURN`. The result may have any
  ordinary M type except the internal `PACKET`; entry metadata also checks its type.
  The saved result remains a GC root after the worker's stack is destroyed.
- Failure to create an execution context, control block, or OS thread produces `VM_ERROR`;
  an invalid entry or signature mismatch produces `RUNTIME_ERROR`. The output parameter
  is unchanged on error.
- `join` waits for completion, performs the OS join once, and returns `mvm_value_t`.
  Repeated calls return the saved result or the same error. On a worker error,
  `result` is unchanged. Cooperative stop returns `MVM_RESULT_INTERRUPT`.
  Attempting to join the current thread produces `RUNTIME_ERROR`.
- `detach` is idempotent. After it, `join` produces `RUNTIME_ERROR`. Detaching after
  a completed join is allowed and does not change the saved result.
- `interrupt` sets an atomic flag. The worker checks it before each
  instruction. A native call already in progress is not interrupted.
- All operations check the handle kind and ownership by this runtime. A foreign,
  missing, or forged handle produces `RUNTIME_ERROR`.

A handle is an `MVM_KIND_THREAD` value and a GC object with stable identity.
It allows `NULL` in arrays and typed fields, compares by identity,
and is retained by ordinary roots. Removing the last handle detaches the worker
without waiting for it. The control block is kept alive by references from the handle, runtime, and worker.

`mvm_destroy` requests interruption of all remaining workers and waits for them,
including detached workers, before freeing shared objects and closing libraries. Therefore,
destruction may wait for a native call already in progress; there is no forced kill.

## Calling from bytecode and M

`mvm_thread_register` registers VM-aware built-ins; there are no new opcodes.
Repeated registration is idempotent.

| Name | Arguments | Result |
|---|---|---|
| `threadSpawn` | `FUNC entry, arbitrary values` | `THREAD` |
| `threadJoin` | `THREAD` | saved `mvm_value_t` |
| `threadDetach` | `THREAD` | `VOID` |
| `threadInterrupt` | `THREAD` | `VOID` |

`threadSpawn` and `threadJoin` are polymorphic built-ins. Their concrete M signatures
are stored in the image but bind to a single implementation. Ordinary native signatures
must still match exactly. In a variadic call, arguments are passed through
`VA_PACK`; packets support scalars, strings, arrays, references, structs, unions,
functions, `Type`, and `Thread`.

[std/thread.m](../../mvm-c/examples/std/thread.m) exports generic wrappers:

```m
let spawn<F>(entry: F, arguments: ...): Thread
let join<R>(thread: Thread): R
```

`F` is a concrete function type. The compiler uses the parameters of `F` as context for
variadic argument types and checks their count; the runtime rechecks
the signature against image metadata. The result type of `join` is specified explicitly.

```m
let answer(): Long { return 42 }
let concat(a: String, b: String): String { return a + b }

let first: Thread = spawn::<(): Long>(answer)
let second: Thread = spawn::<(a: String, b: String): String>(concat, "a", "b")
let number: Long = join::<Long>(first)
let text: String = join::<String>(second)
```

## `BaseThread<R>` container

`base/thread.m` exports `BaseThread<R>` and the `thread<F, R>` factory.
The factory starts a worker and returns a container with a `data: Thread` field and
`join`, `detach`, and `interrupt` operations. As with `Array<T>`, each operation
takes the container as its first argument and uses its `data`.
The built-in name `Thread` remains the type of the handle itself.

```m
import { BaseThread, thread } from "base/thread.m"

let add(left: Long, right: Long): Long {
    return left + right
}

let main(): Word {
    let worker: BaseThread<Long> = thread::<(left: Long, right: Long): Long, Long>(add, 40, 2)
    return (worker.join(worker) - 42) as Word
}
```

`F` specifies the full signature of `entry`; `R` must match its return type.
For a function without a result, use `BaseThread<Void>` and `R = Void`.
The result type is fixed when creating the container; `join` has no type
arguments of its own. Worker arguments use the same variadic mechanism
as `spawn`.

Copying a container preserves handle identity and the worker's shared state.
Repeated `join` returns the saved result; `detach` is idempotent;
after detaching an unjoined worker, `join` produces a runtime error.
`interrupt` sends a cooperative interruption request. Operations do not clear
`data` and preserve C API behavior, including worker errors.

Workers can create their own threads in the same runtime. `join` blocks only
the calling execution context. `threadInterrupt` also wakes waits in
`Mutex.lock` and `ConditionVariable.wait`; during shutdown, the runtime wakes all such
waits. Mutexes, condition variables, scalar atomics, and semaphores are described in
[synchronization primitives](sync.md). Channels, green threads, and automatic
synchronization of arbitrary shared objects are not implemented.
