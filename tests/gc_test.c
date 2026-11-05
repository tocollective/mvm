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
#include <stdint.h>
#include <string.h>

static const mvm_value_t null_value = {.kind = MVM_KIND_NULL};

static mvm_t *create_vm(void) {
  mvm_t *vm = test_vm_create(NULL, 16, MVM_FRAME_DEFAULT_LIMIT);
  assert(vm);
  return vm;
}

static void push(mvm_t *vm, mvm_value_t value) {
  assert(mvm_stack_push(vm->stack, value));
}

static mvm_result_t run(mvm_t *vm, mvm_opcodes_t op, mvm_kind_t kind) {
  vm->ram->data[0] = (uint8_t)op;
  vm->ram->data[1] = (uint8_t)kind;
  vm->pc = 0;
  return mvm_update(vm);
}

static mvm_value_t number(uint64_t value) {
  return (mvm_value_t){.kind = MVM_KIND_ULONG, .as.ul = value};
}

static mvm_value_t array_new(mvm_t *vm, mvm_kind_t kind, size_t length) {
  mvm_value_t value;
  assert(mvm_array_new(vm, kind, length, &value) == MVM_RESULT_OK);
  push(vm, value);
  return value;
}

static mvm_value_t concat(mvm_t *vm, const char *lhs, const char *rhs) {
  push(vm, (mvm_value_t){.kind = MVM_KIND_STRING, .as.s = (char *)lhs});
  push(vm, (mvm_value_t){.kind = MVM_KIND_STRING, .as.s = (char *)rhs});
  assert(run(vm, MVM_OP_CONCAT, 0) == MVM_RESULT_OK);
  return vm->stack->data[vm->stack->size - 1];
}

static void test_alloc(void) {
  mvm_t *vm = create_vm();
  const mvm_value_t sizes[] = {
      {.kind = MVM_KIND_UBYTE, .as.ub = 0},
      {.kind = MVM_KIND_UHALF, .as.uh = 1},
      {.kind = MVM_KIND_UWORD, .as.uw = 8},
      {.kind = MVM_KIND_ULONG, .as.ul = 17},
      {.kind = MVM_KIND_SBYTE, .as.sb = 1},
      {.kind = MVM_KIND_SHALF, .as.sh = 2},
      {.kind = MVM_KIND_SWORD, .as.sw = 3},
      {.kind = MVM_KIND_SLONG, .as.sl = 4},
  };
  const size_t lengths[] = {0, 1, 8, 17, 1, 2, 3, 4};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i) {
    push(vm, sizes[i]);
    vm->gc_next_collection = 0;
    assert(run(vm, MVM_OP_ALLOC, 0) == MVM_RESULT_OK);
    const mvm_value_t value = vm->stack->data[i];
    assert(value.kind == MVM_KIND_REFERENCE && value.as.ptr);
    assert((uintptr_t)value.as.ptr % _Alignof(max_align_t) == 0);
    if (i) {
      assert(value.as.ptr != vm->stack->data[i - 1].as.ptr);
      for (size_t j = 0; j < lengths[i]; ++j)
        assert(((unsigned char *)value.as.ptr)[j] == 0);
    }
  }
  assert(mvm_gc_collect(vm) == 0);
  mvm_stack_clear(vm->stack);
  assert(mvm_gc_collect(vm) == sizeof(sizes) / sizeof(*sizes));
  assert(!vm->objects && !vm->gc_bytes);
  // Overflow is rejected before an allocation is attempted.
  mvm_value_t output = number(42);
  assert(mvm_alloc(vm, SIZE_MAX, &output) == MVM_RESULT_VM_ERROR);
  assert(output.kind == MVM_KIND_ULONG && output.as.ul == 42);
  mvm_destroy(vm);
}

static void test_array_defaults(void) {
  mvm_t *vm = create_vm();
  for (mvm_kind_t kind = MVM_KIND_NULL; kind < MVM_KIND_COUNT; ++kind) {
    if (kind == MVM_KIND_PACKET || kind == MVM_KIND_UNION)
      continue;
    push(vm, number(2));
    vm->gc_next_collection = 0;
    assert(run(vm, MVM_OP_ARRAY_NEW, kind) == MVM_RESULT_OK);
    mvm_value_t array = vm->stack->data[vm->stack->size - 1];
    assert(array.kind == MVM_KIND_ARRAY);
    size_t length;
    assert(mvm_array_length(vm, array, &length) == MVM_RESULT_OK && length == 2);
    mvm_value_t element;
    assert(mvm_array_get(vm, array, 1, &element) == MVM_RESULT_OK);
    if (kind == MVM_KIND_STRING || kind == MVM_KIND_ARRAY ||
        kind == MVM_KIND_REFERENCE || kind == MVM_KIND_FUNC ||
        kind == MVM_KIND_THREAD || kind == MVM_KIND_MUTEX ||
        kind == MVM_KIND_CONDITION || kind == MVM_KIND_ATOMIC) {
      assert(element.kind == MVM_KIND_NULL);
      assert(mvm_array_set(vm, array, 0, null_value) == MVM_RESULT_OK);
    } else {
      assert(element.kind == kind);
      switch (kind) {
      case MVM_KIND_BOOL: assert(!element.as.b); break;
      case MVM_KIND_UBYTE: assert(!element.as.ub); break;
      case MVM_KIND_UHALF: assert(!element.as.uh); break;
      case MVM_KIND_UWORD: assert(!element.as.uw); break;
      case MVM_KIND_ULONG: assert(!element.as.ul); break;
      case MVM_KIND_SBYTE: assert(!element.as.sb); break;
      case MVM_KIND_SHALF: assert(!element.as.sh); break;
      case MVM_KIND_SWORD: assert(!element.as.sw); break;
      case MVM_KIND_SLONG: assert(!element.as.sl); break;
      case MVM_KIND_FLOAT: assert(element.as.f == 0.0f); break;
      case MVM_KIND_DOUBLE: assert(element.as.d == 0.0); break;
      case MVM_KIND_TYPE: assert(element.as.type == MVM_KIND_VOID); break;
      default: break;
      }
      assert(mvm_array_set(vm, array, 0, element) == MVM_RESULT_OK);
    }
    push(vm, array);
    assert(run(vm, MVM_OP_ARRAY_LEN, 0) == MVM_RESULT_OK);
    assert(vm->stack->data[vm->stack->size - 1].kind == MVM_KIND_UWORD);
    assert(vm->stack->data[vm->stack->size - 1].as.uw == 2);
    --vm->stack->size;
  }
  mvm_value_t empty = array_new(vm, MVM_KIND_ULONG, 0);
  mvm_value_t result = number(99);
  assert(mvm_array_get(vm, empty, 0, &result) == MVM_RESULT_RUNTIME_ERROR);
  assert(result.as.ul == 99);
  mvm_destroy(vm);
}

static void test_array_opcodes(void) {
  mvm_t *vm = create_vm();
  mvm_value_t array = array_new(vm, MVM_KIND_ULONG, 3);
  push(vm, array);
  push(vm, number(1));
  push(vm, number(123));
  assert(run(vm, MVM_OP_ARRAY_SET, 0) == MVM_RESULT_OK);
  assert(vm->stack->size == 1);
  push(vm, array);
  push(vm, (mvm_value_t){.kind = MVM_KIND_SBYTE, .as.sb = 1});
  assert(run(vm, MVM_OP_ARRAY_GET, 0) == MVM_RESULT_OK);
  assert(vm->stack->size == 2 && vm->stack->data[1].as.ul == 123);
  --vm->stack->size;
  push(vm, array);
  assert(run(vm, MVM_OP_EQ, 0) == MVM_RESULT_OK);
  assert(vm->stack->data[0].as.b);
  mvm_destroy(vm);
}

static void expect_error(mvm_t *vm, mvm_opcodes_t op, mvm_kind_t kind,
                         const mvm_value_t *values, size_t count,
                         mvm_result_t expected) {
  mvm_stack_clear(vm->stack);
  for (size_t i = 0; i < count; ++i)
    push(vm, values[i]);
  mvm_value_t before[3];
  assert(count <= 3);
  memcpy(before, vm->stack->data, count * sizeof(*before));
  assert(run(vm, op, kind) == expected);
  assert(vm->pc == 1 && vm->stack->size == count);
  assert(!memcmp(before, vm->stack->data, count * sizeof(*before)));
  assert(vm->frame_count == 0);
}

static void test_errors(void) {
  mvm_t *vm = create_vm();
  mvm_value_t array = array_new(vm, MVM_KIND_ULONG, 2);
  mvm_gc_root_t root;
  assert(mvm_gc_root_register(vm, &root, &array, 1));
  const mvm_value_t invalid[] = {
      {.kind = MVM_KIND_SBYTE, .as.sb = -1},
      {.kind = MVM_KIND_SHALF, .as.sh = -1},
      {.kind = MVM_KIND_SWORD, .as.sw = -1},
      {.kind = MVM_KIND_SLONG, .as.sl = -1},
      {.kind = MVM_KIND_BOOL, .as.b = true},
      {.kind = MVM_KIND_DOUBLE, .as.d = 1.0},
      {.kind = MVM_KIND_NULL},
  };
  for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
    expect_error(vm, MVM_OP_ALLOC, 0, &invalid[i], 1, MVM_RESULT_RUNTIME_ERROR);
    expect_error(vm, MVM_OP_ARRAY_NEW, MVM_KIND_ULONG, &invalid[i], 1,
                 MVM_RESULT_RUNTIME_ERROR);
    mvm_value_t values[] = {array, invalid[i], number(4)};
    expect_error(vm, MVM_OP_ARRAY_GET, 0, values, 2, MVM_RESULT_RUNTIME_ERROR);
    expect_error(vm, MVM_OP_ARRAY_SET, 0, values, 3, MVM_RESULT_RUNTIME_ERROR);
  }
  const mvm_value_t too_large = number(UINT64_MAX);
  expect_error(vm, MVM_OP_ALLOC, 0, &too_large, 1,
               SIZE_MAX < UINT64_MAX ? MVM_RESULT_RUNTIME_ERROR
                                     : MVM_RESULT_VM_ERROR);
  expect_error(vm, MVM_OP_ARRAY_NEW, MVM_KIND_ULONG, &too_large, 1,
               MVM_RESULT_RUNTIME_ERROR);
  const mvm_value_t length = number(2);
  const mvm_kind_t bad_kinds[] = {MVM_KIND_VOID, MVM_KIND_COUNT, 255};
  for (size_t i = 0; i < sizeof(bad_kinds) / sizeof(*bad_kinds); ++i)
    expect_error(vm, MVM_OP_ARRAY_NEW, bad_kinds[i], &length, 1,
                 MVM_RESULT_RUNTIME_ERROR);
  mvm_value_t values[] = {array, number(2), number(4)};
  expect_error(vm, MVM_OP_ARRAY_GET, 0, values, 2, MVM_RESULT_RUNTIME_ERROR);
  expect_error(vm, MVM_OP_ARRAY_SET, 0, values, 3, MVM_RESULT_RUNTIME_ERROR);
  values[1] = number(0);
  values[2] = null_value;
  expect_error(vm, MVM_OP_ARRAY_SET, 0, values, 3, MVM_RESULT_RUNTIME_ERROR);
  values[0] = number(4);
  expect_error(vm, MVM_OP_ARRAY_GET, 0, values, 2, MVM_RESULT_RUNTIME_ERROR);
  expect_error(vm, MVM_OP_ARRAY_LEN, 0, values, 1, MVM_RESULT_RUNTIME_ERROR);
  // Fabricated handles must not be dereferenced.
  values[0] = (mvm_value_t){.kind = MVM_KIND_ARRAY, .as.array = (void *)vm};
  expect_error(vm, MVM_OP_ARRAY_LEN, 0, values, 1, MVM_RESULT_RUNTIME_ERROR);
  mvm_value_t element;
  assert(mvm_array_get(vm, array, 0, &element) == MVM_RESULT_OK);
  assert(element.kind == MVM_KIND_ULONG && element.as.ul == 0);
  const mvm_opcodes_t ops[] = {MVM_OP_ALLOC, MVM_OP_ARRAY_NEW,
      MVM_OP_ARRAY_GET, MVM_OP_ARRAY_SET, MVM_OP_ARRAY_LEN};
  for (size_t i = 0; i < sizeof(ops) / sizeof(*ops); ++i)
    expect_error(vm, ops[i], MVM_KIND_ULONG, values, 0, MVM_RESULT_RUNTIME_ERROR);
  mvm_gc_root_unregister(vm, &root);
  mvm_destroy(vm);

  vm = test_vm_create(NULL, 1, 1);
  assert(vm);
  push(vm, length);
  vm->ram->data[0] = MVM_OP_ARRAY_NEW;
  assert(mvm_update(vm) == MVM_RESULT_RUNTIME_ERROR);
  assert(vm->pc == 1 && vm->stack->size == 1);
  mvm_destroy(vm);
}

static void test_gc_graph(void) {
  mvm_t *vm = create_vm();
  mvm_value_t parent = array_new(vm, MVM_KIND_ARRAY, 3);
  mvm_value_t strings = array_new(vm, MVM_KIND_STRING, 1);
  mvm_value_t refs = array_new(vm, MVM_KIND_REFERENCE, 1);
  mvm_value_t text = concat(vm, "nested", " string");
  mvm_value_t raw;
  assert(mvm_alloc(vm, 8, &raw) == MVM_RESULT_OK);
  assert(mvm_array_set(vm, strings, 0, text) == MVM_RESULT_OK);
  assert(mvm_array_set(vm, refs, 0, raw) == MVM_RESULT_OK);
  assert(mvm_array_set(vm, parent, 0, strings) == MVM_RESULT_OK);
  assert(mvm_array_set(vm, parent, 1, refs) == MVM_RESULT_OK);
  assert(mvm_array_set(vm, parent, 2, parent) == MVM_RESULT_OK);
  assert(mvm_stack_truncate(vm->stack, 1));
  assert(mvm_gc_collect(vm) == 0);
  assert(!strcmp(text.as.s, "nested string"));
  mvm_value_t output;
  assert(mvm_array_get(vm, strings, 0, &output) == MVM_RESULT_OK);
  assert(output.as.s == text.as.s);
  assert(mvm_array_set(vm, parent, 0, null_value) == MVM_RESULT_OK);
  assert(mvm_gc_collect(vm) == 2); // strings and its string
  mvm_stack_clear(vm->stack);
  assert(mvm_gc_collect(vm) == 3); // cyclic parent, refs and raw
  assert(!vm->gc_bytes && !vm->objects);
  mvm_destroy(vm);
}

static void test_roots_and_aliases(void) {
  mvm_t *vm = create_vm();
  mvm_value_t roots[2];
  assert(mvm_alloc(vm, 8, &roots[0]) == MVM_RESULT_OK);
  roots[1] = array_new(vm, MVM_KIND_ARRAY, 1);
  mvm_gc_root_t root, second;
  assert(!mvm_gc_root_register(NULL, &root, roots, 2));
  assert(!mvm_gc_root_register(vm, &root, NULL, 1));
  assert(mvm_gc_root_register(vm, &root, roots, 2));
  assert(!mvm_gc_root_register(vm, &root, roots, 2));
  assert(mvm_gc_root_register(vm, &second, NULL, 0));
  mvm_stack_clear(vm->stack);
  assert(mvm_gc_collect(vm) == 0);
  // Root contents are read afresh, and roots can be removed out of order.
  roots[0].as.ptr = (unsigned char *)roots[0].as.ptr + 3;
  vm->gc_next_collection = 0;
  mvm_value_t garbage;
  assert(mvm_alloc(vm, 8, &garbage) == MVM_RESULT_OK);
  assert(mvm_gc_collect(vm) == 1);
  mvm_gc_root_unregister(vm, &root);
  mvm_gc_root_unregister(vm, &root);
  assert(mvm_gc_collect(vm) == 2);
  mvm_gc_root_unregister(vm, &second);
  assert(!vm->gc_roots);
  mvm_value_t text = concat(vm, "prefix", "suffix");
  vm->stack->data[0] = (mvm_value_t){.kind = MVM_KIND_STRING, .as.s = text.as.s + 6};
  assert(mvm_gc_collect(vm) == 0);
  assert(run(vm, MVM_OP_STRING_LEN, 0) == MVM_RESULT_OK);
  assert(vm->stack->data[0].as.uw == 6);
  assert(mvm_gc_collect(vm) == 1);
  mvm_destroy(vm);
}

static void test_global_array_root(void) {
  mvm_t *vm = create_vm();
  mvm_value_t parent = array_new(vm, MVM_KIND_ARRAY, 1);
  mvm_value_t child = array_new(vm, MVM_KIND_STRING, 1);
  mvm_value_t text = concat(vm, "global", " array");
  assert(mvm_array_set(vm, parent, 0, child) == MVM_RESULT_OK);
  assert(mvm_array_set(vm, child, 0, text) == MVM_RESULT_OK);
  mvm_stack_clear(vm->stack);
  push(vm, parent);
  const uint8_t program[] = {MVM_OP_GLOBAL_SET, 0, 0, 0, 0};
  memcpy(vm->ram->data, program, sizeof(program));
  vm->pc = 0;
  assert(mvm_update(vm) == MVM_RESULT_OK);
  assert(vm->stack->size == 0 && vm->global_count == 1);
  assert(mvm_gc_collect(vm) == 0);
  assert(!strcmp(text.as.s, "global array"));
  vm->globals[0] = null_value;
  assert(mvm_gc_collect(vm) == 3);
  assert(!vm->gc_bytes);
  mvm_destroy(vm);
}

static void test_managed_string_bounds(void) {
  mvm_t *vm = create_vm();
  mvm_value_t text = concat(vm, "a", "b");
  text.as.s[2] = 'x'; // host overwrites the terminator
  assert(run(vm, MVM_OP_STRING_LEN, 0) == MVM_RESULT_RUNTIME_ERROR);
  assert(vm->stack->size == 1 && vm->stack->data[0].as.s == text.as.s);
  text.as.s[2] = '\0';
  assert(run(vm, MVM_OP_STRING_LEN, 0) == MVM_RESULT_OK);
  assert(vm->stack->data[0].as.uw == 2);
  assert(mvm_gc_collect(vm) == 1);
  mvm_destroy(vm);
}

static void test_deep_cycles(void) {
  mvm_t *vm = create_vm();
  const size_t depth = 4096;
  mvm_value_t head = array_new(vm, MVM_KIND_ARRAY, 1);
  mvm_value_t current = head;
  for (size_t i = 1; i < depth; ++i) {
    mvm_value_t next;
    assert(mvm_array_new(vm, MVM_KIND_ARRAY, 1, &next) == MVM_RESULT_OK);
    assert(mvm_array_set(vm, current, 0, next) == MVM_RESULT_OK);
    current = next;
  }
  assert(mvm_array_set(vm, current, 0, head) == MVM_RESULT_OK);
  assert(mvm_gc_collect(vm) == 0);
  assert(mvm_gc_collect(vm) == 0);
  mvm_stack_clear(vm->stack);
  assert(mvm_gc_collect(vm) == depth);
  mvm_destroy(vm);
}

static void test_frame_roots(void) {
  // The caller's array stays alive while the callee performs ALLOC and returns.
  const uint8_t program[] = {
      MVM_OP_CALL, 7, 0, 0, 0, 0,
      MVM_OP_RETURN,
      MVM_OP_LIT_UBYTE, 1,
      MVM_OP_ALLOC,
      MVM_OP_RETURN,
  };
  mvm_t *vm = create_vm();
  memcpy(vm->ram->data, program, sizeof(program));
  mvm_value_t array = array_new(vm, MVM_KIND_ULONG, 1);
  assert(mvm_update(vm) == MVM_RESULT_OK);
  assert(vm->frame_count == 1 && vm->frames[0].base == 1);
  // No operation may consume a value below the callee's base.
  const mvm_opcodes_t ops[] = {MVM_OP_ALLOC, MVM_OP_ARRAY_NEW,
      MVM_OP_ARRAY_GET, MVM_OP_ARRAY_SET, MVM_OP_ARRAY_LEN};
  for (size_t i = 0; i < sizeof(ops) / sizeof(*ops); ++i) {
    vm->ram->data[7] = (uint8_t)ops[i];
    vm->pc = 7;
    assert(mvm_update(vm) == MVM_RESULT_RUNTIME_ERROR);
    assert(vm->pc == 8 && vm->stack->size == 1 && vm->frame_count == 1);
  }
  vm->ram->data[7] = MVM_OP_LIT_UBYTE;
  vm->pc = 7;
  assert(mvm_update(vm) == MVM_RESULT_OK);
  vm->gc_next_collection = 0;
  assert(mvm_update(vm) == MVM_RESULT_OK);
  assert(mvm_update(vm) == MVM_RESULT_OK);
  assert(vm->stack->size == 2 && vm->stack->data[0].as.array == array.as.array);
  assert(mvm_gc_collect(vm) == 0);
  mvm_destroy(vm);
}

static char *native_collect(mvm_t *vm, char *text, void *raw) {
  // Simulate host work replacing stack storage. The C argument snapshot must
  // remain rooted even when the original argument slots no longer retain it.
  for (size_t i = 0; i < vm->stack->size; ++i)
    vm->stack->data[i] = null_value;
  assert(mvm_gc_collect(vm) == 0);
  assert(!strcmp(text, "native string"));
  assert(*(uint8_t *)raw == 73);
  mvm_value_t temporary;
  assert(mvm_alloc(vm, 4, &temporary) == MVM_RESULT_OK);
  mvm_gc_root_t root;
  assert(mvm_gc_root_register(vm, &root, &temporary, 1));
  vm->gc_next_collection = 0;
  mvm_value_t garbage;
  assert(mvm_alloc(vm, 4, &garbage) == MVM_RESULT_OK);
  assert(mvm_gc_collect(vm) == 1);
  mvm_gc_root_unregister(vm, &root);
  // Also force stack reallocation while libffi holds the argument snapshot.
  const size_t count = vm->stack->size;
  const size_t capacity = vm->stack->capacity;
  for (size_t i = 0; i < capacity; ++i)
    push(vm, null_value);
  assert(mvm_stack_truncate(vm->stack, count));
  return text + 7;
}

static mvm_array_t *native_array_identity(mvm_t *vm, mvm_array_t *array) {
  assert(mvm_gc_collect(vm) == 0);
  return array;
}

static void native_error(mvm_t *vm, char *text) {
  assert(mvm_gc_collect(vm) == 0);
  assert(!strcmp(text, "error path"));
  vm->native_error = MVM_RESULT_RUNTIME_ERROR;
}

static void test_native_roots(void) {
  mvm_t *vm = create_vm();
  const mvm_kind_t kinds[] = {MVM_KIND_REFERENCE, MVM_KIND_STRING, MVM_KIND_REFERENCE};
  assert(mvm_register_native(vm, "collect", (void (*)(void))native_collect,
      MVM_KIND_STRING, 3, kinds, NULL) == MVM_RESULT_OK);
  push(vm, (mvm_value_t){.kind = MVM_KIND_REFERENCE, .as.ptr = vm});
  concat(vm, "native", " string");
  mvm_value_t raw;
  assert(mvm_alloc(vm, 8, &raw) == MVM_RESULT_OK);
  *(uint8_t *)raw.as.ptr = 73;
  push(vm, raw);
  const uint8_t call[] = {MVM_OP_NATIVE, 0, 0, 3};
  memcpy(vm->ram->data, call, sizeof(call));
  vm->pc = 0;
  assert(mvm_update(vm) == MVM_RESULT_OK);
  assert(!vm->gc_roots && vm->stack->size == 1);
  assert(vm->stack->data[0].kind == MVM_KIND_STRING);
  assert(!strcmp(vm->stack->data[0].as.s, "string"));
  assert(mvm_gc_collect(vm) == 2); // raw and native temporary
  mvm_stack_clear(vm->stack);
  assert(mvm_gc_collect(vm) == 1);

  const mvm_kind_t array_args[] = {MVM_KIND_REFERENCE, MVM_KIND_ARRAY};
  assert(mvm_register_native(vm, "arrayIdentity", (void (*)(void))native_array_identity,
      MVM_KIND_ARRAY, 2, array_args, NULL) == MVM_RESULT_OK);
  push(vm, (mvm_value_t){.kind = MVM_KIND_REFERENCE, .as.ptr = vm});
  mvm_value_t array = array_new(vm, MVM_KIND_STRING, 1);
  const uint8_t array_call[] = {MVM_OP_NATIVE, 1, 0, 2};
  memcpy(vm->ram->data, array_call, sizeof(array_call));
  vm->pc = 0;
  assert(mvm_update(vm) == MVM_RESULT_OK);
  assert(vm->stack->size == 1 && vm->stack->data[0].as.array == array.as.array);
  mvm_stack_clear(vm->stack);
  assert(mvm_gc_collect(vm) == 1);

  const mvm_kind_t error_args[] = {MVM_KIND_REFERENCE, MVM_KIND_STRING};
  assert(mvm_register_native(vm, "error", (void (*)(void))native_error,
      MVM_KIND_VOID, 2, error_args, NULL) == MVM_RESULT_OK);
  push(vm, (mvm_value_t){.kind = MVM_KIND_REFERENCE, .as.ptr = vm});
  concat(vm, "error", " path");
  const uint8_t error_call[] = {MVM_OP_NATIVE, 2, 0, 2};
  memcpy(vm->ram->data, error_call, sizeof(error_call));
  vm->pc = 0;
  assert(mvm_update(vm) == MVM_RESULT_RUNTIME_ERROR);
  assert(!vm->gc_roots && vm->stack->size == 2);
  mvm_stack_clear(vm->stack);
  assert(mvm_gc_collect(vm) == 1);
  mvm_destroy(vm);
}

static void test_record_api_and_opcodes(void) {
  mvm_t *vm = create_vm();
  mvm_value_t record;
  assert(mvm_record_new(vm, 2, &record) == MVM_RESULT_OK);
  assert(record.kind == MVM_KIND_REFERENCE && record.as.ptr);
  push(vm, record);

  mvm_value_t output;
  assert(mvm_record_get(vm, record, 0, &output) == MVM_RESULT_OK);
  assert(output.kind == MVM_KIND_NULL);
  assert(mvm_record_get(vm, record, 1, &output) == MVM_RESULT_OK);
  assert(output.kind == MVM_KIND_NULL);
  assert(mvm_record_set(vm, record, 0, number(41)) == MVM_RESULT_OK);
  assert(mvm_record_get(vm, record, 0, &output) == MVM_RESULT_OK);
  assert(output.kind == MVM_KIND_ULONG && output.as.ul == 41);

  push(vm, record);
  push(vm, number(42));
  assert(run(vm, MVM_OP_FIELD_SET, 1) == MVM_RESULT_OK);
  assert(vm->pc == 3 && vm->stack->size == 1);
  assert(vm->stack->data[0].as.ptr == record.as.ptr);
  push(vm, record);
  assert(run(vm, MVM_OP_FIELD_GET, 1) == MVM_RESULT_OK);
  assert(vm->pc == 3 && vm->stack->size == 2);
  assert(vm->stack->data[1].kind == MVM_KIND_ULONG);
  assert(vm->stack->data[1].as.ul == 42);

  output = number(99);
  assert(mvm_record_get(vm, record, 2, &output) == MVM_RESULT_RUNTIME_ERROR);
  assert(output.kind == MVM_KIND_ULONG && output.as.ul == 99);
  mvm_value_t empty;
  assert(mvm_record_new(vm, 0, &empty) == MVM_RESULT_OK);
  assert(mvm_record_get(vm, empty, 0, &output) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm_record_set(vm, empty, 0, null_value) == MVM_RESULT_RUNTIME_ERROR);
  mvm_destroy(vm);
}

static void test_record_errors(void) {
  mvm_t *vm = create_vm();
  mvm_value_t record;
  assert(mvm_record_new(vm, 2, &record) == MVM_RESULT_OK);
  mvm_gc_root_t root;
  assert(mvm_gc_root_register(vm, &root, &record, 1));
  mvm_value_t raw;
  assert(mvm_alloc(vm, 8, &raw) == MVM_RESULT_OK);

  const mvm_value_t invalid_targets[] = {
      {.kind = MVM_KIND_NULL},
      {.kind = MVM_KIND_ULONG, .as.ul = 4},
      {.kind = MVM_KIND_ARRAY},
      {.kind = MVM_KIND_REFERENCE},
      {.kind = MVM_KIND_REFERENCE, .as.ptr = vm},
      raw,
  };
  for (size_t i = 0; i < sizeof(invalid_targets) / sizeof(*invalid_targets);
       ++i) {
    mvm_value_t values[] = {invalid_targets[i], number(42)};
    expect_error(vm, MVM_OP_FIELD_GET, 0, values, 1,
                 MVM_RESULT_RUNTIME_ERROR);
    expect_error(vm, MVM_OP_FIELD_SET, 0, values, 2,
                 MVM_RESULT_RUNTIME_ERROR);
  }
  mvm_value_t values[] = {record, number(42)};
  expect_error(vm, MVM_OP_FIELD_GET, 2, values, 1, MVM_RESULT_RUNTIME_ERROR);
  expect_error(vm, MVM_OP_FIELD_SET, 2, values, 2, MVM_RESULT_RUNTIME_ERROR);
  expect_error(vm, MVM_OP_FIELD_GET, 0, values, 0, MVM_RESULT_RUNTIME_ERROR);
  expect_error(vm, MVM_OP_FIELD_SET, 0, values, 0, MVM_RESULT_RUNTIME_ERROR);
  expect_error(vm, MVM_OP_FIELD_SET, 0, values, 1, MVM_RESULT_RUNTIME_ERROR);

  mvm_value_t output = number(99);
  assert(mvm_record_get(vm, raw, 0, &output) == MVM_RESULT_RUNTIME_ERROR);
  assert(output.kind == MVM_KIND_ULONG && output.as.ul == 99);
  assert(mvm_record_set(vm, raw, 0, number(1)) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm_record_new(vm, SIZE_MAX, &output) == MVM_RESULT_VM_ERROR);
  assert(output.kind == MVM_KIND_ULONG && output.as.ul == 99);
  mvm_gc_root_unregister(vm, &root);
  mvm_destroy(vm);

  const mvm_opcodes_t ops[] = {MVM_OP_FIELD_GET, MVM_OP_FIELD_SET};
  for (size_t i = 0; i < sizeof(ops) / sizeof(*ops); ++i) {
    for (size_t available = 0; available < 2; ++available) {
      vm = test_vm_create(NULL, 1 + available, 1);
      assert(vm);
      vm->ram->data[0] = (uint8_t)ops[i];
      push(vm, null_value);
      assert(mvm_update(vm) == MVM_RESULT_RUNTIME_ERROR);
      assert(vm->pc == 1 && vm->stack->size == 1);
      assert(vm->stack->data[0].kind == MVM_KIND_NULL);
      mvm_destroy(vm);
    }

    vm = test_vm_create(NULL, 3, 1);
    assert(vm);
    vm->ram->data[0] = (uint8_t)ops[i];
    mvm_stack_destroy(vm->stack);
    vm->stack = NULL;
    assert(mvm_update(vm) == MVM_RESULT_VM_ERROR);
    assert(vm->pc == 1);
    mvm_destroy(vm);
  }
}

static void test_record_gc(void) {
  mvm_t *vm = create_vm();
  mvm_value_t parent;
  mvm_value_t child;
  assert(mvm_record_new(vm, 2, &parent) == MVM_RESULT_OK);
  push(vm, parent);
  assert(mvm_record_new(vm, 1, &child) == MVM_RESULT_OK);
  push(vm, child);
  const mvm_value_t text = concat(vm, "record", " field");
  assert(mvm_record_set(vm, parent, 0, child) == MVM_RESULT_OK);
  assert(mvm_record_set(vm, child, 0, parent) == MVM_RESULT_OK);
  assert(mvm_record_set(vm, parent, 1, text) == MVM_RESULT_OK);
  assert(mvm_stack_truncate(vm->stack, 1));

  assert(mvm_gc_collect(vm) == 0);
  assert(!strcmp(text.as.s, "record field"));
  mvm_value_t output;
  assert(mvm_record_get(vm, child, 0, &output) == MVM_RESULT_OK);
  assert(output.as.ptr == parent.as.ptr);
  assert(mvm_record_set(vm, parent, 1, null_value) == MVM_RESULT_OK);
  assert(mvm_gc_collect(vm) == 1);
  mvm_stack_clear(vm->stack);
  assert(mvm_gc_collect(vm) == 2);
  assert(!vm->objects && !vm->gc_bytes);
  mvm_destroy(vm);
}

static void test_record_frame_boundaries(void) {
  const mvm_opcodes_t ops[] = {MVM_OP_FIELD_GET, MVM_OP_FIELD_SET};
  for (size_t i = 0; i < sizeof(ops) / sizeof(*ops); ++i) {
    const uint8_t program[] = {
        MVM_OP_CALL, 6, 0, 0, 0, 0,
        ops[i], 0, 0,
    };
    mvm_t *vm = create_vm();
    memcpy(vm->ram->data, program, sizeof(program));
    mvm_value_t record;
    assert(mvm_record_new(vm, 1, &record) == MVM_RESULT_OK);
    push(vm, record);
    assert(mvm_update(vm) == MVM_RESULT_OK);
    assert(vm->frame_count == 1 && vm->frames[0].base == 1);
    assert(mvm_update(vm) == MVM_RESULT_RUNTIME_ERROR);
    assert(vm->pc == 7 && vm->stack->size == 1 && vm->frame_count == 1);
    assert(vm->stack->data[0].as.ptr == record.as.ptr);
    mvm_destroy(vm);
  }
}

int main(void) {
  test_alloc();
  test_array_defaults();
  test_array_opcodes();
  test_errors();
  test_gc_graph();
  test_roots_and_aliases();
  test_global_array_root();
  test_managed_string_bounds();
  test_deep_cycles();
  test_frame_roots();
  test_native_roots();
  test_record_api_and_opcodes();
  test_record_errors();
  test_record_gc();
  test_record_frame_boundaries();
  return 0;
}
