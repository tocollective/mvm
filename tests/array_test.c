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

#include "vm_fixture.h"
#include "mvm.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>
#include <string.h>

extern size_t array_test_allocation_failures;
extern size_t array_test_allocation_skip;

static mvm_value_t number(uint64_t n) {
  return (mvm_value_t){.kind = MVM_KIND_ULONG, .as.ul = n};
}

static mvm_t *create_vm(void) {
  mvm_t *vm = test_vm_create(NULL, 32, 8);
  assert(vm);
  return vm;
}

static size_t length(mvm_t *vm, mvm_value_t array) {
  size_t n;
  assert(mvm_array_length(vm, array, &n) == MVM_RESULT_OK);
  return n;
}

static size_t capacity(mvm_t *vm, mvm_value_t array) {
  size_t n;
  assert(mvm_array_capacity(vm, array, &n) == MVM_RESULT_OK);
  return n;
}

static mvm_value_t get(mvm_t *vm, mvm_value_t array, size_t index) {
  mvm_value_t result;
  assert(mvm_array_get(vm, array, index, &result) == MVM_RESULT_OK);
  return result;
}

static void test_growth_and_identity(void) {
  mvm_t *vm = create_vm();
  mvm_value_t array;
  assert(mvm_array_new(vm, MVM_KIND_ULONG, 0, &array) == MVM_RESULT_OK);
  const mvm_value_t alias = array;
  const size_t empty_bytes = vm->gc_bytes;
  assert(capacity(vm, array) == 0);
  for (size_t i = 0; i < 2048; ++i) {
    vm->gc_next_collection = 0; // The C receiver is rooted by the operation.
    assert(mvm_array_push(vm, array, number(i)) == MVM_RESULT_OK);
    assert(length(vm, alias) == i + 1);
    assert(array.as.array == alias.as.array);
  }
  assert(vm->gc_bytes ==
         empty_bytes + capacity(vm, array) * sizeof(mvm_value_t));
  for (size_t i = 0; i < 2048; ++i)
    assert(get(vm, alias, i).as.ul == i);
  assert(mvm_array_reserve(vm, array, 4096) == MVM_RESULT_OK);
  const size_t bytes = vm->gc_bytes;
  assert(mvm_array_reserve(vm, array, 1) == MVM_RESULT_OK);
  assert(capacity(vm, array) == 4096 && length(vm, array) == 2048);
  assert(mvm_array_resize(vm, array, 3) == MVM_RESULT_OK);
  assert(mvm_array_resize(vm, array, 2048) == MVM_RESULT_OK);
  assert(get(vm, array, 2).as.ul == 2 && get(vm, array, 3).as.ul == 0);
  assert(get(vm, array, 2047).as.ul == 0 && vm->gc_bytes == bytes);
  assert(mvm_gc_collect(vm) == 1 && vm->gc_bytes == 0);
  mvm_destroy(vm);
}

static void test_resize_defaults(void) {
  mvm_t *vm = create_vm();
  for (mvm_kind_t kind = MVM_KIND_NULL; kind < MVM_KIND_COUNT; ++kind) {
    if (kind == MVM_KIND_PACKET || kind == MVM_KIND_UNION)
      continue;
    mvm_value_t array;
    assert(mvm_array_new(vm, kind, 1, &array) == MVM_RESULT_OK);
    const mvm_value_t initial = get(vm, array, 0);
    assert(mvm_array_resize(vm, array, 17) == MVM_RESULT_OK);
    const mvm_value_t added = get(vm, array, 16);
    assert(added.kind == initial.kind);
    // The existing defaults test verifies every active scalar union member.
    assert(mvm_array_set(vm, array, 16, initial) == MVM_RESULT_OK);
    assert(mvm_array_resize(vm, array, 0) == MVM_RESULT_OK);
    assert(mvm_array_resize(vm, array, 17) == MVM_RESULT_OK);
    assert(get(vm, array, 0).kind == initial.kind);
    assert(mvm_gc_collect(vm) == 1);
  }
  assert(vm->gc_bytes == 0);
  mvm_destroy(vm);
}

static void test_reference_lifetimes(void) {
  mvm_t *vm = create_vm();
  mvm_value_t array, child;
  assert(mvm_array_new(vm, MVM_KIND_ARRAY, 0, &array) == MVM_RESULT_OK);
  mvm_gc_root_t root;
  assert(mvm_gc_root_register(vm, &root, &array, 1));
  assert(mvm_array_new(vm, MVM_KIND_ULONG, 1, &child) == MVM_RESULT_OK);
  // Neither child nor target need another explicit root inside push.
  vm->gc_next_collection = 0;
  assert(mvm_array_push(vm, array, child) == MVM_RESULT_OK);
  assert(mvm_gc_collect(vm) == 0);
  assert(length(vm, get(vm, array, 0)) == 1);
  assert(mvm_array_resize(vm, array, 0) == MVM_RESULT_OK);
  assert(mvm_gc_collect(vm) == 1);
  assert(mvm_array_push(vm, array, array) == MVM_RESULT_OK);
  mvm_value_t removed;
  assert(mvm_array_pop(vm, array, &removed) == MVM_RESULT_OK);
  assert(removed.as.array == array.as.array && length(vm, array) == 0);
  assert(mvm_array_push(vm, array, array) == MVM_RESULT_OK);
  mvm_gc_root_unregister(vm, &root);
  assert(mvm_gc_collect(vm) == 1 && vm->gc_bytes == 0);
  mvm_destroy(vm);
}

static void test_edits_against_model(void) {
  mvm_t *vm = create_vm();
  mvm_value_t array;
  assert(mvm_array_new(vm, MVM_KIND_ULONG, 0, &array) == MVM_RESULT_OK);
  uint64_t model[128];
  size_t count = 0;
  uint32_t random = 12345;
  for (size_t step = 0; step < 4000; ++step) {
    random = random * UINT32_C(1664525) + UINT32_C(1013904223);
    const bool add = !count || (count < 128 && (random & 0x100));
    const size_t index = (random >> 9) % (add ? count + 1 : count);
    if (add) {
      assert(mvm_array_insert(vm, array, index, number(random)) ==
             MVM_RESULT_OK);
      memmove(model + index + 1, model + index,
              (count - index) * sizeof(*model));
      model[index] = random;
      ++count;
    } else {
      mvm_value_t removed;
      assert(mvm_array_remove(vm, array, index, &removed) == MVM_RESULT_OK);
      assert(removed.kind == MVM_KIND_ULONG && removed.as.ul == model[index]);
      memmove(model + index, model + index + 1,
              (count - index - 1) * sizeof(*model));
      --count;
    }
    assert(length(vm, array) == count);
    for (size_t i = 0; i < count; ++i)
      assert(get(vm, array, i).as.ul == model[i]);
  }
  mvm_destroy(vm);
}

static void test_allocation_failure(void) {
  mvm_t *vm = create_vm();
  mvm_value_t array;
  assert(mvm_array_new(vm, MVM_KIND_ULONG, 8, &array) == MVM_RESULT_OK);
  const size_t bytes = vm->gc_bytes;
  const mvm_value_t bad = {.kind = MVM_KIND_BOOL, .as.b = true};
  array_test_allocation_failures = 2; // Initial allocation and retry both fail.
  assert(mvm_array_push(vm, array, bad) == MVM_RESULT_RUNTIME_ERROR);
  assert(array_test_allocation_failures == 2); // Type check precedes growth.
  assert(mvm_array_push(vm, array, number(9)) == MVM_RESULT_VM_ERROR);
  assert(length(vm, array) == 8 && capacity(vm, array) == 8);
  assert(vm->gc_bytes == bytes && !vm->gc_roots);
  array_test_allocation_failures = 2;
  assert(mvm_array_resize(vm, array, 64) == MVM_RESULT_VM_ERROR);
  assert(length(vm, array) == 8 && capacity(vm, array) == 8);
  array_test_allocation_failures = 2;
  assert(mvm_array_reserve(vm, array, 64) == MVM_RESULT_VM_ERROR);
  assert(vm->gc_bytes == bytes && !vm->gc_roots);
  array_test_allocation_failures = 1; // Collection and retry succeed.
  assert(mvm_array_push(vm, array, number(9)) == MVM_RESULT_OK);
  assert(length(vm, array) == 9 && get(vm, array, 8).as.ul == 9);
  mvm_gc_root_t root;
  assert(mvm_gc_root_register(vm, &root, &array, 1));
  mvm_value_t output = number(42);
  array_test_allocation_failures = 2;
  assert(mvm_array_new(vm, MVM_KIND_ULONG, 1, &output) == MVM_RESULT_VM_ERROR);
  assert(output.kind == MVM_KIND_ULONG && output.as.ul == 42);
  // Fail buffer allocation after the stable header has been allocated.
  array_test_allocation_skip = 1;
  array_test_allocation_failures = 2;
  assert(mvm_array_new(vm, MVM_KIND_ULONG, 1, &output) == MVM_RESULT_VM_ERROR);
  assert(output.kind == MVM_KIND_ULONG && output.as.ul == 42);
  assert(mvm_gc_collect(vm) ==
         1); // The unpublished, empty header is collectable.
  mvm_gc_root_unregister(vm, &root);
  mvm_destroy(vm);
}

static mvm_result_t execute(mvm_t *vm, mvm_opcodes_t op,
                            const mvm_value_t *args, size_t count) {
  mvm_stack_clear(vm->stack);
  for (size_t i = 0; i < count; ++i)
    assert(mvm_stack_push(vm->stack, args[i]));
  vm->ram->data[0] = (uint8_t)op;
  vm->pc = 0;
  return mvm_update(vm);
}

static void test_opcodes_and_errors(void) {
  mvm_t *vm = create_vm();
  mvm_t *foreign = create_vm();
  vm->frames = mvm_memory_calloc(vm->memory, 1, sizeof(*vm->frames));
  assert(vm->frames);
  vm->frame_capacity = 1;
  mvm_value_t array, other;
  assert(mvm_array_new(vm, MVM_KIND_ULONG, 0, &array) == MVM_RESULT_OK);
  assert(mvm_array_new(foreign, MVM_KIND_ARRAY, 0, &other) == MVM_RESULT_OK);
  mvm_gc_root_t root;
  assert(mvm_gc_root_register(vm, &root, &array, 1));
  mvm_value_t args[3] = {array, number(10), number(20)};
  assert(execute(vm, MVM_OP_ARRAY_PUSH, args, 2) == MVM_RESULT_OK);
  assert(vm->stack->size == 0);
  args[1] = number(0);
  assert(execute(vm, MVM_OP_ARRAY_INSERT, args, 3) == MVM_RESULT_OK);
  assert(get(vm, array, 0).as.ul == 20 && get(vm, array, 1).as.ul == 10);
  assert(execute(vm, MVM_OP_ARRAY_REMOVE, args, 2) == MVM_RESULT_OK);
  assert(vm->stack->size == 1 && vm->stack->data[0].as.ul == 20);
  assert(execute(vm, MVM_OP_ARRAY_POP, args, 1) == MVM_RESULT_OK);
  assert(vm->stack->size == 1 && vm->stack->data[0].as.ul == 10);
  args[1] = number(32);
  assert(execute(vm, MVM_OP_ARRAY_RESERVE, args, 2) == MVM_RESULT_OK);
  assert(execute(vm, MVM_OP_ARRAY_CAP, args, 1) == MVM_RESULT_OK);
  assert(vm->stack->size == 1 && vm->stack->data[0].as.uw == 32);
  args[1] = number(3);
  assert(execute(vm, MVM_OP_ARRAY_RESIZE, args, 2) == MVM_RESULT_OK);
  assert(length(vm, array) == 3 && get(vm, array, 2).as.ul == 0);

  const mvm_opcodes_t ops[] = {MVM_OP_ARRAY_CAP,    MVM_OP_ARRAY_RESERVE,
                               MVM_OP_ARRAY_RESIZE, MVM_OP_ARRAY_PUSH,
                               MVM_OP_ARRAY_POP,    MVM_OP_ARRAY_INSERT,
                               MVM_OP_ARRAY_REMOVE};
  const size_t arity[] = {1, 2, 2, 2, 1, 3, 2};
  for (size_t i = 0; i < sizeof(ops) / sizeof(*ops); ++i) {
    args[0] = array;
    args[1] = number(0);
    assert(execute(vm, ops[i], args, arity[i] - 1) == MVM_RESULT_RUNTIME_ERROR);
    assert(vm->stack->size == arity[i] - 1 && vm->pc == 1);
    // A callee may not consume its caller's slots.
    vm->frames[0] = (mvm_frame_t){.base = vm->stack->size};
    vm->frame_count = 1;
    assert(execute(vm, ops[i], args, arity[i] - 1) == MVM_RESULT_RUNTIME_ERROR);
    vm->frame_count = 0;
    const mvm_value_t invalid[] = {
        number(0),
        other,
        {.kind = MVM_KIND_ARRAY, .as.array = (void *)vm},
        {.kind = MVM_KIND_NULL}};
    for (size_t j = 0; j < sizeof(invalid) / sizeof(*invalid); ++j) {
      args[0] = invalid[j];
      assert(execute(vm, ops[i], args, arity[i]) == MVM_RESULT_RUNTIME_ERROR);
      assert(vm->pc == 1 && vm->stack->size == arity[i]);
      assert(!memcmp(args, vm->stack->data, arity[i] * sizeof(*args)));
    }
  }
  args[0] = array;
  const mvm_opcodes_t indexed[] = {MVM_OP_ARRAY_RESERVE, MVM_OP_ARRAY_RESIZE,
                                   MVM_OP_ARRAY_INSERT, MVM_OP_ARRAY_REMOVE};
  const mvm_value_t bad_indices[] = {number(UINT64_MAX),
                                     {.kind = MVM_KIND_SLONG, .as.sl = -1},
                                     {.kind = MVM_KIND_DOUBLE, .as.d = 1.0}};
  for (size_t i = 0; i < sizeof(indexed) / sizeof(*indexed); ++i) {
    const size_t n = indexed[i] == MVM_OP_ARRAY_INSERT ? 3 : 2;
    for (size_t j = 0; j < sizeof(bad_indices) / sizeof(*bad_indices); ++j) {
      args[1] = bad_indices[j];
      assert(execute(vm, indexed[i], args, n) == MVM_RESULT_RUNTIME_ERROR);
      assert(vm->stack->size == n && length(vm, array) == 3);
    }
  }
  args[1] = number(64);
  const size_t bytes = vm->gc_bytes;
  array_test_allocation_failures = 2;
  assert(execute(vm, MVM_OP_ARRAY_RESIZE, args, 2) == MVM_RESULT_VM_ERROR);
  assert(vm->stack->size == 2 && vm->pc == 1);
  assert(vm->gc_bytes == bytes && length(vm, array) == 3);
  assert(mvm_array_resize(vm, array, 0) == MVM_RESULT_OK);
  assert(execute(vm, MVM_OP_ARRAY_POP, args, 1) == MVM_RESULT_RUNTIME_ERROR);
  mvm_value_t output = number(42);
  assert(mvm_array_remove(vm, array, 0, &output) == MVM_RESULT_RUNTIME_ERROR);
  assert(output.as.ul == 42);
  assert(mvm_array_pop(NULL, array, &output) == MVM_RESULT_VM_ERROR);
  assert(mvm_array_pop(vm, array, NULL) == MVM_RESULT_VM_ERROR);
  mvm_gc_root_unregister(vm, &root);
  mvm_destroy(foreign);
  mvm_destroy(vm);
}

int main(void) {
  test_growth_and_identity();
  test_resize_defaults();
  test_reference_lifetimes();
  test_edits_against_model();
  test_allocation_failure();
  test_opcodes_and_errors();
  return 0;
}
