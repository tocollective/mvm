/*
 * Copyright 2026 tocollective contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#include "mvm/all.h"
#include "opcodes.h"
#include "value.h"
#include <stdatomic.h>

#define MVM_FRAME_DEFAULT_LIMIT 1024
#define MVM_PC_START 0
#define MVM_MEMORY_DEFAULT_MIN ((size_t)1024 * 1024)
#define MVM_MEMORY_DEFAULT_MAX ((size_t)256 * 1024 * 1024)

typedef enum mvm_result {
  MVM_RESULT_OK,
  MVM_RESULT_VM_ERROR,
  MVM_RESULT_RUNTIME_ERROR,
  MVM_RESULT_INTERRUPT,
  MVM_RESULT_OUT_OF_MEMORY,
  MVM_RESULT_BREAKPOINT,
} mvm_result_t;

#define MVM_FRAME_NO_PACKET SIZE_MAX

typedef struct mvm_frame {
  size_t return_pc;    // address to resume at after RETURN
  size_t call_pc;      // CALL/CALL_INDIRECT instruction in the caller
  size_t base;         // stack index of the callee's first argument
  size_t packet_index; // hidden variadic packet or MVM_FRAME_NO_PACKET
  struct mvm_function_type *caller_function;
} mvm_frame_t;

typedef struct mvm_object mvm_object_t;
typedef struct mvm_type mvm_type_t;
typedef struct mvm_function_type mvm_function_type_t;
typedef struct mvm_debug_global mvm_debug_global_t;

typedef enum mvm_type_tag {
  MVM_TYPE_AGGREGATE,
  MVM_TYPE_PRIMITIVE,
  MVM_TYPE_REFERENCE,
  MVM_TYPE_ARRAY,
  MVM_TYPE_FUNCTION,
  MVM_TYPE_ENUM,
  MVM_TYPE_UNION,
} mvm_type_tag_t;

#define MVM_TYPE_FLAG_MUTABLE 0x1
#define MVM_TYPE_FLAG_VARIADIC 0x2
#define MVM_TYPE_FLAG_NULLABLE 0x2

typedef struct mvm_type_field {
  size_t offset;
  size_t size;
  mvm_kind_t kind; // VOID denotes a nested aggregate field.
  uint32_t type_id;
  bool has_type_id;
} mvm_type_field_t;

typedef struct mvm_type_reference {
  size_t offset;
  mvm_kind_t kind; // GC-traced reference or handle kind.
} mvm_type_reference_t;

typedef struct mvm_type_desc {
  size_t size;
  size_t alignment;
  size_t field_count;
  const mvm_type_field_t *fields;
  size_t reference_count;
  const mvm_type_reference_t *references;
  mvm_type_tag_t tag;
  mvm_kind_t kind;
  uint32_t inner_type_id;
  uint32_t flags;
  uint16_t argc;
  const uint32_t *param_type_ids;
  uint32_t result_type_id;
  size_t variant_count;
  const uint32_t *variant_type_ids;
} mvm_type_desc_t;

// Caller-owned root range. Its storage and values must remain valid until
// unregistered; do not copy it or register it in multiple VMs simultaneously.
typedef struct mvm_gc_root {
  const mvm_value_t *values;
  size_t count;
  struct mvm_gc_root *next;
} mvm_gc_root_t;
typedef struct mvm_native mvm_native_t;
typedef struct mvm_native_library mvm_native_library_t;
typedef struct mvm_file_state mvm_file_state_t;

#define MVM_NATIVE_ABI_VERSION 3
#define MVM_NATIVE_EXPORTS_SYMBOL "mvm_get_native_exports"

typedef struct mvm_native_export {
  const char *name;
  void (*function)(void);
  mvm_kind_t result_kind;
  uint8_t argc;
  const mvm_kind_t *arg_kinds;
} mvm_native_export_t;

typedef struct mvm_native_exports {
  uint32_t abi_version;
  size_t count;
  const mvm_native_export_t *functions;
} mvm_native_exports_t;

typedef const mvm_native_exports_t *(*mvm_native_exports_fn)(void);

typedef struct mvm_instruction {
  mvm_opcodes_t op;
  size_t next_pc;
  union {
    mvm_value_t value;
    uint16_t local_index;
    uint16_t field_index;
    uint32_t global_index;
    uint32_t jump_address;
    uint32_t type_id;
    struct {
      uint32_t union_type_id, variant_type_id;
    } union_new;
    mvm_kind_t kind;
    struct {
      uint32_t address;
      uint8_t argc;
    } call;
    struct {
      uint16_t id;
      uint8_t argc;
    } native;
    struct {
      uint32_t count;
      size_t types_pc;
    } va_pack;
    struct {
      uint16_t left;
      uint16_t right;
      mvm_opcodes_t comparison;
      bool when_true;
      uint32_t address;
    } local_jump;
  } as;
} mvm_instruction_t;

typedef struct mvm {
  mvm_memory_t *memory;
  const char *binary_path;
  mvm_ram_t *ram;
  mvm_stack_t *stack;

  // Globals grow on demand when GLOBAL_SET writes past the current end.
  mvm_value_t *globals;
  size_t global_count;
  size_t global_capacity;
  mvm_debug_global_t *debug_globals;

  mvm_type_t *types;
  size_t type_count;
  size_t type_capacity;

  mvm_object_t *objects;
  mvm_gc_root_t *gc_roots;
  size_t gc_bytes;
  size_t gc_next_collection;
  size_t gc_collections;

  mvm_native_t *natives;
  size_t native_count;
  mvm_result_t native_error;
  mvm_native_library_t *native_libraries;
  mvm_file_state_t *files;
  uint32_t file_error;

  // Call frames grow on demand up to frame_limit (maximum call depth).
  mvm_frame_t *frames;
  size_t frame_count;
  size_t frame_capacity;
  size_t frame_limit;

  size_t pc;
  mvm_function_type_t *current_function;
  uint8_t entry_argc;
  bool working;
  bool debug;

  mvm_function_type_t *function_types;
  size_t function_type_count;

  struct mvm_execution *executions;
  struct mvm_sync_control *sync_controls;
  struct mvm_sync_waiter *waiting;
  void *runtime;
  atomic_bool executing;
  size_t execution_depth;
  bool shutting_down;
} mvm_t;

// Reserve minimum bytes initially; all VM-owned allocations share maximum.
// The linked code/rodata buffer is sized automatically. minimum may be zero;
// maximum must be positive and at least minimum. Failure leaves no instance.
mvm_t *mvm_create(const char *binary_path, size_t minimum_memory,
                  size_t maximum_memory, size_t frame_limit);
void mvm_destroy(mvm_t *mvm);
// Enables source locations and call stacks for runtime errors and breakpoints.
void mvm_set_debug(mvm_t *mvm, bool enabled);

// Nonmoving mark/sweep; returns the number of reclaimed objects. All active
// stack slots (including callers' locals), globals and registered roots are
// roots.
size_t mvm_gc_collect(mvm_t *mvm);
bool mvm_gc_root_register(mvm_t *mvm, mvm_gc_root_t *root,
                          const mvm_value_t *values, size_t count);
void mvm_gc_root_unregister(mvm_t *mvm, mvm_gc_root_t *root);

// Type descriptors are validated and copied. Fields and references must be
// sorted by offset and may not overlap other entries of the same kind.
mvm_result_t mvm_type_register(mvm_t *mvm, const mvm_type_desc_t *desc,
                               uint32_t *id);

// Closed union objects are immutable. Construction and aggregate extraction may
// collect; root live host values. Outputs are unchanged on failure.
bool mvm_type_defaultable(const mvm_t *mvm, uint32_t type_id);
mvm_result_t mvm_union_new(mvm_t *mvm, uint32_t union_type_id,
                           uint32_t variant_type_id, mvm_value_t payload,
                           mvm_value_t *result);
mvm_result_t mvm_union_variantof(const mvm_t *mvm, mvm_value_t value,
                                 mvm_value_t *result);
mvm_result_t mvm_union_get(mvm_t *mvm, mvm_value_t value, uint32_t type_id,
                           mvm_value_t *result);
mvm_result_t mvm_union_widen(mvm_t *mvm, mvm_value_t value, uint32_t type_id,
                             mvm_value_t *result);
mvm_result_t mvm_array_new_typed(mvm_t *mvm, uint32_t array_type_id,
                                 size_t length, mvm_value_t *result);

// Checked packet access for variadic native adapters. Packet values cannot be
// stored, returned or converted to ordinary M values.
mvm_result_t mvm_variadic_count(const mvm_t *mvm, mvm_value_t packet,
                                size_t *count);
mvm_result_t mvm_variadic_arg(mvm_t *mvm, mvm_value_t packet, size_t index,
                              uint32_t type_id, mvm_value_t *result);

// Allocation may collect. Root live C values before calling. New objects must
// be rooted before the next allocation. Outputs stay unchanged on failure.
// Raw objects are zeroed, max_align_t-aligned, and contain no traced fields.
mvm_result_t mvm_alloc(mvm_t *mvm, size_t size, mvm_value_t *result);
// Typed values are zeroed and have the exact size and alignment of their type.
mvm_result_t mvm_typed_alloc(mvm_t *mvm, uint32_t type_id, mvm_value_t *result);
mvm_result_t mvm_typed_zero(mvm_t *mvm, uint32_t type_id,
                            mvm_value_t destination);
mvm_result_t mvm_typed_copy(mvm_t *mvm, uint32_t type_id,
                            mvm_value_t destination, mvm_value_t source);
mvm_result_t mvm_typed_field_get(const mvm_t *mvm, mvm_value_t value,
                                 size_t index, mvm_value_t *result);
mvm_result_t mvm_typed_field_set(mvm_t *mvm, mvm_value_t value, size_t index,
                                 mvm_value_t field);
mvm_result_t mvm_typed_field_address(mvm_t *mvm, mvm_value_t value,
                                     size_t index, mvm_value_t *result);
// Length is limited to UINT32_MAX; VOID elements are invalid. Reference-bearing
// slots and handles default to NULL and accept NULL; scalar slots default
// to zero (TYPE: VOID). FUNC contains a bytecode address and is not traced.
mvm_result_t mvm_array_new(mvm_t *mvm, mvm_kind_t kind, size_t length,
                           mvm_value_t *result);
mvm_result_t mvm_array_get(const mvm_t *mvm, mvm_value_t array, size_t index,
                           mvm_value_t *result);
mvm_result_t mvm_array_set(mvm_t *mvm, mvm_value_t array, size_t index,
                           mvm_value_t value);
mvm_result_t mvm_array_length(const mvm_t *mvm, mvm_value_t array,
                              size_t *length);
// Arrays have stable identity; growth may collect and move private storage.
// These calls root their arguments while growing. C values retained across
// calls still need roots. Length/capacity are limited to UINT32_MAX. Reserve
// never shrinks; resize initializes added slots and clears removed references.
// Mutations and outputs remain unchanged on failure. Pop/remove return an
// unrooted value: root it before the next allocation. Empty pop is an error.
mvm_result_t mvm_array_capacity(const mvm_t *mvm, mvm_value_t array,
                                size_t *capacity);
mvm_result_t mvm_array_reserve(mvm_t *mvm, mvm_value_t array, size_t capacity);
mvm_result_t mvm_array_resize(mvm_t *mvm, mvm_value_t array, size_t length);
mvm_result_t mvm_array_push(mvm_t *mvm, mvm_value_t array, mvm_value_t value);
mvm_result_t mvm_array_pop(mvm_t *mvm, mvm_value_t array, mvm_value_t *result);
mvm_result_t mvm_array_insert(mvm_t *mvm, mvm_value_t array, size_t index,
                              mvm_value_t value);
mvm_result_t mvm_array_remove(mvm_t *mvm, mvm_value_t array, size_t index,
                              mvm_value_t *result);
// Records are managed REFERENCE values. Fields default to NULL, accept any
// value, and are traced by GC. Field indices are zero-based.
mvm_result_t mvm_record_new(mvm_t *mvm, size_t length, mvm_value_t *result);
mvm_result_t mvm_record_get(const mvm_t *mvm, mvm_value_t record, size_t index,
                            mvm_value_t *result);
mvm_result_t mvm_record_set(mvm_t *mvm, mvm_value_t record, size_t index,
                            mvm_value_t value);

typedef mvm_result_t (*mvm_builtin_fn)(mvm_t *mvm, const mvm_value_t *args,
                                       mvm_value_t *result);
mvm_result_t mvm_register_native(mvm_t *mvm, const char *name,
                                 void (*function)(void), mvm_kind_t result_kind,
                                 uint8_t argc, const mvm_kind_t *arg_kinds,
                                 uint16_t *id);
mvm_result_t mvm_register_builtin(mvm_t *mvm, const char *name,
                                  mvm_builtin_fn function,
                                  mvm_kind_t result_kind, uint8_t argc,
                                  const mvm_kind_t *arg_kinds, uint16_t *id);
// A variadic adapter receives fixed arguments followed by one PACKET value.
mvm_result_t mvm_register_builtin_variadic(mvm_t *mvm, const char *name,
                                           mvm_builtin_fn function,
                                           mvm_kind_t result_kind,
                                           uint8_t fixed_argc,
                                           const mvm_kind_t *fixed_arg_kinds,
                                           uint16_t *id);
bool mvm_find_native(const mvm_t *mvm, const char *name, uint16_t *id);
// OK: exports registered. RUNTIME_ERROR: path/library rejected, including null
// or empty paths. VM_ERROR: invalid VM or resource/internal failure. Failure
// closes any opened handle and rolls back registrations from this attempt.
// Loader diagnostics are written to stderr.
mvm_result_t mvm_load_native_library(mvm_t *mvm, const char *path);
// Disable the most recent load with this exact path, preserving native IDs.
// Active calls and clones retain the OS handle until their last reference ends.
// RUNTIME_ERROR: null/empty/unknown path. VM_ERROR: null VM.
mvm_result_t mvm_dispose_native_library(mvm_t *mvm, const char *path);

// Workers share RAM, globals, types, natives and the GC heap with their
// runtime. Each worker owns only its execution stack, frames and program
// counter. Entry receives the supplied values as locals; function metadata is
// checked when available. The saved result remains a GC root until the runtime
// is destroyed.
mvm_result_t mvm_thread_spawn(mvm_t *mvm, mvm_value_t entry,
                              const mvm_value_t *arguments,
                              size_t argument_count, mvm_value_t *result);
mvm_result_t mvm_thread_join(mvm_t *mvm, mvm_value_t thread,
                             mvm_value_t *result);
mvm_result_t mvm_thread_detach(mvm_t *mvm, mvm_value_t thread);
mvm_result_t mvm_thread_interrupt(mvm_t *mvm, mvm_value_t thread);
mvm_result_t mvm_thread_register(mvm_t *mvm);

// Synchronization handles are stable GC objects belonging to one runtime.
// Mutexes are nonrecursive and are not unlocked automatically on thread exit.
mvm_result_t mvm_mutex_new(mvm_t *mvm, mvm_value_t *result);
mvm_result_t mvm_mutex_lock(mvm_t *mvm, mvm_value_t mutex);
mvm_result_t mvm_mutex_try_lock(mvm_t *mvm, mvm_value_t mutex, bool *acquired);
mvm_result_t mvm_mutex_unlock(mvm_t *mvm, mvm_value_t mutex);
mvm_result_t mvm_condition_new(mvm_t *mvm, mvm_value_t *result);
mvm_result_t mvm_condition_wait(mvm_t *mvm, mvm_value_t condition,
                                mvm_value_t mutex);
mvm_result_t mvm_condition_signal(mvm_t *mvm, mvm_value_t condition);
mvm_result_t mvm_condition_broadcast(mvm_t *mvm, mvm_value_t condition);
// Atomic values have a fixed BOOL or signed-integer kind. add/sub return the
// previous value and use modular arithmetic at that kind's width.
mvm_result_t mvm_atomic_new(mvm_t *mvm, mvm_value_t initial,
                            mvm_value_t *result);
mvm_result_t mvm_atomic_load(mvm_t *mvm, mvm_value_t atomic,
                             mvm_value_t *result);
mvm_result_t mvm_atomic_store(mvm_t *mvm, mvm_value_t atomic,
                              mvm_value_t value);
mvm_result_t mvm_atomic_exchange(mvm_t *mvm, mvm_value_t atomic,
                                 mvm_value_t value, mvm_value_t *result);
mvm_result_t mvm_atomic_compare_exchange(mvm_t *mvm, mvm_value_t atomic,
                                         mvm_value_t expected,
                                         mvm_value_t desired,
                                         mvm_value_t *result);
mvm_result_t mvm_atomic_add(mvm_t *mvm, mvm_value_t atomic, mvm_value_t value,
                            mvm_value_t *result);
mvm_result_t mvm_atomic_sub(mvm_t *mvm, mvm_value_t atomic, mvm_value_t value,
                            mvm_value_t *result);
mvm_result_t mvm_sync_register(mvm_t *mvm);

// Runs the current execution until RETURN or an error. The current context is
// entered once; GC safepoints are still observed between instructions.
mvm_result_t mvm_run(mvm_t *mvm);
mvm_result_t mvm_update(mvm_t *mvm);
