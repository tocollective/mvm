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

#include "../source/mvm/internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <string.h>

static mvm_memory_stats_t stats(mvm_memory_t *memory) {
  mvm_memory_stats_t result;
  assert(mvm_memory_stats(memory, &result));
  assert(result.minimum <= result.committed);
  assert(result.used <= result.committed);
  assert(result.committed <= result.maximum);
  return result;
}

static void test_reserve_and_limits(void) {
  assert(!mvm_memory_create(2, 1));
  assert(!mvm_memory_create(0, 0));
  mvm_memory_t *memory = mvm_memory_create(65536, 131072);
  assert(memory && stats(memory).committed == 65536);
  unsigned char *a = mvm_memory_alloc(memory, 40000);
  assert(a && (uintptr_t)a % _Alignof(max_align_t) == 0);
  memset(a, 0x5a, 40000);
  unsigned char *b = mvm_memory_alloc(memory, 40000);
  assert(b && stats(memory).committed > 65536);
  const size_t before = mvm_memory_failures();
  assert(!mvm_memory_realloc(memory, a, 131072));
  assert(mvm_memory_failures() != before);
  for (size_t i = 0; i < 40000; ++i)
    assert(a[i] == 0x5a);
  assert(!mvm_memory_calloc(memory, SIZE_MAX, 2));
  assert(!mvm_memory_alloc(memory, SIZE_MAX));
  mvm_memory_free(b);
  mvm_memory_free(a);
  assert(stats(memory).committed == 65536);
  a = mvm_memory_calloc(memory, 40000, 1);
  assert(a);
  for (size_t i = 0; i < 40000; ++i)
    assert(a[i] == 0);
  assert(!mvm_memory_realloc(memory, a, 0));
  mvm_memory_destroy(memory);
}

static void test_split_coalesce_and_resize(void) {
  mvm_memory_t *memory = mvm_memory_create(32768, 32768);
  assert(memory);
  const size_t baseline = stats(memory).used;
  unsigned char *a = mvm_memory_alloc(memory, 4096);
  void *b = mvm_memory_alloc(memory, 4096);
  void *c = mvm_memory_alloc(memory, 4096);
  assert(a && b && c);
  memset(a, 0x33, 4096);
  mvm_memory_free(b);
  assert(mvm_memory_realloc(memory, a, 8000) == a);
  for (size_t i = 0; i < 4096; ++i)
    assert(a[i] == 0x33);
  assert(mvm_memory_realloc(memory, a, 64) == a);
  mvm_memory_free(c);
  mvm_memory_free(a);
  assert(stats(memory).used == baseline);
  void *large = mvm_memory_alloc(memory, 30000);
  assert(large);
  mvm_memory_free(large);
  mvm_memory_destroy(memory);
}

static uint32_t random_next(uint32_t *state) {
  *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
  return *state;
}

static void test_randomized_reallocation(void) {
  mvm_memory_t *memory = mvm_memory_create(65536, 262144);
  assert(memory);
  unsigned char *slots[64] = {0};
  size_t lengths[64] = {0};
  uint32_t state = 7;
  for (size_t iteration = 0; iteration < 30000; ++iteration) {
    const size_t index = (random_next(&state) >> 8) % 64;
    for (size_t j = 0; j < lengths[index]; ++j)
      assert(slots[index][j] == (unsigned char)index);
    const size_t length = (random_next(&state) >> 8) % 8192;
    unsigned char *next = mvm_memory_realloc(memory, slots[index], length);
    if (next || !length) {
      const size_t common = length < lengths[index] ? length : lengths[index];
      for (size_t j = 0; j < common; ++j)
        assert(next[j] == (unsigned char)index);
      slots[index] = next;
      lengths[index] = length;
      if (length)
        memset(next, (int)index, length);
    }
    stats(memory);
  }
  for (size_t i = 0; i < 64; ++i)
    mvm_memory_free(slots[i]);
  assert(stats(memory).committed == 65536);
  mvm_memory_destroy(memory);
}

static void test_gc_reclaims_before_oom(void) {
  mvm_t *vm = mvm_create(NULL, 0, 1024 * 1024, 64);
  assert(vm && vm->ram->size == 0);
  const size_t baseline = stats(vm->memory).used;
  for (size_t i = 0; i < 32; ++i) {
    mvm_value_t value;
    // Each new array crosses the pool limit while the previous one is garbage.
    assert(mvm_array_new(vm, MVM_KIND_SLONG, 8192, &value) == MVM_RESULT_OK);
    stats(vm->memory);
  }
  assert(vm->gc_collections > 0);
  mvm_gc_collect(vm);
  assert(stats(vm->memory).used == baseline);
  mvm_value_t live;
  assert(mvm_array_new(vm, MVM_KIND_SLONG, 8192, &live) == MVM_RESULT_OK);
  mvm_gc_root_t root;
  assert(mvm_gc_root_register(vm, &root, &live, 1));
  assert(mvm_array_reserve(vm, live, 1000000) == MVM_RESULT_VM_ERROR);
  size_t length;
  assert(mvm_array_length(vm, live, &length) == MVM_RESULT_OK &&
         length == 8192);
  mvm_gc_root_unregister(vm, &root);
  mvm_destroy(vm);
}

static void test_stacks_share_budget(void) {
  mvm_t *vm = mvm_create(NULL, 0, 512 * 1024, 64);
  assert(vm);
  mvm_value_t live;
  assert(mvm_alloc(vm, 200000, &live) == MVM_RESULT_OK);
  assert(mvm_stack_push(vm->stack, live));
  const size_t before = stats(vm->memory).used;
  const size_t capacity = vm->stack->capacity;
  assert(!mvm_stack_reserve(vm->stack, 20000));
  assert(vm->stack->capacity == capacity && stats(vm->memory).used == before);
  mvm_destroy(vm);
}

static void test_worker_oom_and_shared_pool(void) {
  mvm_t *vm = mvm_create(NULL, 0, 1024 * 1024, 64);
  assert(vm && mvm_ram_resize(vm->ram, 8));
  const uint8_t program[] = {
      MVM_OP_LIT_UWORD, 0xff,           0xff,         0xff, 0x7f,
      MVM_OP_ARRAY_NEW, MVM_KIND_SLONG, MVM_OP_RETURN};
  memcpy(vm->ram->data, program, sizeof(program));
  mvm_value_t handles[2] = {0};
  mvm_gc_root_t root;
  assert(mvm_gc_root_register(vm, &root, handles, 2));
  for (size_t i = 0; i < 2; ++i) {
    assert(mvm_thread_spawn(vm,
                            (mvm_value_t){.kind = MVM_KIND_FUNC, .as.func = 0},
                            NULL, 0, &handles[i]) == MVM_RESULT_OK);
  }
  for (size_t i = 0; i < 2; ++i) {
    mvm_value_t output;
    assert(mvm_thread_join(vm, handles[i], &output) ==
           MVM_RESULT_OUT_OF_MEMORY);
  }
  stats(vm->memory);
  mvm_gc_root_unregister(vm, &root);
  mvm_destroy(vm);
}

static void test_detached_worker_pool_lifetime(void) {
  for (size_t i = 0; i < 128; ++i) {
    mvm_t *vm = mvm_create(NULL, 0, 1024 * 1024, 64);
    assert(vm && mvm_ram_resize(vm->ram, 2));
    vm->ram->data[0] = MVM_OP_LIT_NULL;
    vm->ram->data[1] = MVM_OP_RETURN;
    mvm_value_t thread;
    assert(mvm_thread_spawn(vm, (mvm_value_t){.kind = MVM_KIND_FUNC}, NULL, 0,
                            &thread) == MVM_RESULT_OK);
    assert(mvm_thread_detach(vm, thread) == MVM_RESULT_OK);
    mvm_destroy(vm);
  }
}

int main(void) {
  test_reserve_and_limits();
  test_split_coalesce_and_resize();
  test_randomized_reallocation();
  test_gc_reclaims_before_oom();
  test_stacks_share_budget();
  test_worker_oom_and_shared_pool();
  test_detached_worker_pool_lifetime();
  return 0;
}
