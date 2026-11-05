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

# mvm synchronization primitives

mvm provides `Mutex`, `ConditionVariable`, scalar `Atomic` values, and a library
`Semaphore`. Handles belong to a single `mvm_t`, have stable identity,
allow `NULL`, compare by identity, and can be stored in arrays,
records, unions, and typed fields. A foreign, missing, or forged handle
produces `RUNTIME_ERROR`.

When loading `.mvm`, the VM registers built-ins before linking the image. An embedder
that creates a runtime without an image and calls bytecode manually must call
`mvm_thread_register` and `mvm_sync_register`. Repeated registration is idempotent;
a partially existing set is rejected, and a failed new registration rolls back its
new entries. Adding handle kinds changed the native ABI to version 3.

## Mutex

```m
let mutex(): Mutex
let lock(mutex: Mutex): Void
let tryLock(mutex: Mutex): Bool
let unlock(mutex: Mutex): Void
```

A mutex is nonrecursive. Repeated `lock` or `tryLock` by its owner, or `unlock`
without ownership, produces `RUNTIME_ERROR`. `tryLock` does not block and returns `false`
if another context holds the mutex. Successful acquisition has acquire semantics;
release has release semantics.

Thread completion or interruption does not release a mutex. The runtime checks ownership
itself and does not rely on differences between POSIX and Windows primitives.

## ConditionVariable

```m
let condition(): ConditionVariable
let wait(condition: ConditionVariable, mutex: Mutex): Void
let signal(condition: ConditionVariable): Void
let broadcast(condition: ConditionVariable): Void
```

`wait` requires mutex ownership. Enqueuing and releasing the mutex are atomic
with respect to `signal`/`broadcast`. Before returning from a normal wait and after
a cooperative interrupt, the mutex is reacquired. The predicate must be checked
in a loop because spurious wakeups are allowed:

```m
lock(mutex)
while (!ready) {
    wait(condition, mutex)
}
unlock(mutex)
```

`signal` wakes at most one waiter; `broadcast` wakes all waiters. A signal with no waiter
is not retained. A condition variable is not permanently bound to a single mutex. This
version has no timeout.

## Atomics

```m
let atomic<T>(initial: T): Atomic
let load<T>(atomic: Atomic): T
let store<T>(atomic: Atomic, value: T): Void
let exchange<T>(atomic: Atomic, value: T): T
let compareExchange<T>(atomic: Atomic, expected: T, desired: T): T
let add<T>(atomic: Atomic, value: T): T
let sub<T>(atomic: Atomic, value: T): T
```

The type is fixed at creation. Supported types are `Bool`, `Byte`, `Half`, `Word`, and
`Long`; unsigned and floating-point values are rejected. A type mismatch produces
`RUNTIME_ERROR`. `add`/`sub` are available only for signed integers, return
the previous value, and use modular overflow at the value's bit width.
`compareExchange` also returns the previous value, so success is determined
by comparing it with `expected`.

All public operations are sequentially consistent (`memory_order_seq_cst`).
Custom memory orders are unavailable. References, arrays, records, and their compound
operations do not become atomic.

## Semaphore

`base/sync.m` implements a semaphore using a mutex and condition variable:

```m
let semaphore(initial: Long): Semaphore
let acquire(semaphore: Semaphore): Void
let tryAcquire(semaphore: Semaphore): Bool
let release(semaphore: Semaphore): Void
```

The counter is nonnegative. A negative initial value or overflow during
`release` produces `RUNTIME_ERROR`. `acquire` waits for a nonzero counter in a loop;
`tryAcquire` does not block waiting for it; `release` increments the counter and wakes one
waiting context. Copies of `Semaphore` share state through a managed reference.
There is no separate semaphore OS object or runtime kind.

## GC, blocking, and shutdown

Before waiting, the execution context leaves the active GC state through the same
mechanism as `join`; built-in arguments remain GC roots. Stop-the-world GC does not
wait for a user mutex. After wakeup, execution resumes at a
safe boundary; the runtime mutex is not held during a user
wait.

`threadInterrupt` wakes `lock` and `wait` and returns `MVM_RESULT_INTERRUPT`.
For `wait`, the mutex is reacquired first. `mvm_destroy` wakes all
blocked workers; during runtime destruction, a wait may end without
reacquisition because further M code execution is prohibited. Workers first
leave the platform primitives, then resources are freed.

A handle and active operations retain a separate reference-counted control block.
If a handle is unreachable while the mutex is held or a wait is active, control block
destruction is deferred until the last active operation finishes. GC and finalizers never
perform an implicit `unlock`.

## C API

Declarations are in `include/mvm.h`:

- `mvm_mutex_new`, `mvm_mutex_lock`, `mvm_mutex_try_lock`, `mvm_mutex_unlock`;
- `mvm_condition_new`, `mvm_condition_wait`, `mvm_condition_signal`,
  `mvm_condition_broadcast`;
- `mvm_atomic_new`, `mvm_atomic_load`, `mvm_atomic_store`,
  `mvm_atomic_exchange`, `mvm_atomic_compare_exchange`, `mvm_atomic_add`,
  `mvm_atomic_sub`;
- `mvm_sync_register`.

Output parameters are unchanged on error. Creating a handle may trigger GC,
so live C values must be rooted according to the usual rules. Atomic operations and
notification operations do not allocate memory.
