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

// Private runtime definitions; this header is not part of the host API.
#include "mvm.h"

#define MVM_GC_INITIAL_THRESHOLD (1024 * 1024)

typedef enum mvm_object_kind {
  MVM_OBJECT_RAW,
  MVM_OBJECT_STRING,
  MVM_OBJECT_ARRAY,
  MVM_OBJECT_RECORD,
  MVM_OBJECT_TYPED,
  MVM_OBJECT_THREAD,
  MVM_OBJECT_PACKET,
  MVM_OBJECT_UNION,
  MVM_OBJECT_MUTEX,
  MVM_OBJECT_CONDITION,
  MVM_OBJECT_ATOMIC,
} mvm_object_kind_t;

#define MVM_TYPE_ID_NONE UINT32_MAX

typedef struct mvm_debug_line {
  uint32_t pc;
  const char *file;
  uint32_t line;
  uint32_t column;
} mvm_debug_line_t;

typedef struct mvm_debug_variable {
  uint32_t slot;
  uint32_t type_id;
  const char *name;
} mvm_debug_variable_t;

struct mvm_debug_global {
  uint32_t type_id;
  const char *name;
};

struct mvm_function_type {
  uint32_t address;
  uint32_t type_id;
  uint32_t max_stack;
  uint32_t code_size;
  uint32_t instruction_count;
  mvm_instruction_t *instructions;
  uint32_t *instruction_indices;
  const char *name;
  mvm_debug_line_t *debug_lines;
  size_t debug_line_count;
  mvm_debug_variable_t *debug_variables;
  size_t debug_variable_count;
};

struct mvm_type {
  const char *name;
  size_t size;
  size_t alignment;
  size_t field_count;
  mvm_type_field_t *fields;
  size_t reference_count;
  mvm_type_reference_t *references;
  mvm_type_tag_t tag;
  mvm_kind_t kind;
  uint32_t inner_type_id;
  uint32_t flags;
  uint16_t argc;
  uint32_t *param_type_ids;
  uint32_t result_type_id;
  size_t variant_count;
  uint32_t *variant_type_ids;
};

struct mvm_object {
  mvm_object_t *next;
  mvm_object_t *gray_next;
  size_t size;
  mvm_object_kind_t kind;
  uint32_t type_id;
  bool marked;
  max_align_t alignment;
  unsigned char data[];
};

struct mvm_array {
  size_t length;
  size_t capacity;
  mvm_kind_t kind;
  mvm_value_t *elements;
  uint32_t element_type_id;
};

struct mvm_union {
  uint32_t active_type_id;
  mvm_value_t payload;
};

struct mvm_record {
  size_t length;
  mvm_value_t fields[];
};

struct mvm_thread {
  struct mvm_thread_control *control;
};

struct mvm_mutex {
  struct mvm_sync_control *control;
};

struct mvm_condition {
  struct mvm_sync_control *control;
};

struct mvm_atomic {
  struct mvm_sync_control *control;
};

typedef struct mvm_execution {
  mvm_t *runtime;
  mvm_stack_t *stack;
  mvm_frame_t *frames;
  size_t frame_count;
  size_t frame_capacity;
  size_t frame_limit;
  size_t pc;
  mvm_function_type_t *function;
  bool working;
  atomic_bool executing;
  size_t execution_depth;
  mvm_result_t native_error;
  uint32_t file_error;
  struct mvm_thread_control *control;
  struct mvm_sync_waiter *waiting;
  struct mvm_execution *next;
} mvm_execution_t;

mvm_execution_t *mvm_current_execution(const mvm_t *mvm);

static inline mvm_stack_t *mvm_execution_stack_for(
    const mvm_t *mvm, const mvm_execution_t *execution) {
  return execution ? execution->stack : mvm->stack;
}

static inline mvm_stack_t *mvm_execution_stack(const mvm_t *mvm) {
  return mvm_execution_stack_for(mvm, mvm_current_execution(mvm));
}

static inline size_t *mvm_execution_pc_for(mvm_t *mvm,
                                           mvm_execution_t *execution) {
  return execution ? &execution->pc : &mvm->pc;
}

static inline size_t *mvm_execution_pc(mvm_t *mvm) {
  return mvm_execution_pc_for(mvm, mvm_current_execution(mvm));
}

static inline mvm_frame_t *mvm_execution_frames_for(mvm_t *mvm,
                                                    mvm_execution_t *execution) {
  return execution ? execution->frames : mvm->frames;
}

static inline mvm_frame_t *mvm_execution_frames(mvm_t *mvm) {
  return mvm_execution_frames_for(mvm, mvm_current_execution(mvm));
}

static inline void mvm_execution_set_frames_for(mvm_t *mvm,
                                                mvm_execution_t *execution,
                                                mvm_frame_t *frames) {
  if (execution)
    execution->frames = frames;
  else
    mvm->frames = frames;
}

static inline void mvm_execution_set_frames(mvm_t *mvm, mvm_frame_t *frames) {
  mvm_execution_set_frames_for(mvm, mvm_current_execution(mvm), frames);
}

static inline size_t *mvm_execution_frame_count_for(mvm_t *mvm,
                                                    mvm_execution_t *execution) {
  return execution ? &execution->frame_count : &mvm->frame_count;
}

static inline size_t *mvm_execution_frame_count(mvm_t *mvm) {
  return mvm_execution_frame_count_for(mvm, mvm_current_execution(mvm));
}

static inline size_t *mvm_execution_frame_capacity_for(mvm_t *mvm,
                                                       mvm_execution_t *execution) {
  return execution ? &execution->frame_capacity : &mvm->frame_capacity;
}

static inline size_t *mvm_execution_frame_capacity(mvm_t *mvm) {
  return mvm_execution_frame_capacity_for(mvm, mvm_current_execution(mvm));
}

static inline size_t mvm_execution_frame_limit_for(const mvm_t *mvm,
                                                   const mvm_execution_t *execution) {
  return execution ? execution->frame_limit : mvm->frame_limit;
}

static inline size_t mvm_execution_frame_limit(const mvm_t *mvm) {
  return mvm_execution_frame_limit_for(mvm, mvm_current_execution(mvm));
}

static inline bool *mvm_execution_working_for(mvm_t *mvm,
                                              mvm_execution_t *execution) {
  return execution ? &execution->working : &mvm->working;
}

static inline bool *mvm_execution_working(mvm_t *mvm) {
  return mvm_execution_working_for(mvm, mvm_current_execution(mvm));
}

static inline mvm_function_type_t *mvm_execution_function_for(
    mvm_t *mvm, mvm_execution_t *execution) {
  return execution ? execution->function : mvm->current_function;
}

static inline void mvm_execution_set_function_for(
    mvm_t *mvm, mvm_execution_t *execution, mvm_function_type_t *function) {
  if (execution)
    execution->function = function;
  else
    mvm->current_function = function;
}

static inline mvm_result_t *mvm_execution_native_error(mvm_t *mvm) {
  mvm_execution_t *execution = mvm_current_execution(mvm);
  return execution ? &execution->native_error : &mvm->native_error;
}

struct mvm_packet {
  size_t count;
  mvm_value_t values[];
};

static inline uint64_t mvm_read_u64(const unsigned char *data) {
  uint64_t value = 0;
  for (size_t i = 0; i < sizeof(value); ++i)
    value |= (uint64_t)data[i] << (i * 8);
  return value;
}

static inline void mvm_write_u64(unsigned char *data, uint64_t value) {
  for (size_t i = 0; i < sizeof(value); ++i)
    data[i] = (unsigned char)(value >> (i * 8));
}

static inline bool mvm_nullable_kind(mvm_kind_t kind) {
  return kind == MVM_KIND_STRING || kind == MVM_KIND_ARRAY ||
         kind == MVM_KIND_REFERENCE || kind == MVM_KIND_FUNC ||
         kind == MVM_KIND_THREAD || kind == MVM_KIND_MUTEX ||
         kind == MVM_KIND_CONDITION || kind == MVM_KIND_ATOMIC;
}

// Whether `width` operand bytes are available at pc.
static inline bool mvm_has_operand_for(const mvm_t *mvm,
                                       const mvm_execution_t *execution,
                                       const size_t width) {
  const size_t pc = execution ? execution->pc : mvm->pc;
  return mvm->ram && mvm->ram->data && pc <= mvm->ram->size &&
         width <= mvm->ram->size - pc;
}

static inline bool mvm_has_operand(const mvm_t *mvm, const size_t width) {
  return mvm_has_operand_for(mvm, mvm_current_execution(mvm), width);
}

// Stack index where the current frame starts; zero at the top level.
static inline size_t mvm_frame_base_for(const mvm_t *mvm,
                                        const mvm_execution_t *execution) {
  const size_t count = execution ? execution->frame_count : mvm->frame_count;
  if (!count)
    return 0;
  return execution ? execution->frames[count - 1].base
                   : mvm->frames[count - 1].base;
}

static inline size_t mvm_frame_base(const mvm_t *mvm) {
  return mvm_frame_base_for(mvm, mvm_current_execution(mvm));
}

bool mvm_runtime_init(mvm_t *mvm);
void mvm_execution_enter(mvm_t *mvm);
void mvm_execution_leave(mvm_t *mvm);
void mvm_execution_poll(mvm_t *mvm);
mvm_result_t mvm_step(mvm_t *mvm, mvm_execution_t *execution);
void mvm_debug_report(mvm_t *mvm, mvm_execution_t *execution,
                      mvm_result_t status, mvm_opcodes_t op,
                      size_t instruction_pc);
void mvm_runtime_lock(mvm_t *mvm);
void mvm_runtime_unlock(mvm_t *mvm);
bool mvm_runtime_gc_begin(mvm_t *mvm);
void mvm_runtime_gc_end(mvm_t *mvm);
bool mvm_thread_execution_interrupted(const mvm_execution_t *execution);
void mvm_thread_shutdown(mvm_t *mvm);
mvm_function_type_t *mvm_function_at(mvm_t *mvm, uint32_t address);
bool mvm_code_write_blocked(const mvm_t *mvm, size_t address, size_t width);

// Managed heap and object validation.
mvm_object_t *mvm_gc_find(const mvm_t *mvm, const void *pointer);
mvm_object_t *mvm_gc_allocate(mvm_t *mvm, mvm_object_kind_t kind, size_t size);
// Frees owned storage and updates gc_bytes; the caller unlinks the object.
void mvm_gc_free_object(mvm_t *mvm, mvm_object_t *object);
// The caller roots the array and validates capacity before this may collect.
bool mvm_gc_array_reserve(mvm_t *mvm, mvm_array_t *array, size_t capacity);
mvm_array_t *mvm_checked_array(const mvm_t *mvm, mvm_value_t value);

// String and value operations used by the interpreter.
mvm_result_t mvm_measure_string(const mvm_t *mvm, const mvm_value_t value,
                                size_t *byte_length, size_t *character_length);
mvm_result_t mvm_concat_strings(mvm_t *mvm, const mvm_value_t lhs,
                                const mvm_value_t rhs, mvm_value_t *result);
mvm_result_t mvm_string_get(const mvm_t *mvm, const mvm_value_t string,
                            const mvm_value_t index, mvm_value_t *result);
mvm_result_t mvm_string_from_bytes(mvm_t *mvm, const unsigned char *data,
                                   size_t length, mvm_value_t *result);
mvm_result_t mvm_array_new_primitive(mvm_t *mvm, mvm_kind_t kind,
                                     size_t length, mvm_value_t *result);
mvm_result_t mvm_value_index(const mvm_value_t value, size_t *index);
mvm_result_t mvm_compare_values(const mvm_t *mvm, const mvm_opcodes_t op,
                                const mvm_value_t lhs, const mvm_value_t rhs,
                                mvm_value_t *result);
mvm_result_t mvm_binary_arithmetic(const mvm_opcodes_t op,
                                   const mvm_value_t lhs, const mvm_value_t rhs,
                                   mvm_value_t *result);
mvm_result_t mvm_unary_arithmetic(const mvm_opcodes_t op,
                                  const mvm_value_t value, mvm_value_t *result);
mvm_result_t mvm_binary_bitwise(const mvm_opcodes_t op, const mvm_value_t lhs,
                                const mvm_value_t rhs, mvm_value_t *result);
mvm_result_t mvm_bitwise_not(const mvm_value_t value, mvm_value_t *result);
mvm_result_t mvm_cast_value(const mvm_value_t value, const mvm_kind_t kind,
                            mvm_value_t *result);

// Scalar memory access and typed object validation.
size_t mvm_ram_kind_width(const mvm_kind_t kind);
mvm_result_t mvm_load_ram(mvm_ram_t *ram, const size_t address,
                          const mvm_kind_t kind, mvm_value_t *value);
bool mvm_store_ram(mvm_ram_t *ram, const size_t address,
                   const mvm_value_t value);
mvm_object_t *mvm_typed_object(const mvm_t *mvm, const mvm_value_t value);
const mvm_type_t *mvm_type_by_id(const mvm_t *mvm, uint32_t type_id);
bool mvm_type_va_allowed(const mvm_t *mvm, uint32_t type_id);
mvm_result_t mvm_reference_type(mvm_t *mvm, uint32_t pointee_type_id,
                                bool mutable, uint32_t *type_id);
mvm_result_t mvm_ref_read(mvm_t *mvm, uint32_t reference_type_id,
                          mvm_value_t reference, mvm_value_t *result);
mvm_result_t mvm_ref_write(mvm_t *mvm, uint32_t reference_type_id,
                           mvm_value_t reference, mvm_value_t value);
mvm_result_t mvm_typed_init(mvm_t *mvm, uint32_t pointee_type_id,
                            mvm_value_t value, mvm_value_t *result);
mvm_result_t mvm_type_clone(mvm_t *destination, const mvm_t *source);
mvm_result_t mvm_type_load(mvm_t *mvm, const mvm_type_desc_t *descs,
                           size_t count);
bool mvm_type_defaultable(const mvm_t *mvm, uint32_t type_id);
bool mvm_value_matches_type(const mvm_t *mvm, uint32_t type_id, mvm_value_t value);
mvm_result_t mvm_union_new(mvm_t *mvm, uint32_t union_type_id,
                            uint32_t variant_type_id, mvm_value_t payload,
                            mvm_value_t *result);
mvm_result_t mvm_union_variantof(const mvm_t *mvm, mvm_value_t value,
                                  mvm_value_t *result);
mvm_result_t mvm_union_get(mvm_t *mvm, mvm_value_t value, uint32_t type_id,
                            mvm_value_t *result);
mvm_result_t mvm_union_widen(mvm_t *mvm, mvm_value_t value, uint32_t type_id,
                              mvm_value_t *result);
mvm_result_t mvm_typed_construct(mvm_t *mvm, uint32_t type_id, mvm_value_t *result);
mvm_result_t mvm_array_new_typed(mvm_t *mvm, uint32_t array_type_id,
                                  size_t length, mvm_value_t *result);
bool mvm_image_load(mvm_t *mvm, const char *path);

mvm_packet_t *mvm_checked_packet(const mvm_t *mvm, mvm_value_t value);
mvm_result_t mvm_packet_new(mvm_t *mvm, size_t count,
                            const mvm_value_t *values, size_t types_pc,
                            mvm_value_t *result);
mvm_result_t mvm_va_arg(mvm_t *mvm, const mvm_packet_t *packet, size_t index,
                        uint32_t type_id, mvm_value_t *result);

// Thread handles are stable GC objects; control blocks are shared by the handle,
// runtime and worker until each releases its reference.
mvm_thread_t *mvm_checked_thread(const mvm_t *mvm, mvm_value_t value);
void mvm_thread_release_handle(mvm_thread_t *thread);

mvm_mutex_t *mvm_checked_mutex(const mvm_t *mvm, mvm_value_t value);
mvm_condition_t *mvm_checked_condition(const mvm_t *mvm, mvm_value_t value);
mvm_atomic_t *mvm_checked_atomic(const mvm_t *mvm, mvm_value_t value);
bool mvm_sync_value_valid(const mvm_t *mvm, mvm_value_t value);
void mvm_mutex_release_handle(mvm_mutex_t *mutex);
void mvm_condition_release_handle(mvm_condition_t *condition);
void mvm_atomic_release_handle(mvm_atomic_t *atomic);
void mvm_sync_interrupt_execution(mvm_execution_t *execution);
bool mvm_sync_interrupted(mvm_t *mvm);
void mvm_sync_shutdown(mvm_t *mvm);

mvm_result_t mvm_file_register(mvm_t *mvm);
void mvm_file_shutdown(mvm_t *mvm);

mvm_result_t mvm_math_register(mvm_t *mvm);

// Native state is owned by native.c, including libffi and library handles.
void mvm_native_destroy(mvm_t *mvm);
void mvm_native_rewind(mvm_t *mvm, size_t count);
mvm_result_t mvm_native_defer(mvm_t *mvm, const char *name,
                              mvm_kind_t result_kind, uint8_t argc,
                              const mvm_kind_t *arg_kinds, bool variadic,
                              uint16_t *id);
mvm_result_t mvm_native_clone(mvm_t *destination, const mvm_t *source);
mvm_result_t mvm_native_bind_signature(mvm_t *mvm, uint16_t id,
                                       uint32_t result_type_id,
                                       const uint32_t *arg_type_ids);
mvm_result_t mvm_register_builtin_dynamic(mvm_t *mvm, const char *name,
                                          mvm_builtin_fn function,
                                          uint16_t *id);
bool mvm_native_describe(const mvm_t *mvm, uint16_t id, uint8_t *argc,
                         mvm_kind_t *result_kind, const mvm_kind_t **arg_kinds,
                         bool *variadic, bool *dynamic);
mvm_result_t mvm_execute_native(mvm_t *mvm, uint16_t id, uint8_t argc);

// Decode and execute one instruction.
mvm_result_t mvm_decode(const mvm_t *mvm, mvm_execution_t *execution,
                        const mvm_opcodes_t op, mvm_instruction_t *instruction);
mvm_result_t mvm_execute(mvm_t *mvm, mvm_execution_t *execution,
                         const mvm_instruction_t *instruction,
                         size_t instruction_pc);
