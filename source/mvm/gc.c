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

#include <stdlib.h>

bool mvm_gc_root_register(mvm_t *mvm, mvm_gc_root_t *root,
                          const mvm_value_t *values, size_t count) {
  if (!mvm || !root || (count && !values))
    return false;
  mvm_runtime_lock(mvm);
  for (mvm_gc_root_t *current = mvm->gc_roots; current;
       current = current->next) {
    if (current == root) {
      mvm_runtime_unlock(mvm);
      return false;
    }
  }
  *root =
      (mvm_gc_root_t){.values = values, .count = count, .next = mvm->gc_roots};
  mvm->gc_roots = root;
  mvm_runtime_unlock(mvm);
  return true;
}

void mvm_gc_root_unregister(mvm_t *mvm, mvm_gc_root_t *root) {
  if (!mvm || !root)
    return;
  mvm_runtime_lock(mvm);
  for (mvm_gc_root_t **current = &mvm->gc_roots; *current;
       current = &(*current)->next) {
    if (*current == root) {
      *current = root->next;
      *root = (mvm_gc_root_t){0};
      break;
    }
  }
  mvm_runtime_unlock(mvm);
}

static mvm_object_t *mvm_gc_find_unlocked(const mvm_t *mvm,
                                          const void *pointer) {
  if (!pointer)
    return NULL;
  const uintptr_t address = (uintptr_t)pointer;
  for (mvm_object_t *object = mvm->objects; object; object = object->next) {
    const uintptr_t base = (uintptr_t)object->data;
    if (address == base || (address > base && address - base < object->size))
      return object;
  }
  return NULL;
}

// Interior pointers returned by native functions retain the containing object.
// Compare integer addresses so unrelated host/RAM pointers are safe to inspect.
mvm_object_t *mvm_gc_find(const mvm_t *mvm, const void *pointer) {
  mvm_runtime_lock((mvm_t *)mvm);
  mvm_object_t *object = mvm_gc_find_unlocked(mvm, pointer);
  mvm_runtime_unlock((mvm_t *)mvm);
  return object;
}

static void mvm_gc_mark_value(mvm_t *mvm, const mvm_value_t value,
                              mvm_object_t **gray) {
  const void *pointer;
  switch (value.kind) {
  case MVM_KIND_STRING:
    pointer = value.as.s;
    break;
  case MVM_KIND_REFERENCE:
    pointer = value.as.ptr;
    break;
  case MVM_KIND_ARRAY:
    pointer = value.as.array;
    break;
  case MVM_KIND_THREAD:
    pointer = value.as.thread;
    break;
  case MVM_KIND_PACKET:
    pointer = value.as.packet;
    break;
  case MVM_KIND_UNION:
    pointer = value.as.union_value;
    break;
  case MVM_KIND_MUTEX:
    pointer = value.as.mutex;
    break;
  case MVM_KIND_CONDITION:
    pointer = value.as.condition;
    break;
  case MVM_KIND_ATOMIC:
    pointer = value.as.atomic;
    break;
  default:
    return;
  }
  mvm_object_t *object = mvm_gc_find_unlocked(mvm, pointer);
  if (!object || object->marked)
    return;
  object->marked = true;
  object->gray_next = *gray;
  *gray = object;
}

static void mvm_gc_mark_stack(mvm_t *mvm, const mvm_stack_t *stack,
                              mvm_object_t **gray) {
  if (!stack)
    return;
  for (size_t i = 0; i < stack->size; ++i)
    mvm_gc_mark_value(mvm, stack->data[i], gray);
}

size_t mvm_gc_collect(mvm_t *mvm) {
  if (!mvm)
    return 0;
  if (!mvm_runtime_gc_begin(mvm))
    return 0;

  mvm_object_t *gray = NULL;
  mvm_gc_mark_stack(mvm, mvm->stack, &gray);
  for (mvm_execution_t *execution = mvm->executions; execution;
       execution = execution->next)
    mvm_gc_mark_stack(mvm, execution->stack, &gray);
  for (size_t i = 0; i < mvm->global_count; ++i)
    mvm_gc_mark_value(mvm, mvm->globals[i], &gray);
  for (mvm_gc_root_t *root = mvm->gc_roots; root; root = root->next) {
    for (size_t i = 0; i < root->count; ++i)
      mvm_gc_mark_value(mvm, root->values[i], &gray);
  }

  // Intrusive worklist needs neither recursion nor allocations during GC.
  while (gray) {
    mvm_object_t *object = gray;
    gray = object->gray_next;
    if (object->kind == MVM_OBJECT_UNION) {
      const mvm_union_t *value = (const mvm_union_t *)object->data;
      mvm_gc_mark_value(mvm, value->payload, &gray);
    } else if (object->kind == MVM_OBJECT_ARRAY) {
      const mvm_array_t *array = (const mvm_array_t *)object->data;
      for (size_t i = 0; i < array->length; ++i)
        mvm_gc_mark_value(mvm, array->elements[i], &gray);
    } else if (object->kind == MVM_OBJECT_RECORD) {
      const struct mvm_record *record = (const struct mvm_record *)object->data;
      for (size_t i = 0; i < record->length; ++i)
        mvm_gc_mark_value(mvm, record->fields[i], &gray);
    } else if (object->kind == MVM_OBJECT_PACKET) {
      const mvm_packet_t *packet = (const mvm_packet_t *)object->data;
      for (size_t i = 0; i < packet->count; ++i)
        mvm_gc_mark_value(mvm, packet->values[i], &gray);
    } else if (object->kind == MVM_OBJECT_TYPED &&
               (size_t)object->type_id < mvm->type_count) {
      const mvm_type_t *type = &mvm->types[object->type_id];
      for (size_t i = 0; i < type->reference_count; ++i) {
        const uint64_t bits =
            mvm_read_u64(object->data + type->references[i].offset);
        mvm_gc_mark_value(mvm,
                          (mvm_value_t){.kind = MVM_KIND_REFERENCE,
                                        .as.ptr = (void *)(uintptr_t)bits},
                          &gray);
      }
    }
  }

  size_t reclaimed = 0;
  mvm_object_t **current = &mvm->objects;
  while (*current) {
    mvm_object_t *object = *current;
    if (object->marked) {
      object->marked = false;
      current = &object->next;
      continue;
    }
    *current = object->next;
    mvm_gc_free_object(mvm, object);
    ++reclaimed;
  }

  ++mvm->gc_collections;
  if (mvm->gc_bytes > SIZE_MAX / 2) {
    mvm->gc_next_collection = SIZE_MAX;
  } else {
    mvm->gc_next_collection = mvm->gc_bytes * 2;
    if (mvm->gc_next_collection < MVM_GC_INITIAL_THRESHOLD)
      mvm->gc_next_collection = MVM_GC_INITIAL_THRESHOLD;
  }
  mvm_runtime_gc_end(mvm);
  return reclaimed;
}

void mvm_gc_free_object(mvm_t *mvm, mvm_object_t *object) {
  if (object->kind == MVM_OBJECT_ARRAY) {
    mvm_array_t *array = (mvm_array_t *)object->data;
    mvm->gc_bytes -= array->capacity * sizeof(*array->elements);
    mvm_memory_free(array->elements);
  } else if (object->kind == MVM_OBJECT_THREAD) {
    mvm_thread_release_handle((mvm_thread_t *)object->data);
  } else if (object->kind == MVM_OBJECT_MUTEX) {
    mvm_mutex_release_handle((mvm_mutex_t *)object->data);
  } else if (object->kind == MVM_OBJECT_CONDITION) {
    mvm_condition_release_handle((mvm_condition_t *)object->data);
  } else if (object->kind == MVM_OBJECT_ATOMIC) {
    mvm_atomic_release_handle((mvm_atomic_t *)object->data);
  }
  mvm->gc_bytes -= sizeof(*object) + object->size;
  mvm_memory_free(object);
}

static bool mvm_gc_needed(const mvm_t *mvm, size_t size) {
  return size > SIZE_MAX - mvm->gc_bytes || size > mvm->gc_next_collection ||
         mvm->gc_bytes > mvm->gc_next_collection - size;
}

bool mvm_gc_array_reserve(mvm_t *mvm, mvm_array_t *array, size_t capacity) {
  mvm_runtime_lock(mvm);
  if (capacity <= array->capacity) {
    mvm_runtime_unlock(mvm);
    return true;
  }
  if (capacity > UINT32_MAX || capacity > SIZE_MAX / sizeof(*array->elements)) {
    mvm_runtime_unlock(mvm);
    return false;
  }
  const size_t bytes = capacity * sizeof(*array->elements);
  const size_t growth = bytes - array->capacity * sizeof(*array->elements);
  if (mvm_gc_needed(mvm, growth)) {
    mvm_runtime_unlock(mvm);
    mvm_gc_collect(mvm);
    mvm_runtime_lock(mvm);
  }
  if (growth > SIZE_MAX - mvm->gc_bytes) {
    mvm_runtime_unlock(mvm);
    return false;
  }

  mvm_value_t *elements =
      mvm_memory_realloc(mvm->memory, array->elements, bytes);
  if (!elements) {
    mvm_runtime_unlock(mvm);
    mvm_gc_collect(mvm);
    mvm_runtime_lock(mvm);
    elements = mvm_memory_realloc(mvm->memory, array->elements, bytes);
    if (!elements) {
      mvm_runtime_unlock(mvm);
      return false;
    }
  }
  array->elements = elements;
  array->capacity = capacity;
  mvm->gc_bytes += growth;
  mvm_runtime_unlock(mvm);
  return true;
}

mvm_object_t *mvm_gc_allocate(mvm_t *mvm, mvm_object_kind_t kind, size_t size) {
  if (size > SIZE_MAX - sizeof(mvm_object_t))
    return NULL;
  const size_t total = sizeof(mvm_object_t) + size;
  mvm_runtime_lock(mvm);
  if (mvm_gc_needed(mvm, total)) {
    mvm_runtime_unlock(mvm);
    mvm_gc_collect(mvm);
    mvm_runtime_lock(mvm);
  }
  if (total > SIZE_MAX - mvm->gc_bytes) {
    mvm_runtime_unlock(mvm);
    return NULL;
  }

  mvm_object_t *object = mvm_memory_calloc(mvm->memory, 1, total);
  if (!object) {
    mvm_runtime_unlock(mvm);
    mvm_gc_collect(mvm);
    mvm_runtime_lock(mvm);
    object = mvm_memory_calloc(mvm->memory, 1, total);
    if (!object) {
      mvm_runtime_unlock(mvm);
      return NULL;
    }
  }
  object->kind = kind;
  object->type_id = MVM_TYPE_ID_NONE;
  object->size = size;
  object->next = mvm->objects;
  mvm->objects = object;
  mvm->gc_bytes += total;
  mvm_runtime_unlock(mvm);
  return object;
}

mvm_result_t mvm_alloc(mvm_t *mvm, size_t size, mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  mvm_object_t *object = mvm_gc_allocate(mvm, MVM_OBJECT_RAW, size);
  if (!object)
    return MVM_RESULT_VM_ERROR;
  *result = (mvm_value_t){.kind = MVM_KIND_REFERENCE, .as.ptr = object->data};
  return MVM_RESULT_OK;
}
