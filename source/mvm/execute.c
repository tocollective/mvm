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

#include "internal.h"
#include "numeric_cast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mvm_result_t mvm_reserve_globals(mvm_t *mvm, const size_t count) {
  if (count <= mvm->global_count)
    return MVM_RESULT_OK;
  if (count > SIZE_MAX / sizeof(mvm_value_t))
    return MVM_RESULT_VM_ERROR;

  if (count > mvm->global_capacity) {
    const size_t max_capacity = SIZE_MAX / sizeof(mvm_value_t);
    size_t capacity = mvm->global_capacity > 0 ? mvm->global_capacity : 8;
    while (capacity < count) {
      if (capacity > max_capacity / 2) {
        capacity = max_capacity;
        break;
      }
      capacity *= 2;
    }

    mvm_value_t *globals = mvm_memory_realloc(mvm->memory, mvm->globals,
                                              capacity * sizeof(mvm_value_t));
    if (!globals)
      return MVM_RESULT_VM_ERROR;
    mvm->globals = globals;
    mvm->global_capacity = capacity;
  }

  for (size_t i = mvm->global_count; i < count; ++i)
    mvm->globals[i] = (mvm_value_t){.kind = MVM_KIND_NULL};
  mvm->global_count = count;
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_reserve_function_stack(mvm_t *mvm,
                                               mvm_execution_t *execution,
                                               const uint32_t address,
                                               const size_t base,
                                               const size_t argc) {
  const mvm_function_type_t *function = mvm_function_at(mvm, address);
  if (!function)
    return MVM_RESULT_OK;
  if (function->max_stack < argc)
    return MVM_RESULT_RUNTIME_ERROR;
  if (function->max_stack > SIZE_MAX - base)
    return MVM_RESULT_VM_ERROR;
  return mvm_stack_reserve(mvm_execution_stack_for(mvm, execution),
                           base + function->max_stack)
             ? MVM_RESULT_OK
             : MVM_RESULT_VM_ERROR;
}

static mvm_result_t mvm_push_frame(mvm_t *mvm, mvm_execution_t *execution,
                                   const mvm_frame_t frame) {
  if (*mvm_execution_frame_count_for(mvm, execution) ==
      mvm_execution_frame_limit_for(mvm, execution)) {
    // pc already points past the CALL opcode.
    fprintf(stderr, "[RUNTIME ERROR]: Call stack overflow at %zX\n",
            *mvm_execution_pc_for(mvm, execution) - 1);
    return MVM_RESULT_RUNTIME_ERROR;
  }

  if (*mvm_execution_frame_count_for(mvm, execution) ==
      *mvm_execution_frame_capacity_for(mvm, execution)) {
    size_t capacity = *mvm_execution_frame_capacity_for(mvm, execution) > 0
                          ? *mvm_execution_frame_capacity_for(mvm, execution) * 2
                          : 8;
    if (capacity > mvm_execution_frame_limit_for(mvm, execution))
      capacity = mvm_execution_frame_limit_for(mvm, execution);

    mvm_frame_t *frames = mvm_memory_realloc(
        mvm->memory, mvm_execution_frames_for(mvm, execution),
        capacity * sizeof(mvm_frame_t));
    if (!frames)
      return MVM_RESULT_VM_ERROR;
    mvm_execution_set_frames_for(mvm, execution, frames);
    *mvm_execution_frame_capacity_for(mvm, execution) = capacity;
  }

  mvm_execution_frames_for(mvm, execution)[
      (*mvm_execution_frame_count_for(mvm, execution))++] = frame;
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_call_packet(mvm_t *mvm, mvm_execution_t *execution,
                                    const size_t base, const size_t argc,
                                    size_t *packet_index) {
  mvm_stack_t *stack = mvm_execution_stack_for(mvm, execution);
  *packet_index = MVM_FRAME_NO_PACKET;
  for (size_t i = 0; i < argc; ++i) {
    const mvm_value_t value = stack->data[base + i];
    if (value.kind != MVM_KIND_PACKET)
      continue;
    if (i + 1 != argc || !mvm_checked_packet(mvm, value))
      return MVM_RESULT_RUNTIME_ERROR;
    *packet_index = base + i;
  }
  return MVM_RESULT_OK;
}

static mvm_packet_t *mvm_current_packet(mvm_t *mvm,
                                        mvm_execution_t *execution) {
  mvm_stack_t *stack = mvm_execution_stack_for(mvm, execution);
  if (!*mvm_execution_frame_count_for(mvm, execution))
    return NULL;
  const mvm_frame_t *frame = &mvm_execution_frames_for(mvm, execution)[
      *mvm_execution_frame_count_for(mvm, execution) - 1];
  if (frame->packet_index == MVM_FRAME_NO_PACKET ||
      frame->packet_index >= stack->size)
    return NULL;
  return mvm_checked_packet(mvm, stack->data[frame->packet_index]);
}

static mvm_result_t mvm_unary_fast(const mvm_opcodes_t op,
                                   const mvm_value_t value,
                                   mvm_value_t *result) {
  if (op != MVM_OP_NEG &&
      (value.kind == MVM_KIND_ULONG || value.kind == MVM_KIND_SLONG)) {
    *result = (mvm_value_t){.kind = value.kind};
    if (value.kind == MVM_KIND_ULONG) {
      result->as.ul = op == MVM_OP_INC ? value.as.ul + 1 : value.as.ul - 1;
    } else {
      const uint64_t bits = op == MVM_OP_INC ? (uint64_t)value.as.sl + 1
                                             : (uint64_t)value.as.sl - 1;
      memcpy(&result->as.sl, &bits, sizeof(bits));
    }
    return MVM_RESULT_OK;
  }
  return mvm_unary_arithmetic(op, value, result);
}

static mvm_result_t mvm_compare_fast(const mvm_t *mvm, mvm_opcodes_t op,
                                     mvm_value_t lhs, mvm_value_t rhs,
                                     bool *comparison) {
  if (lhs.kind == rhs.kind) {
#define MVM_COMPARE_NUMERIC(kind, field)                                        \
    case kind:                                                                  \
      switch (op) {                                                             \
      case MVM_OP_EQ: *comparison = lhs.as.field == rhs.as.field; break;        \
      case MVM_OP_NE: *comparison = lhs.as.field != rhs.as.field; break;        \
      case MVM_OP_LT: *comparison = lhs.as.field < rhs.as.field; break;         \
      case MVM_OP_LE: *comparison = lhs.as.field <= rhs.as.field; break;        \
      case MVM_OP_GT: *comparison = lhs.as.field > rhs.as.field; break;         \
      default: *comparison = lhs.as.field >= rhs.as.field; break;               \
      }                                                                         \
      return MVM_RESULT_OK
    switch (lhs.kind) {
      MVM_COMPARE_NUMERIC(MVM_KIND_UBYTE, ub);
      MVM_COMPARE_NUMERIC(MVM_KIND_UHALF, uh);
      MVM_COMPARE_NUMERIC(MVM_KIND_UWORD, uw);
      MVM_COMPARE_NUMERIC(MVM_KIND_ULONG, ul);
      MVM_COMPARE_NUMERIC(MVM_KIND_SBYTE, sb);
      MVM_COMPARE_NUMERIC(MVM_KIND_SHALF, sh);
      MVM_COMPARE_NUMERIC(MVM_KIND_SWORD, sw);
      MVM_COMPARE_NUMERIC(MVM_KIND_SLONG, sl);
      MVM_COMPARE_NUMERIC(MVM_KIND_FLOAT, f);
      MVM_COMPARE_NUMERIC(MVM_KIND_DOUBLE, d);
    default:
      break;
    }
#undef MVM_COMPARE_NUMERIC
  }
  mvm_value_t result;
  const mvm_result_t status = mvm_compare_values(mvm, op, lhs, rhs, &result);
  if (status == MVM_RESULT_OK)
    *comparison = result.as.b;
  return status;
}

mvm_result_t mvm_execute(mvm_t *mvm, mvm_execution_t *execution,
                         const mvm_instruction_t *instruction,
                         size_t instruction_pc) {
  mvm_stack_t *stack = mvm_execution_stack_for(mvm, execution);
  switch (instruction->op) {
  case MVM_OP_BREAKPOINT:
    return MVM_RESULT_BREAKPOINT;
  case MVM_OP_NULL_CHECK:
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    if (stack->size <= mvm_frame_base_for(mvm, execution) ||
        stack->data[stack->size - 1].kind == MVM_KIND_NULL)
      return MVM_RESULT_RUNTIME_ERROR;
    return MVM_RESULT_OK;
  case MVM_OP_RETURN: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;

    // Every function returns exactly one value from the top of its frame.
    const size_t base = mvm_frame_base_for(mvm, execution);
    if (stack->size <= base ||
        stack->data[stack->size - 1].kind == MVM_KIND_PACKET)
      return MVM_RESULT_RUNTIME_ERROR;

    if (*mvm_execution_frame_count_for(mvm, execution) == 0) {
      // Top-level return ends the program; the result stays on the stack.
      *mvm_execution_working_for(mvm, execution) = false;
      *mvm_execution_pc_for(mvm, execution) = instruction->next_pc;
      return MVM_RESULT_OK;
    }

    // Replace the callee's arguments and locals with its result.
    const mvm_frame_t frame = mvm_execution_frames_for(mvm, execution)[--*mvm_execution_frame_count_for(mvm, execution)];
    stack->data[frame.base] = stack->data[stack->size - 1];
    mvm_stack_truncate(stack, frame.base + 1);
    *mvm_execution_pc_for(mvm, execution) = frame.return_pc;
    mvm_execution_set_function_for(mvm, execution, frame.caller_function);
    return MVM_RESULT_OK;
  }
  case MVM_OP_CALL:
  case MVM_OP_CALL_INDIRECT: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;

    const bool indirect = instruction->op == MVM_OP_CALL_INDIRECT;
    const uint8_t argc = instruction->as.call.argc;
    // The function value is above the arguments and is not a callee local.
    // Check everything, including frame allocation, before consuming it.
    const size_t base = mvm_frame_base_for(mvm, execution);
    const size_t required = (size_t)argc + (indirect ? 1 : 0);
    if (stack->size < base || stack->size - base < required)
      return MVM_RESULT_RUNTIME_ERROR;
    uint32_t address = instruction->as.call.address;
    if (indirect) {
      const mvm_value_t function = stack->data[stack->size - 1];
      if (function.kind != MVM_KIND_FUNC)
        return MVM_RESULT_RUNTIME_ERROR;
      address = function.as.func;
    }
    if (address >= mvm->ram->size)
      return MVM_RESULT_RUNTIME_ERROR;

    const size_t args_base = stack->size - required;
    size_t packet_index;
    const mvm_result_t packet_status =
        mvm_call_packet(mvm, execution, args_base, argc, &packet_index);
    if (packet_status != MVM_RESULT_OK)
      return packet_status;
    const mvm_result_t reserve_status =
        mvm_reserve_function_stack(mvm, execution, address, args_base, required);
    if (reserve_status != MVM_RESULT_OK)
      return reserve_status;
    const mvm_frame_t frame = {
        .return_pc = instruction->next_pc,
        .call_pc = instruction_pc,
        .base = args_base,
        .packet_index = packet_index,
        .caller_function = mvm_execution_function_for(mvm, execution),
    };
    const mvm_result_t result = mvm_push_frame(mvm, execution, frame);
    if (result != MVM_RESULT_OK)
      return result;
    if (indirect)
      --stack->size;
    *mvm_execution_pc_for(mvm, execution) = address;
    mvm_execution_set_function_for(mvm, execution, mvm_function_at(mvm, address));
    return MVM_RESULT_OK;
  }
  case MVM_OP_TAIL_CALL: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;

    const size_t base = mvm_frame_base_for(mvm, execution);
    const size_t argc = instruction->as.call.argc;
    if (stack->size < base || stack->size - base < argc)
      return MVM_RESULT_RUNTIME_ERROR;
    const uint32_t address = instruction->as.call.address;
    if (address >= mvm->ram->size)
      return MVM_RESULT_RUNTIME_ERROR;

    const size_t args_base = stack->size - argc;
    size_t packet_index;
    const mvm_result_t packet_status =
        mvm_call_packet(mvm, execution, args_base, argc, &packet_index);
    if (packet_status != MVM_RESULT_OK)
      return packet_status;
    const mvm_result_t reserve_status =
        mvm_reserve_function_stack(mvm, execution, address, base, argc);
    if (reserve_status != MVM_RESULT_OK)
      return reserve_status;
    const size_t packet_offset = packet_index == MVM_FRAME_NO_PACKET
                                     ? 0
                                     : packet_index - args_base;
    if (argc > 0) {
      memmove(stack->data + base, stack->data + args_base,
              argc * sizeof(mvm_value_t));
    }
    mvm_stack_truncate(stack, base + argc);
    if (*mvm_execution_frame_count_for(mvm, execution)) {
      mvm_execution_frames_for(mvm, execution)[*mvm_execution_frame_count_for(mvm, execution) - 1].packet_index =
          packet_index == MVM_FRAME_NO_PACKET ? MVM_FRAME_NO_PACKET
                                              : base + packet_offset;
    }
    *mvm_execution_pc_for(mvm, execution) = address;
    mvm_execution_set_function_for(mvm, execution, mvm_function_at(mvm, address));
    return MVM_RESULT_OK;
  }
  case MVM_OP_NATIVE: {
    const mvm_result_t status = mvm_execute_native(
        mvm, instruction->as.native.id, instruction->as.native.argc);
    if (status != MVM_RESULT_OK)
      return status;
  } break;
  case MVM_OP_JUMP:
    if (instruction->as.jump_address >= mvm->ram->size)
      return MVM_RESULT_RUNTIME_ERROR;
    *mvm_execution_pc_for(mvm, execution) = instruction->as.jump_address;
    return MVM_RESULT_OK;
  case MVM_OP_JUMP_IF_FALSE:
  case MVM_OP_JUMP_IF_TRUE: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    if (instruction->as.jump_address >= mvm->ram->size)
      return MVM_RESULT_RUNTIME_ERROR;
    if (stack->size <= mvm_frame_base_for(mvm, execution))
      return MVM_RESULT_RUNTIME_ERROR;

    const size_t top = stack->size - 1;
    const mvm_value_t condition = stack->data[top];
    if (condition.kind != MVM_KIND_BOOL)
      return MVM_RESULT_RUNTIME_ERROR;
    --stack->size;

    const bool jump = instruction->op == MVM_OP_JUMP_IF_TRUE ? condition.as.b
                                                             : !condition.as.b;
    if (jump) {
      *mvm_execution_pc_for(mvm, execution) = instruction->as.jump_address;
      return MVM_RESULT_OK;
    }
  } break;
  case MVM_OP_LOCAL_GET:
  case MVM_OP_LOCAL_SET: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;

    const size_t base = mvm_frame_base_for(mvm, execution);
    const size_t index = instruction->as.local_index;
    if (index >= stack->size - base)
      return MVM_RESULT_RUNTIME_ERROR;
    const size_t slot = base + index;
    if ((*mvm_execution_frame_count_for(mvm, execution) &&
         mvm_execution_frames_for(mvm, execution)[*mvm_execution_frame_count_for(mvm, execution) - 1].packet_index == slot) ||
        (instruction->op == MVM_OP_LOCAL_SET &&
         stack->data[stack->size - 1].kind == MVM_KIND_PACKET))
      return MVM_RESULT_RUNTIME_ERROR;

    if (instruction->op == MVM_OP_LOCAL_GET) {
      if (!mvm_stack_push(stack, stack->data[slot]))
        return MVM_RESULT_VM_ERROR;
    } else {
      stack->data[slot] = stack->data[stack->size - 1];
      --stack->size;
    }
  } break;
  case MVM_OP_GLOBAL_GET:
  case MVM_OP_GLOBAL_SET: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;

    const uint32_t index = instruction->as.global_index;
    if (instruction->op == MVM_OP_GLOBAL_SET &&
        (stack->size <= mvm_frame_base_for(mvm, execution) ||
         stack->data[stack->size - 1].kind == MVM_KIND_PACKET))
      return MVM_RESULT_RUNTIME_ERROR;
    mvm_runtime_lock(mvm);
    mvm_result_t status = MVM_RESULT_OK;
    if (instruction->op == MVM_OP_GLOBAL_GET) {
      if ((size_t)index >= mvm->global_count)
        status = MVM_RESULT_RUNTIME_ERROR;
      else if (!mvm_stack_push(stack, mvm->globals[index]))
        status = MVM_RESULT_VM_ERROR;
    } else {
      if ((uint64_t)index + 1 > SIZE_MAX / sizeof(mvm_value_t))
        status = MVM_RESULT_VM_ERROR;
      else
        status = mvm_reserve_globals(mvm, (size_t)index + 1);
      if (status == MVM_RESULT_OK) {
        mvm->globals[index] = stack->data[stack->size - 1];
        --stack->size;
      }
    }
    mvm_runtime_unlock(mvm);
    if (status != MVM_RESULT_OK)
      return status;
  } break;
  case MVM_OP_LIT_UBYTE:
  case MVM_OP_LIT_UHALF:
  case MVM_OP_LIT_UWORD:
  case MVM_OP_LIT_ULONG:
  case MVM_OP_LIT_SBYTE:
  case MVM_OP_LIT_SHALF:
  case MVM_OP_LIT_SWORD:
  case MVM_OP_LIT_SLONG:
  case MVM_OP_LIT_FLOAT:
  case MVM_OP_LIT_DOUBLE:
  case MVM_OP_LIT_NULL:
  case MVM_OP_LIT_FALSE:
  case MVM_OP_LIT_TRUE:
  case MVM_OP_LIT_STRING:
  case MVM_OP_LIT_FUNC:
    if (!mvm_stack_push(stack, instruction->as.value))
      return MVM_RESULT_VM_ERROR;
    break;
  case MVM_OP_ALLOC:
  case MVM_OP_ARRAY_NEW: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    if (stack->size <= mvm_frame_base_for(mvm, execution))
      return MVM_RESULT_RUNTIME_ERROR;
    const size_t top = stack->size - 1;
    size_t size;
    mvm_result_t status = mvm_value_index(stack->data[top], &size);
    if (status != MVM_RESULT_OK)
      return status;
    mvm_value_t result;
    status = instruction->op == MVM_OP_ALLOC
                 ? mvm_alloc(mvm, size, &result)
                 : mvm_array_new(mvm, instruction->as.kind, size, &result);
    if (status != MVM_RESULT_OK)
      return status;
    stack->data[top] = result;
  } break;
  case MVM_OP_ARRAY_GET:
  case MVM_OP_ARRAY_SET:
  case MVM_OP_ARRAY_LEN:
  case MVM_OP_ARRAY_CAP:
  case MVM_OP_ARRAY_RESERVE:
  case MVM_OP_ARRAY_RESIZE:
  case MVM_OP_ARRAY_PUSH:
  case MVM_OP_ARRAY_POP:
  case MVM_OP_ARRAY_INSERT:
  case MVM_OP_ARRAY_REMOVE: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    const mvm_opcodes_t op = instruction->op;
    const size_t count = op == MVM_OP_ARRAY_LEN || op == MVM_OP_ARRAY_CAP ||
                                 op == MVM_OP_ARRAY_POP
                             ? 1
                         : op == MVM_OP_ARRAY_SET || op == MVM_OP_ARRAY_INSERT
                             ? 3
                             : 2;
    if (stack->size - mvm_frame_base_for(mvm, execution) < count)
      return MVM_RESULT_RUNTIME_ERROR;
    const size_t slot = stack->size - count;
    for (size_t i = 0; i < count; ++i) {
      if (stack->data[slot + i].kind == MVM_KIND_PACKET)
        return MVM_RESULT_RUNTIME_ERROR;
    }
    const mvm_value_t array = stack->data[slot];
    mvm_value_t result;
    mvm_result_t status;
    bool returns_value = false;
    if (op == MVM_OP_ARRAY_LEN || op == MVM_OP_ARRAY_CAP) {
      size_t size;
      status = op == MVM_OP_ARRAY_LEN ? mvm_array_length(mvm, array, &size)
                                      : mvm_array_capacity(mvm, array, &size);
      if (status != MVM_RESULT_OK)
        return status;
      result = (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = (uint32_t)size};
      returns_value = true;
    } else if (op == MVM_OP_ARRAY_POP) {
      status = mvm_array_pop(mvm, array, &result);
      returns_value = true;
    } else if (op == MVM_OP_ARRAY_PUSH) {
      status = mvm_array_push(mvm, array, stack->data[slot + 1]);
    } else {
      size_t index;
      status = mvm_value_index(stack->data[slot + 1], &index);
      if (status != MVM_RESULT_OK)
        return status;
      switch (op) {
      case MVM_OP_ARRAY_SET:
        status = mvm_array_set(mvm, array, index, stack->data[slot + 2]);
        break;
      case MVM_OP_ARRAY_INSERT:
        status =
            mvm_array_insert(mvm, array, index, stack->data[slot + 2]);
        break;
      case MVM_OP_ARRAY_RESERVE:
        status = mvm_array_reserve(mvm, array, index);
        break;
      case MVM_OP_ARRAY_RESIZE:
        status = mvm_array_resize(mvm, array, index);
        break;
      case MVM_OP_ARRAY_REMOVE:
        status = mvm_array_remove(mvm, array, index, &result);
        returns_value = true;
        break;
      default: // ARRAY_GET
        status = mvm_array_get(mvm, array, index, &result);
        returns_value = true;
        break;
      }
    }
    if (status != MVM_RESULT_OK)
      return status;
    if (returns_value)
      stack->data[slot] = result;
    mvm_stack_truncate(stack, slot + (returns_value ? 1 : 0));
  } break;
  case MVM_OP_FIELD_GET:
  case MVM_OP_FIELD_SET:
  case MVM_OP_FIELD_ADDR: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    const size_t count = instruction->op == MVM_OP_FIELD_SET ? 2 : 1;
    if (stack->size - mvm_frame_base_for(mvm, execution) < count)
      return MVM_RESULT_RUNTIME_ERROR;
    const size_t slot = stack->size - count;
    const mvm_value_t target = stack->data[slot];
    const size_t index = instruction->as.field_index;
    const bool typed = mvm_typed_object(mvm, target) != NULL ||
                       (target.kind == MVM_KIND_REFERENCE && target.has_full_type);
    if (instruction->op == MVM_OP_FIELD_SET) {
      const mvm_value_t field = stack->data[slot + 1];
      if (field.kind == MVM_KIND_PACKET)
        return MVM_RESULT_RUNTIME_ERROR;
      const mvm_result_t status =
          typed ? mvm_typed_field_set(mvm, target, index, field)
                : mvm_record_set(mvm, target, index, field);
      if (status != MVM_RESULT_OK)
        return status;
      mvm_stack_truncate(stack, slot);
    } else {
      mvm_value_t result;
      const mvm_result_t status =
          instruction->op == MVM_OP_FIELD_ADDR
              ? mvm_typed_field_address(mvm, target, index, &result)
          : typed ? mvm_typed_field_get(mvm, target, index, &result)
                  : mvm_record_get(mvm, target, index, &result);
      if (status != MVM_RESULT_OK)
        return status;
      stack->data[slot] = result;
    }
  } break;
  case MVM_OP_TYPED_ALLOC: {
    uint32_t reference_type_id;
    mvm_result_t status = mvm_reference_type(mvm, instruction->as.type_id, true,
                                             &reference_type_id);
    if (status != MVM_RESULT_OK)
      return status;
    mvm_value_t result;
    status = mvm_typed_alloc(mvm, instruction->as.type_id, &result);
    if (status != MVM_RESULT_OK)
      return status;
    result.type_id = reference_type_id;
    result.has_full_type = true;
    if (!mvm_stack_push(stack, result))
      return MVM_RESULT_VM_ERROR;
  } break;
  case MVM_OP_VA_PACK: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    const size_t count = instruction->as.va_pack.count;
    if (stack->size - mvm_frame_base_for(mvm, execution) < count)
      return MVM_RESULT_RUNTIME_ERROR;
    const size_t slot = stack->size - count;
    mvm_value_t packet;
    const mvm_result_t status =
        mvm_packet_new(mvm, count, stack->data + slot,
                       instruction->as.va_pack.types_pc, &packet);
    if (status != MVM_RESULT_OK)
      return status;
    if (count) {
      stack->data[slot] = packet;
      mvm_stack_truncate(stack, slot + 1);
    } else if (!mvm_stack_push(stack, packet)) {
      return MVM_RESULT_VM_ERROR;
    }
  } break;
  case MVM_OP_VA_FORWARD:
  case MVM_OP_VA_COUNT: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    mvm_packet_t *packet = mvm_current_packet(mvm, execution);
    if (!packet)
      return MVM_RESULT_RUNTIME_ERROR;
    const mvm_value_t result = instruction->op == MVM_OP_VA_FORWARD
                                   ? (mvm_value_t){.kind = MVM_KIND_PACKET,
                                                   .as.packet = packet}
                                   : (mvm_value_t){.kind = MVM_KIND_ULONG,
                                                   .as.ul = packet->count};
    if (!mvm_stack_push(stack, result))
      return MVM_RESULT_VM_ERROR;
  } break;
  case MVM_OP_VA_ARG: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    if (stack->size <= mvm_frame_base_for(mvm, execution))
      return MVM_RESULT_RUNTIME_ERROR;
    const mvm_value_t index_value = stack->data[stack->size - 1];
    uint64_t bits;
    switch (index_value.kind) {
    case MVM_KIND_UBYTE: bits = index_value.as.ub; break;
    case MVM_KIND_UHALF: bits = index_value.as.uh; break;
    case MVM_KIND_UWORD: bits = index_value.as.uw; break;
    case MVM_KIND_ULONG: bits = index_value.as.ul; break;
    default: return MVM_RESULT_RUNTIME_ERROR;
    }
    if (bits > SIZE_MAX)
      return MVM_RESULT_RUNTIME_ERROR;
    const mvm_packet_t *packet = mvm_current_packet(mvm, execution);
    if (!packet)
      return MVM_RESULT_RUNTIME_ERROR;
    mvm_value_t result;
    const mvm_result_t status = mvm_va_arg(mvm, packet, (size_t)bits,
                                           instruction->as.type_id, &result);
    if (status != MVM_RESULT_OK)
      return status;
    stack->data[stack->size - 1] = result;
  } break;
  case MVM_OP_REF_READ:
  case MVM_OP_REF_WRITE:
  case MVM_OP_TYPED_INIT: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    const size_t count = instruction->op == MVM_OP_REF_WRITE ? 2 : 1;
    if (stack->size - mvm_frame_base_for(mvm, execution) < count)
      return MVM_RESULT_RUNTIME_ERROR;
    const size_t slot = stack->size - count;
    mvm_value_t result;
    mvm_result_t status;
    if (instruction->op == MVM_OP_REF_READ) {
      status = mvm_ref_read(mvm, instruction->as.type_id,
                            stack->data[slot], &result);
    } else if (instruction->op == MVM_OP_REF_WRITE) {
      status = mvm_ref_write(mvm, instruction->as.type_id,
                             stack->data[slot], stack->data[slot + 1]);
    } else {
      status = mvm_typed_init(mvm, instruction->as.type_id,
                              stack->data[slot], &result);
    }
    if (status != MVM_RESULT_OK)
      return status;
    if (instruction->op == MVM_OP_REF_WRITE) {
      mvm_stack_truncate(stack, slot);
    } else {
      stack->data[slot] = result;
    }
  } break;
  case MVM_OP_UNION_NEW:
  case MVM_OP_UNION_GET:
  case MVM_OP_UNION_WIDEN:
  case MVM_OP_UNION_VARIANTOF:
  case MVM_OP_ARRAY_NEW_TYPED: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    if (stack->size <= mvm_frame_base_for(mvm, execution))
      return MVM_RESULT_RUNTIME_ERROR;
    const size_t slot = stack->size - 1;
    const mvm_value_t input = stack->data[slot];
    mvm_value_t result;
    mvm_result_t status;
    if (instruction->op == MVM_OP_UNION_NEW)
      status = mvm_union_new(mvm, instruction->as.union_new.union_type_id,
                              instruction->as.union_new.variant_type_id, input, &result);
    else if (instruction->op == MVM_OP_UNION_GET)
      status = mvm_union_get(mvm, input, instruction->as.type_id, &result);
    else if (instruction->op == MVM_OP_UNION_WIDEN)
      status = mvm_union_widen(mvm, input, instruction->as.type_id, &result);
    else if (instruction->op == MVM_OP_UNION_VARIANTOF)
      status = mvm_union_variantof(mvm, input, &result);
    else {
      size_t length;
      status = mvm_value_index(input, &length);
      if (status == MVM_RESULT_OK)
        status = mvm_array_new_typed(mvm, instruction->as.type_id, length, &result);
    }
    if (status != MVM_RESULT_OK)
      return status;
    stack->data[slot] = result;
  } break;
  case MVM_OP_TYPED_CONSTRUCT: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    mvm_value_t result;
    const mvm_result_t status = mvm_typed_construct(mvm, instruction->as.type_id, &result);
    if (status != MVM_RESULT_OK)
      return status;
    if (!mvm_stack_push(stack, result))
      return MVM_RESULT_VM_ERROR;
  } break;
  case MVM_OP_LIT_TYPE_ID: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    const mvm_value_t value = {
        .kind = MVM_KIND_TYPE,
        .as.type = instruction->as.type_id,
        .type_id = instruction->as.type_id,
        .has_full_type = true,
    };
    if (!mvm_stack_push(stack, value))
      return MVM_RESULT_VM_ERROR;
  } break;
  case MVM_OP_VALUE_COPY:
  case MVM_OP_VALUE_ZERO: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    const size_t count = instruction->op == MVM_OP_VALUE_COPY ? 2 : 1;
    if (stack->size - mvm_frame_base_for(mvm, execution) < count)
      return MVM_RESULT_RUNTIME_ERROR;
    const size_t slot = stack->size - count;
    const mvm_result_t status =
        instruction->op == MVM_OP_VALUE_COPY
            ? mvm_typed_copy(mvm, instruction->as.type_id,
                             stack->data[slot], stack->data[slot + 1])
            : mvm_typed_zero(mvm, instruction->as.type_id,
                             stack->data[slot]);
    if (status != MVM_RESULT_OK)
      return status;
    mvm_stack_truncate(stack, slot);
  } break;
  case MVM_OP_LOAD:
  case MVM_OP_STORE: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;

    const bool store = instruction->op == MVM_OP_STORE;
    const size_t count = store ? 2 : 1;
    if (stack->size - mvm_frame_base_for(mvm, execution) < count)
      return MVM_RESULT_RUNTIME_ERROR;
    const size_t slot = stack->size - count;
    size_t address;
    const mvm_result_t status =
        mvm_value_index(stack->data[slot], &address);
    if (status != MVM_RESULT_OK)
      return status;
    const size_t width = mvm_ram_kind_width(instruction->as.kind);
    if (address > mvm->ram->size || width > mvm->ram->size - address)
      return MVM_RESULT_RUNTIME_ERROR;

    if (store) {
      const mvm_value_t value = stack->data[slot + 1];
      if (value.kind != instruction->as.kind ||
          mvm_code_write_blocked(mvm, address, width))
        return MVM_RESULT_RUNTIME_ERROR;
      if (!mvm_store_ram(mvm->ram, address, value))
        return MVM_RESULT_RUNTIME_ERROR;
      mvm_stack_truncate(stack, slot);
    } else {
      mvm_value_t value;
      const mvm_result_t result =
          mvm_load_ram(mvm->ram, address, instruction->as.kind, &value);
      if (result != MVM_RESULT_OK)
        return result;
      stack->data[slot] = value;
    }
  } break;
  case MVM_OP_LIT_TYPE: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    const mvm_value_t value = {
        .kind = MVM_KIND_TYPE,
        .as.type = instruction->as.kind,
    };
    if (!mvm_stack_push(stack, value))
      return MVM_RESULT_VM_ERROR;
  } break;
  case MVM_OP_POP: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    if (stack->size <= mvm_frame_base_for(mvm, execution))
      return MVM_RESULT_RUNTIME_ERROR;
    --stack->size;
  } break;
  case MVM_OP_DUP: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    if (stack->size <= mvm_frame_base_for(mvm, execution))
      return MVM_RESULT_RUNTIME_ERROR;
    if (!mvm_stack_push(stack, stack->data[stack->size - 1]))
      return MVM_RESULT_VM_ERROR;
  } break;
  case MVM_OP_SWAP: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    if (stack->size - mvm_frame_base_for(mvm, execution) < 2)
      return MVM_RESULT_RUNTIME_ERROR;
    mvm_value_t *data = stack->data;
    const size_t top = stack->size - 1;
    const mvm_value_t value = data[top];
    data[top] = data[top - 1];
    data[top - 1] = value;
  } break;
  case MVM_OP_ADD:
  case MVM_OP_SUB:
  case MVM_OP_MUL:
  case MVM_OP_DIV:
  case MVM_OP_MOD: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    if (stack->size - mvm_frame_base_for(mvm, execution) < 2)
      return MVM_RESULT_RUNTIME_ERROR;

    mvm_value_t result;
    const size_t top = stack->size - 1;
    const mvm_value_t lhs = stack->data[top - 1];
    const mvm_value_t rhs = stack->data[top];
    mvm_result_t status = MVM_RESULT_OK;
    if (lhs.kind == rhs.kind &&
        (lhs.kind == MVM_KIND_ULONG || lhs.kind == MVM_KIND_SLONG) &&
        (instruction->op == MVM_OP_ADD || instruction->op == MVM_OP_SUB ||
         instruction->op == MVM_OP_MUL)) {
      result = (mvm_value_t){.kind = lhs.kind};
      if (lhs.kind == MVM_KIND_ULONG) {
        if (instruction->op == MVM_OP_ADD)
          result.as.ul = lhs.as.ul + rhs.as.ul;
        else if (instruction->op == MVM_OP_SUB)
          result.as.ul = lhs.as.ul - rhs.as.ul;
        else
          result.as.ul = lhs.as.ul * rhs.as.ul;
      } else {
        const uint64_t a = (uint64_t)lhs.as.sl;
        const uint64_t b = (uint64_t)rhs.as.sl;
        uint64_t bits;
        if (instruction->op == MVM_OP_ADD)
          bits = a + b;
        else if (instruction->op == MVM_OP_SUB)
          bits = a - b;
        else
          bits = a * b;
        memcpy(&result.as.sl, &bits, sizeof(bits));
      }
    } else {
      status = mvm_binary_arithmetic(instruction->op, lhs, rhs, &result);
    }
    if (status != MVM_RESULT_OK)
      return status;
    stack->data[top - 1] = result;
    --stack->size;
  } break;
  case MVM_OP_NEG:
  case MVM_OP_INC:
  case MVM_OP_DEC: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    if (stack->size <= mvm_frame_base_for(mvm, execution))
      return MVM_RESULT_RUNTIME_ERROR;

    mvm_value_t result;
    const size_t top = stack->size - 1;
    const mvm_result_t status =
        mvm_unary_fast(instruction->op, stack->data[top], &result);
    if (status != MVM_RESULT_OK)
      return status;
    stack->data[top] = result;
  } break;
  case MVM_OP_INC_LOCAL:
  case MVM_OP_DEC_LOCAL: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    const size_t base = mvm_frame_base_for(mvm, execution);
    const size_t index = instruction->as.local_index;
    if (index >= stack->size - base)
      return MVM_RESULT_RUNTIME_ERROR;
    const size_t slot = base + index;
    if (*mvm_execution_frame_count_for(mvm, execution) &&
        mvm_execution_frames_for(mvm, execution)[
            *mvm_execution_frame_count_for(mvm, execution) - 1].packet_index == slot)
      return MVM_RESULT_RUNTIME_ERROR;
    mvm_value_t result;
    const mvm_result_t status = mvm_unary_fast(
        instruction->op == MVM_OP_INC_LOCAL ? MVM_OP_INC : MVM_OP_DEC,
        stack->data[slot], &result);
    if (status != MVM_RESULT_OK)
      return status;
    stack->data[slot] = result;
  } break;
  case MVM_OP_BAND:
  case MVM_OP_BOR:
  case MVM_OP_BXOR:
  case MVM_OP_SHL:
  case MVM_OP_SHR: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    if (stack->size - mvm_frame_base_for(mvm, execution) < 2)
      return MVM_RESULT_RUNTIME_ERROR;

    mvm_value_t result;
    const size_t top = stack->size - 1;
    const mvm_result_t status =
        mvm_binary_bitwise(instruction->op, stack->data[top - 1],
                           stack->data[top], &result);
    if (status != MVM_RESULT_OK)
      return status;
    stack->data[top - 1] = result;
    --stack->size;
  } break;
  case MVM_OP_BNOT: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    if (stack->size <= mvm_frame_base_for(mvm, execution))
      return MVM_RESULT_RUNTIME_ERROR;

    mvm_value_t result;
    const size_t top = stack->size - 1;
    const mvm_result_t status = mvm_bitwise_not(stack->data[top], &result);
    if (status != MVM_RESULT_OK)
      return status;
    stack->data[top] = result;
  } break;
  case MVM_OP_EQ:
  case MVM_OP_NE:
  case MVM_OP_LT:
  case MVM_OP_LE:
  case MVM_OP_GT:
  case MVM_OP_GE:
  case MVM_OP_CONCAT:
  case MVM_OP_STRING_GET: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    if (stack->size - mvm_frame_base_for(mvm, execution) < 2)
      return MVM_RESULT_RUNTIME_ERROR;

    const size_t top = stack->size - 1;
    const mvm_value_t lhs = stack->data[top - 1];
    const mvm_value_t rhs = stack->data[top];
    mvm_value_t result;
    mvm_result_t status = MVM_RESULT_OK;
    if (instruction->op >= MVM_OP_EQ && instruction->op <= MVM_OP_GE) {
      bool comparison = false;
      status = mvm_compare_fast(mvm, instruction->op, lhs, rhs, &comparison);
      result = (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = comparison};
    } else if (instruction->op == MVM_OP_CONCAT) {
      status = mvm_concat_strings(mvm, stack->data[top - 1],
                                  stack->data[top], &result);
    } else {
      status = mvm_string_get(mvm, stack->data[top - 1],
                              stack->data[top], &result);
    }
    if (status != MVM_RESULT_OK)
      return status;
    stack->data[top - 1] = result;
    --stack->size;
  } break;
  case MVM_OP_JUMP_IF_LOCAL: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    const size_t base = mvm_frame_base_for(mvm, execution);
    const uint16_t left = instruction->as.local_jump.left;
    const uint16_t right = instruction->as.local_jump.right;
    if (left >= stack->size - base || right >= stack->size - base)
      return MVM_RESULT_RUNTIME_ERROR;
    if (*mvm_execution_frame_count_for(mvm, execution)) {
      const size_t packet = mvm_execution_frames_for(mvm, execution)[
          *mvm_execution_frame_count_for(mvm, execution) - 1].packet_index;
      if (base + left == packet || base + right == packet)
        return MVM_RESULT_RUNTIME_ERROR;
    }
    bool comparison;
    const mvm_result_t status = mvm_compare_fast(
        mvm, instruction->as.local_jump.comparison, stack->data[base + left],
        stack->data[base + right], &comparison);
    if (status != MVM_RESULT_OK)
      return status;
    if (comparison == instruction->as.local_jump.when_true) {
      const size_t address = instruction->as.local_jump.address;
      if (address >= mvm->ram->size)
        return MVM_RESULT_RUNTIME_ERROR;
      *mvm_execution_pc_for(mvm, execution) = address;
      return MVM_RESULT_OK;
    }
  } break;
  case MVM_OP_NOT: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    if (stack->size <= mvm_frame_base_for(mvm, execution))
      return MVM_RESULT_RUNTIME_ERROR;

    const size_t top = stack->size - 1;
    if (stack->data[top].kind != MVM_KIND_BOOL)
      return MVM_RESULT_RUNTIME_ERROR;
    stack->data[top].as.b = !stack->data[top].as.b;
  } break;
  case MVM_OP_STRING_LEN: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    if (stack->size <= mvm_frame_base_for(mvm, execution))
      return MVM_RESULT_RUNTIME_ERROR;

    const size_t top = stack->size - 1;
    size_t byte_length;
    size_t character_length;
    const mvm_result_t status = mvm_measure_string(
        mvm, stack->data[top], &byte_length, &character_length);
    if (status != MVM_RESULT_OK)
      return status;
    if (character_length > UINT32_MAX)
      return MVM_RESULT_RUNTIME_ERROR;
    stack->data[top] =
        (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = character_length};
  } break;
  case MVM_OP_CAST:
  case MVM_OP_TYPEOF:
  case MVM_OP_IS: {
    if (!stack)
      return MVM_RESULT_VM_ERROR;
    if (stack->size <= mvm_frame_base_for(mvm, execution))
      return MVM_RESULT_RUNTIME_ERROR;

    const size_t top = stack->size - 1;
    const mvm_value_t value = stack->data[top];
    mvm_value_t result;
    if (instruction->op == MVM_OP_CAST) {
      const mvm_result_t status =
          mvm_cast_value_fast(value, instruction->as.kind, &result);
      if (status != MVM_RESULT_OK)
        return status;
    } else if (instruction->op == MVM_OP_TYPEOF) {
      result = (mvm_value_t){.kind = MVM_KIND_TYPE, .as.type = value.kind};
    } else {
      result = (mvm_value_t){.kind = MVM_KIND_BOOL,
                             .as.b = value.kind == instruction->as.kind};
    }
    stack->data[top] = result;
  } break;
  case MVM_OP_COUNT:
  default:
    return MVM_RESULT_RUNTIME_ERROR;
  }

  *mvm_execution_pc_for(mvm, execution) = instruction->next_pc;
  return MVM_RESULT_OK;
}
