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

#include <stdbool.h>
#include <stddef.h>

typedef struct mvm_memory mvm_memory_t;

typedef struct mvm_memory_stats {
  size_t minimum;
  size_t maximum;
  size_t committed;
  size_t used;
} mvm_memory_stats_t;

// A nonmoving, synchronized pool. Limits include pool metadata and unused
// reserved storage. They do not describe process RSS or external libraries.
mvm_memory_t *mvm_memory_create(size_t minimum, size_t maximum);
void mvm_memory_destroy(mvm_memory_t *memory);
bool mvm_memory_stats(mvm_memory_t *memory, mvm_memory_stats_t *stats);

// A null pool selects independent system storage, for standalone RAM/stacks.
// All returned storage is aligned for max_align_t. A failed realloc preserves
// the original allocation. Only these allocations may be passed to free.
void *mvm_memory_alloc(mvm_memory_t *memory, size_t size);
void *mvm_memory_calloc(mvm_memory_t *memory, size_t count, size_t size);
void *mvm_memory_realloc(mvm_memory_t *memory, void *pointer, size_t size);
void mvm_memory_free(void *pointer);

// Thread-local sequence, incremented on allocation failure. Callers can
// distinguish a resource error from a validation error without global races.
size_t mvm_memory_failures(void);
