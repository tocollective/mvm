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
#include "../source/mvm/internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

#ifdef UNION_TEST_ALLOCATOR
extern size_t array_test_allocation_failures;
extern size_t array_test_allocation_skip;
#endif

static uint32_t primitive(mvm_t *vm, mvm_kind_t kind, size_t size) {
  const mvm_type_desc_t type = {.tag = MVM_TYPE_PRIMITIVE, .kind = kind,
                                .size = size, .alignment = size ? size : 1,
                                .flags = kind == MVM_KIND_STRING
                                             ? MVM_TYPE_FLAG_NULLABLE : 0};
  uint32_t id;
  assert(mvm_type_register(vm, &type, &id) == MVM_RESULT_OK);
  return id;
}

static uint32_t union_type(mvm_t *vm, const uint32_t *variants, size_t count) {
  const mvm_type_desc_t type = {.tag = MVM_TYPE_UNION, .kind = MVM_KIND_UNION,
      .size = 8, .alignment = 8, .variant_count = count, .variant_type_ids = variants};
  uint32_t id;
  assert(mvm_type_register(vm, &type, &id) == MVM_RESULT_OK);
  return id;
}

static uint32_t array_type(mvm_t *vm, uint32_t inner) {
  const mvm_type_desc_t type = {.tag = MVM_TYPE_ARRAY, .kind = MVM_KIND_ARRAY,
      .size = 8, .alignment = 8, .inner_type_id = inner};
  uint32_t id;
  assert(mvm_type_register(vm, &type, &id) == MVM_RESULT_OK);
  return id;
}

static mvm_value_t word(int32_t n) {
  return (mvm_value_t){.kind = MVM_KIND_SWORD, .as.sw = n};
}

static void test_values_and_gc(void) {
  mvm_t *vm = test_vm_create(NULL, 128, 64);
  assert(vm);
  uint32_t w = primitive(vm, MVM_KIND_SWORD, 4);
  uint32_t str = primitive(vm, MVM_KIND_STRING, 8);
  uint32_t b = primitive(vm, MVM_KIND_BOOL, 1);
  const uint32_t variants[] = {w, str};
  uint32_t u = union_type(vm, variants, 2);
  uint32_t wide = union_type(vm, (uint32_t[]){str, b, w}, 3);
  uint32_t other = union_type(vm, (uint32_t[]){str, b}, 2);
  assert(!mvm_type_defaultable(vm, u));
  mvm_value_t roots[4] = {{.kind = MVM_KIND_NULL}, {.kind = MVM_KIND_NULL},
                           {.kind = MVM_KIND_NULL}, {.kind = MVM_KIND_NULL}};
  mvm_gc_root_t root;
  assert(mvm_gc_root_register(vm, &root, roots, 4));
  vm->gc_next_collection = 0;
  assert(mvm_union_new(vm, u, w, word(42), &roots[0]) == MVM_RESULT_OK);
  mvm_value_t out = word(-1);
  assert(mvm_union_variantof(vm, roots[0], &out) == MVM_RESULT_OK);
  assert(out.kind == MVM_KIND_TYPE && out.has_full_type && out.as.type == w);
  assert(mvm_union_get(vm, roots[0], str, &out) == MVM_RESULT_RUNTIME_ERROR);
  assert(out.kind == MVM_KIND_TYPE && out.as.type == w);
  assert(mvm_union_get(vm, roots[0], w, &out) == MVM_RESULT_OK && out.as.sw == 42);
  assert(mvm_union_new(vm, u, b, word(1), &out) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm_union_new(vm, u, str, word(1), &out) == MVM_RESULT_RUNTIME_ERROR);
  assert(out.kind == MVM_KIND_SWORD && out.as.sw == 42);
  vm->gc_next_collection = 0;
  assert(mvm_union_widen(vm, roots[0], wide, &roots[1]) == MVM_RESULT_OK);
  assert(mvm_union_get(vm, roots[1], w, &out) == MVM_RESULT_OK && out.as.sw == 42);
  assert(mvm_union_widen(vm, roots[1], u, &out) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm_union_new(vm, u, str, (mvm_value_t){.kind = MVM_KIND_NULL}, &roots[2]) == MVM_RESULT_OK);
  assert(mvm_union_variantof(vm, roots[2], &out) == MVM_RESULT_OK && out.as.type == str);
  assert(mvm_union_get(vm, roots[2], str, &out) == MVM_RESULT_OK && out.kind == MVM_KIND_NULL);
  uint32_t arr = array_type(vm, u);
  assert(mvm_array_new_typed(vm, arr, 1, &out) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm_array_new_typed(vm, arr, 0, &roots[3]) == MVM_RESULT_OK);
  vm->gc_next_collection = 0;
  assert(mvm_array_push(vm, roots[3], roots[0]) == MVM_RESULT_OK);
  assert(mvm_array_resize(vm, roots[3], 2) == MVM_RESULT_RUNTIME_ERROR);
  mvm_value_t wrong;
  assert(mvm_union_new(vm, other, b, (mvm_value_t){.kind = MVM_KIND_BOOL}, &wrong) == MVM_RESULT_OK);
  assert(mvm_array_set(vm, roots[3], 0, wrong) == MVM_RESULT_RUNTIME_ERROR);
  roots[0] = (mvm_value_t){.kind = MVM_KIND_NULL};
  mvm_gc_collect(vm);
  assert(mvm_array_get(vm, roots[3], 0, &out) == MVM_RESULT_OK);
  assert(mvm_union_get(vm, out, w, &out) == MVM_RESULT_OK && out.as.sw == 42);
  mvm_t *foreign = test_vm_create(NULL, 128, 64);
  assert(foreign);
  assert(mvm_type_clone(foreign, vm) == MVM_RESULT_OK);
  assert(mvm_union_variantof(foreign, roots[1], &out) == MVM_RESULT_RUNTIME_ERROR);
  assert(foreign->types[u].variant_type_ids != vm->types[u].variant_type_ids);
  mvm_destroy(foreign);
  mvm_gc_root_unregister(vm, &root);
  mvm_gc_collect(vm);
  assert(!vm->objects && !vm->gc_bytes);
  mvm_destroy(vm);
}

static void test_aggregate_copies_and_fields(void) {
  mvm_t *vm = test_vm_create(NULL, 128, 64);
  assert(vm);
  uint32_t w = primitive(vm, MVM_KIND_SWORD, 4);
  uint32_t str = primitive(vm, MVM_KIND_STRING, 8);
  mvm_type_field_t field = {.kind = MVM_KIND_SWORD, .size = 4, .type_id = w, .has_type_id = true};
  mvm_type_desc_t desc = {.tag = MVM_TYPE_AGGREGATE, .kind = MVM_KIND_VOID,
      .size = 4, .alignment = 4, .field_count = 1, .fields = &field};
  uint32_t point;
  assert(mvm_type_register(vm, &desc, &point) == MVM_RESULT_OK);
  uint32_t u = union_type(vm, (uint32_t[]){point, str}, 2);
  mvm_value_t values[4] = {{.kind = MVM_KIND_NULL}, {.kind = MVM_KIND_NULL},
                            {.kind = MVM_KIND_NULL}, {.kind = MVM_KIND_NULL}};
  mvm_gc_root_t root;
  assert(mvm_gc_root_register(vm, &root, values, 4));
  assert(mvm_typed_alloc(vm, point, &values[0]) == MVM_RESULT_OK);
  assert(mvm_typed_field_set(vm, values[0], 0, word(7)) == MVM_RESULT_OK);
  vm->gc_next_collection = 0;
  assert(mvm_union_new(vm, u, point, values[0], &values[1]) == MVM_RESULT_OK);
  assert(mvm_typed_field_set(vm, values[0], 0, word(9)) == MVM_RESULT_OK);
  vm->gc_next_collection = 0;
  assert(mvm_union_get(vm, values[1], point, &values[2]) == MVM_RESULT_OK);
  mvm_value_t out;
  assert(mvm_typed_field_get(vm, values[2], 0, &out) == MVM_RESULT_OK && out.as.sw == 7);
  assert(mvm_typed_field_set(vm, values[2], 0, word(11)) == MVM_RESULT_OK);
  assert(mvm_union_get(vm, values[1], point, &values[2]) == MVM_RESULT_OK);
  assert(mvm_typed_field_get(vm, values[2], 0, &out) == MVM_RESULT_OK && out.as.sw == 7);
  // A packed field traces its descriptor at an unaligned offset.
  field = (mvm_type_field_t){.offset = 1, .size = 8, .kind = MVM_KIND_UNION,
                             .type_id = u, .has_type_id = true};
  mvm_type_reference_t reference = {.offset = 1, .kind = MVM_KIND_UNION};
  desc = (mvm_type_desc_t){.tag = MVM_TYPE_AGGREGATE, .kind = MVM_KIND_VOID,
      .size = 9, .alignment = 1, .field_count = 1, .fields = &field,
      .reference_count = 1, .references = &reference};
  uint32_t packed;
  assert(mvm_type_register(vm, &desc, &packed) == MVM_RESULT_OK);
  assert(!mvm_type_defaultable(vm, packed));
  out = word(99);
  assert(mvm_typed_alloc(vm, packed, &out) == MVM_RESULT_RUNTIME_ERROR && out.as.sw == 99);
  assert(mvm_typed_construct(vm, packed, &values[3]) == MVM_RESULT_OK);
  assert(mvm_typed_field_get(vm, values[3], 0, &out) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm_typed_field_set(vm, values[3], 0, values[1]) == MVM_RESULT_OK);
  assert(mvm_typed_zero(vm, packed, values[3]) == MVM_RESULT_RUNTIME_ERROR);
  values[0] = values[1] = values[2] = (mvm_value_t){.kind = MVM_KIND_NULL};
  mvm_gc_collect(vm);
  assert(mvm_typed_field_get(vm, values[3], 0, &out) == MVM_RESULT_OK);
  assert(mvm_union_get(vm, out, point, &values[2]) == MVM_RESULT_OK);
  assert(mvm_typed_field_get(vm, values[2], 0, &out) == MVM_RESULT_OK && out.as.sw == 7);
  mvm_gc_root_unregister(vm, &root);
  mvm_gc_collect(vm);
  assert(!vm->objects);
  mvm_destroy(vm);
}

static void test_deeply_nested_values(void) {
  mvm_t *vm = test_vm_create(NULL, 128, 64);
  assert(vm);
  uint32_t w = primitive(vm, MVM_KIND_SWORD, 4);
  uint32_t str = primitive(vm, MVM_KIND_STRING, 8);
  uint32_t current = union_type(vm, (uint32_t[]){w, str}, 2);
  mvm_value_t values[2] = {{.kind = MVM_KIND_NULL}, {.kind = MVM_KIND_NULL}};
  mvm_gc_root_t root;
  assert(mvm_gc_root_register(vm, &root, values, 2));
  assert(mvm_union_new(vm, current, w, word(42), &values[0]) == MVM_RESULT_OK);
  for (size_t i = 0; i < 32; ++i) {
    mvm_type_field_t field = {.size = 8, .kind = MVM_KIND_UNION,
                              .type_id = current, .has_type_id = true};
    mvm_type_reference_t reference = {.kind = MVM_KIND_UNION};
    mvm_type_desc_t desc = {.tag = MVM_TYPE_AGGREGATE, .kind = MVM_KIND_VOID,
        .size = 8, .alignment = 8, .field_count = 1, .fields = &field,
        .reference_count = 1, .references = &reference};
    uint32_t aggregate;
    assert(mvm_type_register(vm, &desc, &aggregate) == MVM_RESULT_OK);
    current = union_type(vm, (uint32_t[]){aggregate, str}, 2);
    vm->gc_next_collection = 0;
    assert(mvm_typed_construct(vm, aggregate, &values[1]) == MVM_RESULT_OK);
    assert(mvm_typed_field_set(vm, values[1], 0, values[0]) == MVM_RESULT_OK);
    vm->gc_next_collection = 0;
    assert(mvm_union_new(vm, current, aggregate, values[1], &values[0]) == MVM_RESULT_OK);
  }
  mvm_value_t descriptor;
  assert(mvm_union_variantof(vm, values[0], &descriptor) == MVM_RESULT_OK);
  assert(descriptor.kind == MVM_KIND_TYPE);
  mvm_gc_root_unregister(vm, &root);
  mvm_gc_collect(vm);
  assert(!vm->objects);
  mvm_destroy(vm);
}

static void test_invalid_metadata_and_operands(void) {
  mvm_t *vm = test_vm_create(NULL, 16, 64);
  assert(vm);
  uint32_t w = primitive(vm, MVM_KIND_SWORD, 4);
  mvm_type_desc_t desc = {.tag = MVM_TYPE_UNION, .kind = MVM_KIND_UNION,
      .size = 8, .alignment = 8, .variant_count = 2,
      .variant_type_ids = (uint32_t[]){w, w}};
  assert(mvm_type_register(vm, &desc, NULL) == MVM_RESULT_VM_ERROR);
  desc.variant_type_ids = (uint32_t[]){w, UINT32_MAX};
  assert(mvm_type_register(vm, &desc, NULL) == MVM_RESULT_VM_ERROR);
  desc.variant_count = SIZE_MAX;
  assert(mvm_type_register(vm, &desc, NULL) == MVM_RESULT_VM_ERROR);
  mvm_value_t out = word(99);
  assert(mvm_union_variantof(vm, (mvm_value_t){.kind = MVM_KIND_UNION}, &out) == MVM_RESULT_RUNTIME_ERROR);
  assert(out.as.sw == 99);
  const mvm_opcodes_t ops[] = {MVM_OP_UNION_NEW, MVM_OP_UNION_GET,
      MVM_OP_UNION_WIDEN, MVM_OP_TYPED_CONSTRUCT, MVM_OP_ARRAY_NEW_TYPED};
  for (size_t i = 0; i < sizeof(ops) / sizeof(*ops); ++i) {
    size_t width = ops[i] == MVM_OP_UNION_NEW ? 8 : 4;
    for (size_t n = 0; n < width; ++n) {
      vm->pc = vm->ram->size - n;
      mvm_instruction_t instruction;
      assert(mvm_decode(vm, NULL, ops[i], &instruction) == MVM_RESULT_RUNTIME_ERROR);
    }
  }
  mvm_destroy(vm);
}

#ifdef UNION_TEST_ALLOCATOR
static void test_allocation_failure(void) {
  mvm_t *vm = test_vm_create(NULL, 16, 64);
  assert(vm);
  uint32_t w = primitive(vm, MVM_KIND_SWORD, 4);
  uint32_t str = primitive(vm, MVM_KIND_STRING, 8);
  uint32_t u = union_type(vm, (uint32_t[]){w, str}, 2);
  mvm_value_t out = word(99);
  array_test_allocation_failures = 2;
  assert(mvm_union_new(vm, u, w, word(42), &out) == MVM_RESULT_VM_ERROR);
  assert(out.kind == MVM_KIND_SWORD && out.as.sw == 99 && !vm->gc_roots);
  array_test_allocation_failures = 1;
  assert(mvm_union_new(vm, u, w, word(42), &out) == MVM_RESULT_OK);
  assert(!array_test_allocation_failures && !vm->gc_roots);
  mvm_destroy(vm);
}
#endif

int main(void) {
  test_values_and_gc();
  test_aggregate_copies_and_fields();
  test_invalid_metadata_and_operands();
  test_deeply_nested_values();
#ifdef UNION_TEST_ALLOCATOR
  test_allocation_failure();
#endif
  return 0;
}
