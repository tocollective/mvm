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

static mvm_t *create_vm(void) {
  mvm_t *vm = test_vm_create(NULL, 64, MVM_FRAME_DEFAULT_LIMIT);
  assert(vm);
  return vm;
}

static void push(mvm_t *vm, mvm_value_t value) {
  assert(mvm_stack_push(vm->stack, value));
}

static mvm_value_t number(uint64_t value) {
  return (mvm_value_t){.kind = MVM_KIND_ULONG, .as.ul = value};
}

static uint32_t register_type(mvm_t *vm, const mvm_type_field_t *fields,
                              size_t field_count,
                              const mvm_type_reference_t *references,
                              size_t reference_count, size_t size,
                              size_t alignment) {
  const mvm_type_desc_t desc = {
      .size = size,
      .alignment = alignment,
      .field_count = field_count,
      .fields = fields,
      .reference_count = reference_count,
      .references = references,
  };
  uint32_t id;
  assert(mvm_type_register(vm, &desc, &id) == MVM_RESULT_OK);
  return id;
}

static uint32_t register_header(mvm_t *vm) {
  const mvm_type_field_t fields[] = {
      {.offset = 0, .size = 1, .kind = MVM_KIND_UBYTE},
      {.offset = 4, .size = 4, .kind = MVM_KIND_UWORD},
      {.offset = 8, .size = 8, .kind = MVM_KIND_REFERENCE},
      {.offset = 16, .size = 2, .kind = MVM_KIND_UHALF},
  };
  const mvm_type_reference_t references[] = {
      {.offset = 8, .kind = MVM_KIND_REFERENCE},
  };
  return register_type(vm, fields, 4, references, 1, 24, 8);
}

static mvm_result_t run_type(mvm_t *vm, mvm_opcodes_t op, uint32_t type_id) {
  vm->ram->data[0] = (uint8_t)op;
  for (size_t i = 0; i < sizeof(type_id); ++i)
    vm->ram->data[1 + i] = (uint8_t)(type_id >> (i * 8));
  vm->pc = 0;
  return mvm_update(vm);
}

static mvm_result_t run_field(mvm_t *vm, mvm_opcodes_t op, uint16_t index) {
  vm->ram->data[0] = (uint8_t)op;
  vm->ram->data[1] = (uint8_t)index;
  vm->ram->data[2] = (uint8_t)(index >> 8);
  vm->pc = 0;
  return mvm_update(vm);
}

static mvm_value_t concat(mvm_t *vm, const char *lhs, const char *rhs) {
  push(vm, (mvm_value_t){.kind = MVM_KIND_STRING, .as.s = (char *)lhs});
  push(vm, (mvm_value_t){.kind = MVM_KIND_STRING, .as.s = (char *)rhs});
  vm->ram->data[0] = MVM_OP_CONCAT;
  vm->pc = 0;
  assert(mvm_update(vm) == MVM_RESULT_OK);
  return vm->stack->data[vm->stack->size - 1];
}

static void test_registration_validation(void) {
  mvm_t *vm = create_vm();
  mvm_type_field_t fields[] = {
      {.offset = 0, .size = 1, .kind = MVM_KIND_UBYTE},
      {.offset = 4, .size = 4, .kind = MVM_KIND_UWORD},
      {.offset = 8, .size = 8, .kind = MVM_KIND_REFERENCE},
      {.offset = 16, .size = 2, .kind = MVM_KIND_UHALF},
  };
  mvm_type_reference_t references[] = {
      {.offset = 8, .kind = MVM_KIND_REFERENCE},
  };
  mvm_type_desc_t desc = {
      .size = 24,
      .alignment = 8,
      .field_count = 4,
      .fields = fields,
      .reference_count = 1,
      .references = references,
  };
  uint32_t id;
  assert(mvm_type_register(vm, &desc, &id) == MVM_RESULT_OK && id == 0);

  fields[1].offset = 0;
  references[0].offset = 0;
  mvm_value_t value;
  assert(mvm_typed_alloc(vm, id, &value) == MVM_RESULT_OK);
  assert(mvm_typed_field_set(vm, value, 1,
                             (mvm_value_t){.kind = MVM_KIND_UWORD,
                                           .as.uw = 0x12345678}) ==
         MVM_RESULT_OK);
  assert(mvm_ram_peek_uword(&(mvm_ram_t){.data = value.as.ptr, .size = 24}, 4) ==
         0x12345678);
  mvm_stack_clear(vm->stack);
  assert(mvm_gc_collect(vm) == 1);

  assert(mvm_type_register(vm, NULL, NULL) == MVM_RESULT_VM_ERROR);
  desc.field_count = 0;
  desc.fields = NULL;
  desc.reference_count = 0;
  desc.references = NULL;
  assert(mvm_type_register(vm, &desc, &id) == MVM_RESULT_OK && id == 1);

  desc.size = 0;
  assert(mvm_type_register(vm, &desc, NULL) == MVM_RESULT_VM_ERROR);
  desc.size = 24;
  desc.alignment = 3;
  assert(mvm_type_register(vm, &desc, NULL) == MVM_RESULT_VM_ERROR);
  desc.alignment = 8;
  desc.field_count = 1;
  assert(mvm_type_register(vm, &desc, NULL) == MVM_RESULT_VM_ERROR);
  desc.fields = fields;
  fields[0] = (mvm_type_field_t){.offset = 23, .size = 4, .kind = MVM_KIND_UWORD};
  assert(mvm_type_register(vm, &desc, NULL) == MVM_RESULT_VM_ERROR);
  fields[0] = (mvm_type_field_t){.offset = 0, .size = 8, .kind = MVM_KIND_UWORD};
  assert(mvm_type_register(vm, &desc, NULL) == MVM_RESULT_VM_ERROR);
  fields[0] = (mvm_type_field_t){.offset = 0, .size = 0, .kind = MVM_KIND_VOID};
  assert(mvm_type_register(vm, &desc, NULL) == MVM_RESULT_VM_ERROR);
  fields[0] = (mvm_type_field_t){.offset = 0, .size = 8, .kind = MVM_KIND_REFERENCE};
  desc.reference_count = 1;
  desc.references = references;
  references[0] = (mvm_type_reference_t){.offset = 0, .kind = MVM_KIND_FUNC};
  assert(mvm_type_register(vm, &desc, NULL) == MVM_RESULT_VM_ERROR);
  references[0].kind = MVM_KIND_REFERENCE;
  assert(mvm_type_register(vm, &desc, &id) == MVM_RESULT_OK && id == 2);
  assert(vm->type_count == 3);
  mvm_destroy(vm);
}

static void test_value_api(void) {
  mvm_t *vm = create_vm();
  const uint32_t header = register_header(vm);
  mvm_value_t a;
  mvm_value_t b;
  assert(mvm_typed_alloc(vm, header, &a) == MVM_RESULT_OK);
  push(vm, a);
  assert(mvm_typed_alloc(vm, header, &b) == MVM_RESULT_OK);
  push(vm, b);
  for (size_t i = 0; i < 24; ++i)
    assert(((unsigned char *)a.as.ptr)[i] == 0);

  mvm_value_t output;
  assert(mvm_typed_field_get(vm, a, 2, &output) == MVM_RESULT_OK);
  assert(output.kind == MVM_KIND_NULL);
  assert(mvm_typed_field_set(vm, a, 0,
                             (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 7}) ==
         MVM_RESULT_OK);
  assert(mvm_typed_field_set(vm, a, 1,
                             (mvm_value_t){.kind = MVM_KIND_UWORD,
                                           .as.uw = 0x12345678}) ==
         MVM_RESULT_OK);
  assert(mvm_typed_field_set(vm, a, 3,
                             (mvm_value_t){.kind = MVM_KIND_UHALF,
                                           .as.uh = 0xABCD}) ==
         MVM_RESULT_OK);
  mvm_value_t raw;
  assert(mvm_alloc(vm, 8, &raw) == MVM_RESULT_OK);
  push(vm, raw);
  assert(mvm_typed_field_set(vm, a, 2, raw) == MVM_RESULT_OK);
  assert(((unsigned char *)a.as.ptr)[0] == 7);
  assert(mvm_ram_peek_uword(&(mvm_ram_t){.data = a.as.ptr, .size = 24}, 4) ==
         0x12345678);
  assert(mvm_ram_peek_uhalf(&(mvm_ram_t){.data = a.as.ptr, .size = 24}, 16) ==
         0xABCD);

  assert(mvm_typed_field_set(vm, a, 0, number(9)) == MVM_RESULT_RUNTIME_ERROR);
  assert(((unsigned char *)a.as.ptr)[0] == 7);
  assert(mvm_typed_field_address(vm, a, 1, &output) == MVM_RESULT_OK);
  assert(output.as.ptr == (unsigned char *)a.as.ptr + 4);
  assert(mvm_typed_copy(vm, header, b, a) == MVM_RESULT_OK);
  assert(!memcmp(a.as.ptr, b.as.ptr, 24));
  assert(mvm_typed_field_set(vm, a, 1,
                             (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 9}) ==
         MVM_RESULT_OK);
  assert(mvm_typed_field_get(vm, b, 1, &output) == MVM_RESULT_OK);
  assert(output.kind == MVM_KIND_UWORD && output.as.uw == 0x12345678);
  assert(mvm_typed_zero(vm, header, a) == MVM_RESULT_OK);
  for (size_t i = 0; i < 24; ++i)
    assert(((unsigned char *)a.as.ptr)[i] == 0);

  output = number(99);
  assert(mvm_typed_field_get(vm, a, 4, &output) == MVM_RESULT_RUNTIME_ERROR);
  assert(output.as.ul == 99);
  assert(mvm_typed_field_get(vm, raw, 0, &output) == MVM_RESULT_RUNTIME_ERROR);
  assert(output.as.ul == 99);
  assert(mvm_typed_alloc(vm, 100, &output) == MVM_RESULT_RUNTIME_ERROR);
  assert(output.as.ul == 99);
  assert(mvm_typed_copy(vm, header, raw, b) == MVM_RESULT_RUNTIME_ERROR);
  mvm_stack_clear(vm->stack);
  assert(mvm_gc_collect(vm) == 3);
  mvm_destroy(vm);
}

static void test_packed_and_gc(void) {
  mvm_t *vm = create_vm();
  const mvm_type_field_t packed_fields[] = {
      {.offset = 0, .size = 1, .kind = MVM_KIND_UBYTE},
      {.offset = 1, .size = 4, .kind = MVM_KIND_UWORD},
  };
  const uint32_t packed =
      register_type(vm, packed_fields, 2, NULL, 0, 5, 1);
  mvm_value_t packed_value;
  assert(mvm_typed_alloc(vm, packed, &packed_value) == MVM_RESULT_OK);
  push(vm, packed_value);
  assert(mvm_typed_field_set(vm, packed_value, 1,
                             (mvm_value_t){.kind = MVM_KIND_UWORD,
                                           .as.uw = 0x89ABCDEF}) ==
         MVM_RESULT_OK);
  mvm_value_t output;
  assert(mvm_typed_field_get(vm, packed_value, 1, &output) == MVM_RESULT_OK);
  assert(output.kind == MVM_KIND_UWORD && output.as.uw == 0x89ABCDEF);
  mvm_stack_clear(vm->stack);
  assert(mvm_gc_collect(vm) == 1);

  const mvm_type_field_t fields[] = {
      {.offset = 0, .size = 8, .kind = MVM_KIND_REFERENCE},
      {.offset = 8, .size = 8, .kind = MVM_KIND_STRING},
  };
  const mvm_type_reference_t references[] = {
      {.offset = 0, .kind = MVM_KIND_REFERENCE},
      {.offset = 8, .kind = MVM_KIND_STRING},
  };
  const uint32_t node = register_type(vm, fields, 2, references, 2, 16, 1);
  mvm_value_t parent;
  mvm_value_t child;
  assert(mvm_typed_alloc(vm, node, &parent) == MVM_RESULT_OK);
  push(vm, parent);
  assert(mvm_typed_alloc(vm, node, &child) == MVM_RESULT_OK);
  push(vm, child);
  const mvm_value_t text = concat(vm, "typed", " string");
  assert(mvm_typed_field_set(vm, parent, 0, child) == MVM_RESULT_OK);
  assert(mvm_typed_field_set(vm, child, 0, parent) == MVM_RESULT_OK);
  assert(mvm_typed_field_set(vm, parent, 1, text) == MVM_RESULT_OK);
  assert(mvm_stack_truncate(vm->stack, 1));
  vm->gc_next_collection = 0;
  const size_t collections = vm->gc_collections;
  mvm_value_t garbage;
  assert(mvm_typed_alloc(vm, node, &garbage) == MVM_RESULT_OK);
  assert(vm->gc_collections == collections + 1);
  assert(!strcmp(text.as.s, "typed string"));
  assert(mvm_typed_field_set(vm, parent, 1,
                             (mvm_value_t){.kind = MVM_KIND_NULL}) ==
         MVM_RESULT_OK);
  assert(mvm_gc_collect(vm) == 2);

  assert(mvm_typed_field_address(vm, parent, 1, &output) == MVM_RESULT_OK);
  vm->stack->data[0] = output;
  assert(mvm_gc_collect(vm) == 0);
  mvm_stack_clear(vm->stack);
  assert(mvm_gc_collect(vm) == 2);
  assert(!vm->objects && !vm->gc_bytes);
  mvm_destroy(vm);
}

static void test_opcodes(void) {
  mvm_t *vm = create_vm();
  const mvm_type_field_t fields[] = {
      {.offset = 0, .size = 8, .kind = MVM_KIND_ULONG},
      {.offset = 8, .size = 8, .kind = MVM_KIND_ULONG},
  };
  const uint32_t pair = register_type(vm, fields, 2, NULL, 0, 16, 8);
  assert(run_type(vm, MVM_OP_TYPED_ALLOC, pair) == MVM_RESULT_OK);
  assert(vm->pc == 5 && vm->stack->size == 1);
  const mvm_value_t a = vm->stack->data[0];
  assert(run_type(vm, MVM_OP_TYPED_ALLOC, pair) == MVM_RESULT_OK);
  const mvm_value_t b = vm->stack->data[1];
  mvm_stack_clear(vm->stack);

  push(vm, a);
  push(vm, number(42));
  assert(run_field(vm, MVM_OP_FIELD_SET, 1) == MVM_RESULT_OK);
  assert(vm->pc == 3 && vm->stack->size == 0);
  push(vm, a);
  assert(run_field(vm, MVM_OP_FIELD_GET, 1) == MVM_RESULT_OK);
  assert(vm->stack->data[0].as.ul == 42);
  mvm_stack_clear(vm->stack);

  push(vm, b);
  push(vm, a);
  assert(run_type(vm, MVM_OP_VALUE_COPY, pair) == MVM_RESULT_OK);
  assert(vm->pc == 5 && vm->stack->size == 0);
  push(vm, b);
  assert(run_field(vm, MVM_OP_FIELD_GET, 1) == MVM_RESULT_OK);
  assert(vm->stack->data[0].as.ul == 42);
  mvm_stack_clear(vm->stack);

  push(vm, b);
  assert(run_type(vm, MVM_OP_VALUE_ZERO, pair) == MVM_RESULT_OK);
  push(vm, b);
  assert(run_field(vm, MVM_OP_FIELD_GET, 1) == MVM_RESULT_OK);
  assert(vm->stack->data[0].as.ul == 0);
  mvm_stack_clear(vm->stack);

  push(vm, a);
  assert(run_field(vm, MVM_OP_FIELD_ADDR, 1) == MVM_RESULT_OK);
  assert(vm->stack->data[0].as.ptr == (unsigned char *)a.as.ptr + 8);
  mvm_stack_clear(vm->stack);

  const mvm_opcodes_t field_ops[] = {
      MVM_OP_FIELD_ADDR,
  };
  for (size_t i = 0; i < sizeof(field_ops) / sizeof(*field_ops); ++i) {
    for (size_t available = 0; available < 2; ++available) {
      mvm_t *small = test_vm_create(NULL, 1 + available, 1);
      assert(small);
      small->ram->data[0] = (uint8_t)field_ops[i];
      assert(mvm_update(small) == MVM_RESULT_RUNTIME_ERROR);
      assert(small->pc == 1);
      mvm_destroy(small);
    }
  }
  const mvm_opcodes_t type_ops[] = {
      MVM_OP_TYPED_ALLOC,
      MVM_OP_VALUE_COPY,
      MVM_OP_VALUE_ZERO,
  };
  for (size_t i = 0; i < sizeof(type_ops) / sizeof(*type_ops); ++i) {
    for (size_t available = 0; available < 4; ++available) {
      mvm_t *small = test_vm_create(NULL, 1 + available, 1);
      assert(small);
      small->ram->data[0] = (uint8_t)type_ops[i];
      assert(mvm_update(small) == MVM_RESULT_RUNTIME_ERROR);
      assert(small->pc == 1);
      mvm_destroy(small);
    }
    assert(run_type(vm, type_ops[i], 100) == MVM_RESULT_RUNTIME_ERROR);
    assert(vm->pc == 1 && vm->stack->size == 0);
  }

  push(vm, number(1));
  push(vm, a);
  assert(run_type(vm, MVM_OP_VALUE_COPY, pair) == MVM_RESULT_RUNTIME_ERROR);
  assert(vm->pc == 1 && vm->stack->size == 2);
  mvm_stack_clear(vm->stack);
  assert(mvm_gc_collect(vm) == 2);
  mvm_destroy(vm);
}

static void test_handle_fields(void) {
  mvm_t *vm = create_vm();
  const mvm_type_field_t fields[] = {
      {.offset = 0, .size = 8, .kind = MVM_KIND_STRING},
      {.offset = 8, .size = 8, .kind = MVM_KIND_ARRAY},
      {.offset = 16, .size = 8, .kind = MVM_KIND_FUNC},
  };
  const mvm_type_reference_t references[] = {
      {.offset = 0, .kind = MVM_KIND_STRING},
      {.offset = 8, .kind = MVM_KIND_ARRAY},
  };
  const uint32_t handles = register_type(vm, fields, 3, references, 2, 24, 8);
  mvm_value_t object;
  assert(mvm_typed_alloc(vm, handles, &object) == MVM_RESULT_OK);
  push(vm, object);
  mvm_value_t initial;
  assert(mvm_typed_field_get(vm, object, 2, &initial) == MVM_RESULT_OK);
  assert(initial.kind == MVM_KIND_NULL);
  mvm_value_t array;
  assert(mvm_array_new(vm, MVM_KIND_ULONG, 1, &array) == MVM_RESULT_OK);
  push(vm, array);
  const mvm_value_t text = concat(vm, "handle", " field");
  assert(mvm_typed_field_set(vm, object, 0, text) == MVM_RESULT_OK);
  assert(mvm_typed_field_set(vm, object, 1, array) == MVM_RESULT_OK);
  const mvm_value_t function = {.kind = MVM_KIND_FUNC, .as.func = 42};
  assert(mvm_typed_field_set(vm, object, 2, function) == MVM_RESULT_OK);
  assert(mvm_stack_truncate(vm->stack, 1));
  assert(mvm_gc_collect(vm) == 0);

  mvm_value_t output;
  assert(mvm_typed_field_get(vm, object, 0, &output) == MVM_RESULT_OK);
  assert(output.kind == MVM_KIND_STRING && !strcmp(output.as.s, "handle field"));
  assert(mvm_typed_field_get(vm, object, 1, &output) == MVM_RESULT_OK);
  assert(output.kind == MVM_KIND_ARRAY && output.as.array == array.as.array);
  assert(mvm_typed_field_get(vm, object, 2, &output) == MVM_RESULT_OK);
  assert(output.kind == MVM_KIND_FUNC && output.as.func == 42);
  const mvm_value_t invalid_array = {.kind = MVM_KIND_ARRAY, .as.array = (void *)vm};
  assert(mvm_typed_field_set(vm, object, 1, invalid_array) ==
         MVM_RESULT_RUNTIME_ERROR);

  const mvm_value_t null_value = {.kind = MVM_KIND_NULL};
  assert(mvm_typed_field_set(vm, object, 0, null_value) == MVM_RESULT_OK);
  assert(mvm_typed_field_set(vm, object, 1, null_value) == MVM_RESULT_OK);
  assert(mvm_typed_field_set(vm, object, 2, null_value) == MVM_RESULT_OK);
  assert(mvm_typed_field_get(vm, object, 2, &output) == MVM_RESULT_OK);
  assert(output.kind == MVM_KIND_NULL);
  assert(mvm_gc_collect(vm) == 2);
  mvm_stack_clear(vm->stack);
  assert(mvm_gc_collect(vm) == 1);
  mvm_destroy(vm);
}

static void test_nested_aggregate(void) {
  mvm_t *vm = create_vm();
  const mvm_type_field_t inner_fields[] = {
      {.offset = 0, .size = 4, .kind = MVM_KIND_UWORD},
  };
  const uint32_t inner = register_type(vm, inner_fields, 1, NULL, 0, 4, 4);
  const mvm_type_field_t outer_fields[] = {
      {.offset = 0,
       .size = 4,
       .kind = MVM_KIND_VOID,
       .type_id = inner,
       .has_type_id = true},
  };
  const uint32_t outer = register_type(vm, outer_fields, 1, NULL, 0, 4, 4);
  mvm_value_t object;
  assert(mvm_typed_alloc(vm, outer, &object) == MVM_RESULT_OK);
  mvm_value_t nested;
  assert(mvm_typed_field_get(vm, object, 0, &nested) == MVM_RESULT_OK);
  assert(mvm_typed_field_set(vm, nested, 0,
                             (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 47}) ==
         MVM_RESULT_OK);
  mvm_value_t output;
  assert(mvm_typed_field_get(vm, nested, 0, &output) == MVM_RESULT_OK);
  assert(output.kind == MVM_KIND_UWORD && output.as.uw == 47);

  push(vm, object);
  assert(run_field(vm, MVM_OP_FIELD_GET, 0) == MVM_RESULT_OK);
  assert(run_field(vm, MVM_OP_FIELD_GET, 0) == MVM_RESULT_OK);
  assert(vm->stack->data[0].kind == MVM_KIND_UWORD &&
         vm->stack->data[0].as.uw == 47);
  mvm_destroy(vm);
}

int main(void) {
  test_registration_validation();
  test_value_api();
  test_packed_and_gc();
  test_opcodes();
  test_handle_fields();
  test_nested_aggregate();
  return 0;
}
