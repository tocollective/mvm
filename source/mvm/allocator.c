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

#include "mvm/memory.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
typedef CRITICAL_SECTION memory_mutex_t;
static bool memory_mutex_init(memory_mutex_t *mutex) {
  InitializeCriticalSection(mutex);
  return true;
}
static void memory_lock(memory_mutex_t *mutex) { EnterCriticalSection(mutex); }
static void memory_unlock(memory_mutex_t *mutex) {
  LeaveCriticalSection(mutex);
}
static void memory_mutex_destroy(memory_mutex_t *mutex) {
  DeleteCriticalSection(mutex);
}
#if defined(_MSC_VER)
#define MEMORY_THREAD_LOCAL __declspec(thread)
#else
#define MEMORY_THREAD_LOCAL _Thread_local
#endif
#else
#include <pthread.h>
typedef pthread_mutex_t memory_mutex_t;
static bool memory_mutex_init(memory_mutex_t *mutex) {
  return pthread_mutex_init(mutex, NULL) == 0;
}
static void memory_lock(memory_mutex_t *mutex) { pthread_mutex_lock(mutex); }
static void memory_unlock(memory_mutex_t *mutex) {
  pthread_mutex_unlock(mutex);
}
static void memory_mutex_destroy(memory_mutex_t *mutex) {
  pthread_mutex_destroy(mutex);
}
#define MEMORY_THREAD_LOCAL _Thread_local
#endif

typedef struct memory_chunk memory_chunk_t;
typedef union memory_block memory_block_t;

union memory_block {
  struct {
    memory_chunk_t *chunk;
    memory_block_t *previous;
    memory_block_t *next;
    memory_block_t *free_previous;
    memory_block_t *free_next;
    size_t size;
    bool available;
  } info;
  max_align_t alignment;
};

struct memory_chunk {
  memory_chunk_t *next;
  mvm_memory_t *owner;
  size_t size;
  max_align_t alignment;
  unsigned char data[];
};

struct mvm_memory {
  memory_mutex_t mutex;
  memory_chunk_t *chunks;
  memory_chunk_t *reserve;
  memory_block_t *free_blocks;
  size_t minimum;
  size_t maximum;
  size_t committed;
  size_t used;
  size_t next_chunk;
};

static MEMORY_THREAD_LOCAL size_t memory_failure_count;

size_t mvm_memory_failures(void) { return memory_failure_count; }

static void *memory_failure(void) {
  ++memory_failure_count;
  return NULL;
}

static bool memory_align(size_t size, size_t *aligned) {
  const size_t alignment = _Alignof(max_align_t);
  if (!size)
    size = 1;
  if (size > SIZE_MAX - (alignment - 1))
    return false;
  *aligned = (size + alignment - 1) / alignment * alignment;
  return true;
}

static void memory_remove_free(mvm_memory_t *memory, memory_block_t *block) {
  if (block->info.free_previous)
    block->info.free_previous->info.free_next = block->info.free_next;
  else
    memory->free_blocks = block->info.free_next;
  if (block->info.free_next)
    block->info.free_next->info.free_previous = block->info.free_previous;
}

static void memory_add_free(mvm_memory_t *memory, memory_block_t *block) {
  block->info.available = true;
  block->info.free_previous = NULL;
  block->info.free_next = memory->free_blocks;
  if (memory->free_blocks)
    memory->free_blocks->info.free_previous = block;
  memory->free_blocks = block;
}

static memory_block_t *memory_chunk_new(mvm_memory_t *memory, size_t size) {
  if (size < sizeof(memory_chunk_t) + sizeof(memory_block_t) +
                 _Alignof(max_align_t) ||
      size > memory->maximum - memory->committed)
    return NULL;
  memory_chunk_t *chunk = malloc(size);
  if (!chunk)
    return NULL;
  chunk->owner = memory;
  chunk->size = size;
  chunk->next = memory->chunks;
  memory->chunks = chunk;
  memory->committed += size;
  memory->used += sizeof(*chunk);
  memory_block_t *block = (memory_block_t *)chunk->data;
  *block = (memory_block_t){0};
  block->info.chunk = chunk;
  // Trim the tail so every split block remains correctly aligned.
  const size_t available = size - sizeof(*chunk) - sizeof(*block);
  block->info.size = available / _Alignof(max_align_t) * _Alignof(max_align_t);
  memory_add_free(memory, block);
  return block;
}

// Split only when the remainder can hold another aligned allocation.
static void memory_split(mvm_memory_t *memory, memory_block_t *block,
                         size_t size) {
  if (block->info.size - size < sizeof(*block) + _Alignof(max_align_t))
    return;
  memory_block_t *tail =
      (memory_block_t *)((unsigned char *)(block + 1) + size);
  *tail = (memory_block_t){0};
  tail->info.chunk = block->info.chunk;
  tail->info.size = block->info.size - size - sizeof(*block);
  tail->info.previous = block;
  tail->info.next = block->info.next;
  if (tail->info.next)
    tail->info.next->info.previous = tail;
  block->info.next = tail;
  block->info.size = size;
  memory_add_free(memory, tail);
}

static void *memory_allocate_unlocked(mvm_memory_t *memory, size_t size) {
  memory_block_t *block = memory->free_blocks;
  while (block && block->info.size < size)
    block = block->info.free_next;
  if (!block) {
    const size_t overhead = sizeof(memory_chunk_t) + sizeof(memory_block_t);
    if (size > SIZE_MAX - overhead)
      return NULL;
    const size_t required = overhead + size;
    const size_t remaining = memory->maximum - memory->committed;
    if (required > remaining)
      return NULL;
    size_t capacity = memory->next_chunk;
    if (capacity < required)
      capacity = required;
    if (capacity > remaining)
      capacity = remaining;
    block = memory_chunk_new(memory, capacity);
    if (!block && capacity != required)
      block = memory_chunk_new(memory, required);
    if (!block)
      return NULL;
    memory->next_chunk = capacity > SIZE_MAX / 2 ? SIZE_MAX : capacity * 2;
  }
  memory_remove_free(memory, block);
  memory_split(memory, block, size);
  block->info.available = false;
  memory->used += sizeof(*block) + block->info.size;
  return block + 1;
}

mvm_memory_t *mvm_memory_create(size_t minimum, size_t maximum) {
  if (!maximum || minimum > maximum)
    return NULL;
  if (maximum < sizeof(mvm_memory_t))
    return memory_failure();
  mvm_memory_t *memory = calloc(1, sizeof(*memory));
  if (!memory)
    return memory_failure();
  if (!memory_mutex_init(&memory->mutex)) {
    free(memory);
    return NULL;
  }
  memory->minimum = minimum;
  memory->maximum = maximum;
  memory->committed = memory->used = sizeof(*memory);
  memory->next_chunk = 64 * 1024;
  if (minimum > memory->committed) {
    size_t reserve = minimum - memory->committed;
    const size_t smallest =
        sizeof(memory_chunk_t) + sizeof(memory_block_t) + _Alignof(max_align_t);
    if (reserve < smallest)
      reserve = smallest;
    if (!memory_chunk_new(memory, reserve)) {
      mvm_memory_destroy(memory);
      return memory_failure();
    }
    memory->reserve = memory->chunks;
    memory->next_chunk = reserve;
  }
  return memory;
}

void mvm_memory_destroy(mvm_memory_t *memory) {
  if (!memory)
    return;
  memory_chunk_t *chunk = memory->chunks;
  while (chunk) {
    memory_chunk_t *next = chunk->next;
    free(chunk);
    chunk = next;
  }
  memory_mutex_destroy(&memory->mutex);
  free(memory);
}

bool mvm_memory_stats(mvm_memory_t *memory, mvm_memory_stats_t *stats) {
  if (!memory || !stats)
    return false;
  memory_lock(&memory->mutex);
  *stats = (mvm_memory_stats_t){memory->minimum, memory->maximum,
                                memory->committed, memory->used};
  memory_unlock(&memory->mutex);
  return true;
}

void *mvm_memory_alloc(mvm_memory_t *memory, size_t size) {
  size_t aligned;
  if (!memory_align(size, &aligned))
    return memory_failure();
  if (!memory) {
    if (aligned > SIZE_MAX - sizeof(memory_block_t))
      return memory_failure();
    memory_block_t *block = malloc(sizeof(*block) + aligned);
    if (!block)
      return memory_failure();
    *block = (memory_block_t){0};
    block->info.size = aligned;
    return block + 1;
  }
  memory_lock(&memory->mutex);
  void *pointer = memory_allocate_unlocked(memory, aligned);
  memory_unlock(&memory->mutex);
  return pointer ? pointer : memory_failure();
}

void *mvm_memory_calloc(mvm_memory_t *memory, size_t count, size_t size) {
  if (size && count > SIZE_MAX / size)
    return memory_failure();
  void *pointer = mvm_memory_alloc(memory, count * size);
  if (pointer)
    memset(pointer, 0, count * size);
  return pointer;
}

static void memory_free_unlocked(mvm_memory_t *memory, memory_block_t *block) {
  memory->used -= sizeof(*block) + block->info.size;
  if (block->info.next && block->info.next->info.available) {
    memory_block_t *next = block->info.next;
    memory_remove_free(memory, next);
    block->info.size += sizeof(*next) + next->info.size;
    block->info.next = next->info.next;
    if (block->info.next)
      block->info.next->info.previous = block;
  }
  if (block->info.previous && block->info.previous->info.available) {
    memory_block_t *previous = block->info.previous;
    memory_remove_free(memory, previous);
    previous->info.size += sizeof(*block) + block->info.size;
    previous->info.next = block->info.next;
    if (previous->info.next)
      previous->info.next->info.previous = previous;
    block = previous;
  }
  memory_chunk_t *chunk = block->info.chunk;
  if (!block->info.previous && !block->info.next && chunk != memory->reserve &&
      memory->committed - chunk->size >= memory->minimum) {
    memory_chunk_t **link = &memory->chunks;
    while (*link != chunk)
      link = &(*link)->next;
    *link = chunk->next;
    memory->committed -= chunk->size;
    memory->used -= sizeof(*chunk);
    free(chunk);
  } else {
    memory_add_free(memory, block);
  }
}

void mvm_memory_free(void *pointer) {
  if (!pointer)
    return;
  memory_block_t *block = (memory_block_t *)pointer - 1;
  if (!block->info.chunk) {
    free(block);
    return;
  }
  mvm_memory_t *memory = block->info.chunk->owner;
  memory_lock(&memory->mutex);
  memory_free_unlocked(memory, block);
  memory_unlock(&memory->mutex);
}

void *mvm_memory_realloc(mvm_memory_t *memory, void *pointer, size_t size) {
  if (!pointer)
    return mvm_memory_alloc(memory, size);
  if (!size) {
    mvm_memory_free(pointer);
    return NULL;
  }
  size_t aligned;
  if (!memory_align(size, &aligned))
    return memory_failure();
  memory_block_t *block = (memory_block_t *)pointer - 1;
  if (!block->info.chunk) {
    if (aligned > SIZE_MAX - sizeof(*block))
      return memory_failure();
    block = realloc(block, sizeof(*block) + aligned);
    if (!block)
      return memory_failure();
    block->info.size = aligned;
    return block + 1;
  }
  memory = block->info.chunk->owner;
  memory_lock(&memory->mutex);
  const size_t old_size = block->info.size;
  if (aligned > old_size && block->info.next &&
      block->info.next->info.available &&
      aligned - old_size <= sizeof(*block) + block->info.next->info.size) {
    memory_block_t *next = block->info.next;
    memory_remove_free(memory, next);
    block->info.size += sizeof(*next) + next->info.size;
    block->info.next = next->info.next;
    if (block->info.next)
      block->info.next->info.previous = block;
  }
  if (aligned <= block->info.size) {
    // Merge a split remainder with its free successor to avoid fragmentation.
    memory_split(memory, block, aligned);
    memory_block_t *tail = block->info.next;
    if (tail && tail->info.available && tail->info.next &&
        tail->info.next->info.available) {
      memory_block_t *next = tail->info.next;
      memory_remove_free(memory, next);
      tail->info.size += sizeof(*next) + next->info.size;
      tail->info.next = next->info.next;
      if (tail->info.next)
        tail->info.next->info.previous = tail;
    }
    memory->used = memory->used - old_size + block->info.size;
    memory_unlock(&memory->mutex);
    return pointer;
  }
  void *replacement = memory_allocate_unlocked(memory, aligned);
  if (replacement) {
    memcpy(replacement, pointer, old_size);
    memory_free_unlocked(memory, block);
  }
  memory_unlock(&memory->mutex);
  return replacement ? replacement : memory_failure();
}
