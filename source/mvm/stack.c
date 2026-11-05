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

#include "mvm/stack.h"

#include <stdlib.h>

mvm_stack_t *mvm_stack_create(const size_t capacity) {
  return mvm_stack_create_managed(NULL, capacity);
}

mvm_stack_t *mvm_stack_create_managed(mvm_memory_t *memory, size_t capacity) {
  if (capacity > SIZE_MAX / sizeof(mvm_value_t))
    return NULL;

  mvm_stack_t *stack = mvm_memory_calloc(memory, 1, sizeof(mvm_stack_t));
  if (!stack)
    return NULL;

  if (capacity > 0) {
    stack->data = mvm_memory_alloc(memory, capacity * sizeof(mvm_value_t));
    if (!stack->data) {
      mvm_memory_free(stack);
      return NULL;
    }
  }
  stack->capacity = capacity;
  stack->memory = memory;

  return stack;
}

void mvm_stack_destroy(mvm_stack_t *stack) {
  if (!stack)
    return;
  mvm_memory_free(stack->data);
  mvm_memory_free(stack);
}

bool mvm_stack_reserve(mvm_stack_t *stack, const size_t capacity) {
  if (!stack || capacity > SIZE_MAX / sizeof(mvm_value_t))
    return false;
  if (capacity <= stack->capacity)
    return true;
  mvm_value_t *data = mvm_memory_realloc(stack->memory, stack->data,
                                         capacity * sizeof(mvm_value_t));
  if (!data)
    return false;
  stack->data = data;
  stack->capacity = capacity;
  return true;
}

bool mvm_stack_push(mvm_stack_t *stack, mvm_value_t value) {
  if (!stack)
    return false;
  if (stack->size == stack->capacity) {
    const size_t max_capacity = SIZE_MAX / sizeof(mvm_value_t);
    if (stack->capacity == max_capacity)
      return false;

    size_t capacity = stack->capacity;
    if (capacity == 0)
      capacity = 8;
    else if (capacity > max_capacity / 2)
      capacity = max_capacity;
    else
      capacity *= 2;
    if (!mvm_stack_reserve(stack, capacity))
      return false;
  }

  stack->data[stack->size++] = value;
  return true;
}

bool mvm_stack_pop(mvm_stack_t *stack, mvm_value_t *value) {
  if (!stack || !value || stack->size == 0)
    return false;
  *value = stack->data[--stack->size];
  return true;
}

bool mvm_stack_peek(const mvm_stack_t *stack, mvm_value_t *value) {
  if (!stack || !value || stack->size == 0)
    return false;
  *value = stack->data[stack->size - 1];
  return true;
}

bool mvm_stack_truncate(mvm_stack_t *stack, const size_t size) {
  if (!stack || size > stack->size)
    return false;
  stack->size = size;
  return true;
}

void mvm_stack_clear(mvm_stack_t *stack) {
  if (stack)
    stack->size = 0;
}
