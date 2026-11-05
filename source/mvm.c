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

#include "mvm/internal.h"

#include <stdio.h>
#include <stdlib.h>

mvm_function_type_t *mvm_function_at(mvm_t *mvm, const uint32_t address) {
  if (!mvm)
    return NULL;
  for (size_t i = 0; i < mvm->function_type_count; ++i)
    if (mvm->function_types[i].address == address)
      return &mvm->function_types[i];
  return NULL;
}

bool mvm_code_write_blocked(const mvm_t *mvm, const size_t address,
                            const size_t width) {
  if (!mvm || !width)
    return false;
  for (size_t i = 0; i < mvm->function_type_count; ++i) {
    const mvm_function_type_t *function = &mvm->function_types[i];
    if (function->code_size &&
        address < (size_t)function->address + function->code_size &&
        (size_t)function->address < address + width)
      return true;
  }
  return false;
}

mvm_t *mvm_create(const char *binary_path, size_t minimum_memory,
                  size_t maximum_memory, const size_t frame_limit) {
  mvm_memory_t *memory = mvm_memory_create(minimum_memory, maximum_memory);
  if (!memory)
    return NULL;
  mvm_t *mvm = mvm_memory_calloc(memory, 1, sizeof(mvm_t));
  if (!mvm) {
    mvm_memory_destroy(memory);
    return NULL;
  }
  mvm->memory = memory;
  mvm->binary_path = binary_path;
  mvm->ram = mvm_ram_create_managed(memory, 0);
  mvm->stack = mvm_stack_create_managed(memory, MVM_STACK_MIN_CAPACITY);
  if (!mvm->ram || !mvm->stack) {
    mvm_destroy(mvm);
    return NULL;
  }
  mvm->frame_limit = frame_limit;
  if (mvm->frame_limit > SIZE_MAX / sizeof(mvm_frame_t))
    mvm->frame_limit = SIZE_MAX / sizeof(mvm_frame_t);
  mvm->pc = MVM_PC_START;
  mvm->working = true;
  mvm->gc_next_collection = MVM_GC_INITIAL_THRESHOLD;
  if (!mvm_runtime_init(mvm)) {
    mvm_destroy(mvm);
    return NULL;
  }
  if (binary_path && (mvm_thread_register(mvm) != MVM_RESULT_OK ||
                      mvm_sync_register(mvm) != MVM_RESULT_OK ||
                      mvm_file_register(mvm) != MVM_RESULT_OK)) {
    mvm_destroy(mvm);
    return NULL;
  }
  if (binary_path && !mvm_image_load(mvm, binary_path)) {
    mvm_destroy(mvm);
    return NULL;
  }
  mvm_function_type_t *entry = mvm_function_at(mvm, (uint32_t)mvm->pc);
  if (entry && !mvm_stack_reserve(mvm->stack, entry->max_stack)) {
    mvm_destroy(mvm);
    return NULL;
  }
  mvm->current_function = entry;
  return mvm;
}

void mvm_destroy(mvm_t *mvm) {
  if (!mvm)
    return;
  mvm->working = false;
  mvm_thread_shutdown(mvm);
  mvm_file_shutdown(mvm);
  while (mvm->objects) {
    mvm_object_t *object = mvm->objects;
    mvm->objects = object->next;
    mvm_gc_free_object(mvm, object);
  }
  mvm_native_destroy(mvm);
  for (size_t i = 0; i < mvm->type_count; ++i) {
    mvm_memory_free((void *)mvm->types[i].name);
    mvm_memory_free(mvm->types[i].fields);
    mvm_memory_free(mvm->types[i].references);
    mvm_memory_free(mvm->types[i].param_type_ids);
    mvm_memory_free(mvm->types[i].variant_type_ids);
  }
  mvm_memory_free(mvm->types);
  for (size_t i = 0; i < mvm->function_type_count; ++i) {
    mvm_memory_free(mvm->function_types[i].instructions);
    mvm_memory_free(mvm->function_types[i].instruction_indices);
    mvm_memory_free((void *)mvm->function_types[i].name);
    for (size_t j = 0; j < mvm->function_types[i].debug_line_count; ++j)
      mvm_memory_free((void *)mvm->function_types[i].debug_lines[j].file);
    mvm_memory_free(mvm->function_types[i].debug_lines);
    for (size_t j = 0; j < mvm->function_types[i].debug_variable_count; ++j)
      mvm_memory_free((void *)mvm->function_types[i].debug_variables[j].name);
    mvm_memory_free(mvm->function_types[i].debug_variables);
  }
  mvm_memory_free(mvm->function_types);
  for (size_t i = 0; i < mvm->global_count; ++i)
    if (mvm->debug_globals)
      mvm_memory_free((void *)mvm->debug_globals[i].name);
  mvm_memory_free(mvm->debug_globals);
  mvm_memory_free(mvm->globals);
  mvm_memory_free(mvm->frames);
  mvm_stack_destroy(mvm->stack);
  mvm_ram_destroy(mvm->ram);
  mvm_memory_t *memory = mvm->memory;
  mvm_memory_free(mvm);
  mvm_memory_destroy(memory);
}

mvm_result_t mvm_step(mvm_t *mvm, mvm_execution_t *execution) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  mvm_execution_poll(mvm);
  size_t *pc = mvm_execution_pc_for(mvm, execution);
  const size_t instruction_pc = *pc;
  mvm_result_t status = MVM_RESULT_OK;
  if (!mvm_has_operand_for(mvm, execution, sizeof(uint8_t))) {
    fprintf(stderr, "[RUNTIME ERROR]: pc out of bounds at %zX\n", *pc);
    mvm_debug_report(mvm, execution, MVM_RESULT_RUNTIME_ERROR, MVM_OP_COUNT,
                     instruction_pc);
    return MVM_RESULT_RUNTIME_ERROR;
  }

  mvm_function_type_t *function = mvm_execution_function_for(mvm, execution);
  mvm_instruction_t decoded;
  const mvm_instruction_t *instruction = &decoded;
  mvm_opcodes_t op = MVM_OP_COUNT;
  const bool prepared = function && function->instructions &&
                        function->instruction_indices &&
                        instruction_pc >= function->address &&
                        instruction_pc - function->address < function->code_size;
  if (prepared) {
    const uint32_t index =
        function->instruction_indices[instruction_pc - function->address];
    if (!index) {
      mvm_debug_report(mvm, execution, MVM_RESULT_RUNTIME_ERROR, MVM_OP_COUNT,
                       instruction_pc);
      return MVM_RESULT_RUNTIME_ERROR;
    }
    instruction = &function->instructions[index - 1];
    op = instruction->op;
    ++*pc;
  } else {
    op = mvm_ram_peek_ubyte(mvm->ram, *pc);
    ++*pc;
    status = mvm_decode(mvm, execution, op, &decoded);
  }

  const size_t allocation_failures = mvm_memory_failures();
  if (status == MVM_RESULT_OK)
    status = mvm_execute(mvm, execution, instruction, instruction_pc);
  if (status == MVM_RESULT_VM_ERROR &&
      mvm_memory_failures() != allocation_failures)
    status = MVM_RESULT_OUT_OF_MEMORY;
  if (status != MVM_RESULT_OK && status != MVM_RESULT_BREAKPOINT &&
      status != MVM_RESULT_INTERRUPT)
    fprintf(stderr, "[RUNTIME ERROR]: opcode %i at %zX\n", op, instruction_pc);
  if (status != MVM_RESULT_OK && status != MVM_RESULT_INTERRUPT)
    mvm_debug_report(mvm, execution, status, op, instruction_pc);
  return status;
}

mvm_result_t mvm_run(mvm_t *mvm) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  mvm_execution_t *execution = mvm_current_execution(mvm);
  mvm_execution_enter(mvm);
  mvm_result_t status = MVM_RESULT_OK;
  bool *working = mvm_execution_working_for(mvm, execution);
  while (*working) {
    status = mvm_step(mvm, execution);
    if (status != MVM_RESULT_OK)
      break;
  }
  mvm_execution_leave(mvm);
  return status;
}

mvm_result_t mvm_update(mvm_t *mvm) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  mvm_execution_t *execution = mvm_current_execution(mvm);
  mvm_execution_enter(mvm);
  const mvm_result_t status = mvm_step(mvm, execution);
  mvm_execution_leave(mvm);
  return status;
}
