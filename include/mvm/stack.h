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
#include "value.h"
#include "mvm/memory.h"

#include <stdbool.h>
#include <stddef.h>

#define MVM_STACK_MIN_CAPACITY 0x1000

typedef struct mvm_stack {
  mvm_memory_t *memory;
  mvm_value_t *data;
  size_t size;
  size_t capacity;
} mvm_stack_t;

// Capacity is measured in values. Zero creates an empty, unallocated stack.
mvm_stack_t *mvm_stack_create(const size_t capacity);
mvm_stack_t *mvm_stack_create_managed(mvm_memory_t *memory, size_t capacity);
void mvm_stack_destroy(mvm_stack_t *stack);

// Reserve never shrinks the stack or changes its size.
bool mvm_stack_reserve(mvm_stack_t *stack, size_t capacity);
// Push grows the stack as needed. Failure leaves it unchanged.
bool mvm_stack_push(mvm_stack_t *stack, mvm_value_t value);
// Pop and peek require a non-null output. Failure leaves it unchanged.
bool mvm_stack_pop(mvm_stack_t *stack, mvm_value_t *value);
bool mvm_stack_peek(const mvm_stack_t *stack, mvm_value_t *value);
// Truncate drops values above size. Fails if size exceeds the current size.
bool mvm_stack_truncate(mvm_stack_t *stack, const size_t size);
// Clear retains allocated storage. Values are copied and not owned by the
// stack.
void mvm_stack_clear(mvm_stack_t *stack);
