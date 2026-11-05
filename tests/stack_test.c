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

// Keep checks active in release builds as well.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

static void test_lifo_and_growth(size_t capacity) {
  mvm_stack_t *stack = mvm_stack_create(capacity);
  assert(stack);
  assert(stack->size == 0);
  assert(stack->capacity == capacity);

  for (size_t i = 0; i < 1000; ++i) {
    mvm_value_t value = {.kind = (mvm_kind_t)(i % MVM_KIND_COUNT)};
    assert(mvm_stack_push(stack, value));
    assert(stack->size == i + 1);
    assert(stack->capacity >= stack->size);
  }

  for (size_t i = 1000; i > 0; --i) {
    mvm_value_t value = {.kind = MVM_KIND_VOID};
    assert(mvm_stack_peek(stack, &value));
    assert(value.kind == (mvm_kind_t)((i - 1) % MVM_KIND_COUNT));
    assert(stack->size == i);
    assert(mvm_stack_pop(stack, &value));
    assert(value.kind == (mvm_kind_t)((i - 1) % MVM_KIND_COUNT));
    assert(stack->size == i - 1);
  }

  mvm_value_t value = {.kind = MVM_KIND_NULL};
  assert(!mvm_stack_pop(stack, &value));
  assert(value.kind == MVM_KIND_NULL);
  assert(!mvm_stack_peek(stack, &value));
  assert(value.kind == MVM_KIND_NULL);
  assert(stack->size == 0);
  mvm_stack_destroy(stack);
}

static void test_clear_and_reuse(void) {
  mvm_stack_t *stack = mvm_stack_create(2);
  mvm_value_t value = {.kind = MVM_KIND_BOOL};
  assert(stack);
  assert(mvm_stack_push(stack, value));
  assert(mvm_stack_push(stack, value));

  mvm_value_t *data = stack->data;
  size_t capacity = stack->capacity;
  mvm_stack_clear(stack);
  assert(stack->size == 0);
  assert(stack->capacity == capacity);
  assert(stack->data == data);
  assert(!mvm_stack_peek(stack, &value));

  value.kind = MVM_KIND_DOUBLE;
  assert(mvm_stack_push(stack, value));
  assert(mvm_stack_pop(stack, &value));
  assert(value.kind == MVM_KIND_DOUBLE);
  assert(!mvm_stack_pop(stack, &value));
  mvm_stack_destroy(stack);
}

static void test_truncate(void) {
  mvm_stack_t *stack = mvm_stack_create(0);
  assert(stack);
  for (uint8_t i = 0; i < 4; ++i) {
    mvm_value_t value = {.kind = MVM_KIND_UBYTE, .as.ub = i};
    assert(mvm_stack_push(stack, value));
  }

  assert(!mvm_stack_truncate(stack, 5));
  assert(stack->size == 4);
  assert(mvm_stack_truncate(stack, 4));
  assert(stack->size == 4);
  assert(mvm_stack_truncate(stack, 2));
  assert(stack->size == 2);

  mvm_value_t value = {.kind = MVM_KIND_VOID};
  assert(mvm_stack_peek(stack, &value));
  assert(value.kind == MVM_KIND_UBYTE && value.as.ub == 1);
  assert(mvm_stack_truncate(stack, 0));
  assert(!mvm_stack_peek(stack, &value));
  assert(!mvm_stack_truncate(NULL, 0));
  mvm_stack_destroy(stack);
}

static void test_reserve(void) {
  mvm_stack_t *stack = mvm_stack_create(1);
  assert(stack);
  assert(mvm_stack_reserve(stack, 1000));
  assert(stack->capacity == 1000 && stack->size == 0);
  mvm_value_t *data = stack->data;
  assert(mvm_stack_reserve(stack, 1000));
  assert(stack->capacity == 1000 && stack->data == data);
  assert(mvm_stack_reserve(stack, 2));
  assert(stack->capacity == 1000 && stack->data == data);
  for (size_t i = 0; i < 1000; ++i)
    assert(mvm_stack_push(stack, (mvm_value_t){.kind = MVM_KIND_ULONG, .as.ul = i}));
  assert(stack->capacity == 1000 && stack->data == data);
  assert(!mvm_stack_reserve(NULL, 1));
  if (sizeof(mvm_value_t) > 1)
    assert(!mvm_stack_reserve(stack, SIZE_MAX / sizeof(mvm_value_t) + 1));
  mvm_stack_destroy(stack);
}

static void test_invalid_arguments(void) {
  mvm_value_t value = {.kind = MVM_KIND_BOOL};
  assert(!mvm_stack_push(NULL, value));
  assert(!mvm_stack_pop(NULL, &value));
  assert(!mvm_stack_peek(NULL, &value));
  assert(value.kind == MVM_KIND_BOOL);
  mvm_stack_clear(NULL);
  mvm_stack_destroy(NULL);

  mvm_stack_t *stack = mvm_stack_create(1);
  assert(stack);
  assert(mvm_stack_push(stack, value));
  assert(!mvm_stack_pop(stack, NULL));
  assert(!mvm_stack_peek(stack, NULL));
  assert(stack->size == 1);
  assert(mvm_stack_pop(stack, &value));
  assert(value.kind == MVM_KIND_BOOL);
  mvm_stack_destroy(stack);

  if (sizeof(mvm_value_t) > 1)
    assert(!mvm_stack_create(SIZE_MAX / sizeof(mvm_value_t) + 1));
}

int main(void) {
  test_lifo_and_growth(0);
  test_lifo_and_growth(1);
  test_lifo_and_growth(3);
  test_clear_and_reuse();
  test_truncate();
  test_reserve();
  test_invalid_arguments();
  return 0;
}
