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
#include <string.h>

typedef struct Code {
  uint8_t data[512];
  size_t size;
} Code;

static void code_op(Code *code, mvm_opcodes_t op) {
  assert(code->size < sizeof(code->data));
  code->data[code->size++] = (uint8_t)op;
}

static void code_u8(Code *code, uint8_t value) {
  assert(code->size < sizeof(code->data));
  code->data[code->size++] = value;
}

static size_t code_u32(Code *code, uint32_t value) {
  assert(code->size + 4 <= sizeof(code->data));
  const size_t offset = code->size;
  for (size_t i = 0; i < 4; ++i)
    code->data[code->size++] = (uint8_t)(value >> (i * 8));
  return offset;
}

static void code_type(Code *code, mvm_opcodes_t op, uint32_t type_id) {
  code_op(code, op);
  code_u32(code, type_id);
}

static void code_sword(Code *code, int32_t value) {
  code_op(code, MVM_OP_LIT_SWORD);
  code_u32(code, (uint32_t)value);
}

static void code_uword(Code *code, uint32_t value) {
  code_op(code, MVM_OP_LIT_UWORD);
  code_u32(code, value);
}

static size_t code_call(Code *code, mvm_opcodes_t op, uint8_t argc) {
  code_op(code, op);
  const size_t address = code_u32(code, 0);
  code_u8(code, argc);
  return address;
}

static void code_pack(Code *code, size_t count, const uint32_t *type_ids) {
  code_op(code, MVM_OP_VA_PACK);
  code_u32(code, (uint32_t)count);
  for (size_t i = 0; i < count; ++i)
    code_u32(code, type_ids[i]);
}

static mvm_t *create_vm(const Code *code) {
  const size_t ram_size = code->size < 5 ? 5 : code->size;
  mvm_t *mvm = test_vm_create(NULL, ram_size, MVM_FRAME_DEFAULT_LIMIT);
  assert(mvm);
  memcpy(mvm->ram->data, code->data, code->size);
  return mvm;
}

static void run(mvm_t *mvm) {
  for (size_t steps = 0; mvm->working; ++steps) {
    assert(steps < 1000);
    assert(mvm_update(mvm) == MVM_RESULT_OK);
  }
}

static void run_error(mvm_t *mvm) {
  for (size_t steps = 0; mvm->working; ++steps) {
    assert(steps < 1000);
    const mvm_result_t status = mvm_update(mvm);
    if (status != MVM_RESULT_OK) {
      assert(status == MVM_RESULT_RUNTIME_ERROR);
      return;
    }
  }
  assert(false);
}

static uint32_t register_type(mvm_t *mvm, const mvm_type_desc_t *desc) {
  uint32_t id;
  assert(mvm_type_register(mvm, desc, &id) == MVM_RESULT_OK);
  return id;
}

static uint32_t register_primitive(mvm_t *mvm, mvm_kind_t kind) {
  size_t size = 0;
  size_t alignment = 1;
  switch (kind) {
  case MVM_KIND_BOOL:
  case MVM_KIND_UBYTE:
  case MVM_KIND_SBYTE: size = 1; break;
  case MVM_KIND_UHALF:
  case MVM_KIND_SHALF: size = alignment = 2; break;
  case MVM_KIND_UWORD:
  case MVM_KIND_SWORD:
  case MVM_KIND_FLOAT: size = alignment = 4; break;
  case MVM_KIND_ULONG:
  case MVM_KIND_SLONG:
  case MVM_KIND_DOUBLE:
  case MVM_KIND_STRING:
  case MVM_KIND_TYPE:
  case MVM_KIND_ARRAY:
  case MVM_KIND_REFERENCE:
  case MVM_KIND_FUNC:
  case MVM_KIND_THREAD: size = alignment = 8; break;
  case MVM_KIND_VOID: break;
  default: assert(false);
  }
  const bool nullable = kind == MVM_KIND_STRING || kind == MVM_KIND_ARRAY ||
                        kind == MVM_KIND_REFERENCE || kind == MVM_KIND_FUNC ||
                        kind == MVM_KIND_THREAD;
  const mvm_type_desc_t desc = {
      .size = size,
      .alignment = alignment,
      .tag = MVM_TYPE_PRIMITIVE,
      .kind = kind,
      .flags = nullable ? MVM_TYPE_FLAG_NULLABLE : 0,
  };
  return register_type(mvm, &desc);
}

static uint32_t register_reference(mvm_t *mvm, uint32_t pointee, bool mutable) {
  const mvm_type_desc_t desc = {
      .size = 8,
      .alignment = 8,
      .tag = MVM_TYPE_REFERENCE,
      .kind = MVM_KIND_REFERENCE,
      .inner_type_id = pointee,
      .flags = mutable ? MVM_TYPE_FLAG_MUTABLE : 0,
  };
  return register_type(mvm, &desc);
}

static uint32_t register_array(mvm_t *mvm, uint32_t element, bool mutable) {
  const mvm_type_desc_t desc = {
      .size = 8,
      .alignment = 8,
      .tag = MVM_TYPE_ARRAY,
      .kind = MVM_KIND_ARRAY,
      .inner_type_id = element,
      .flags = mutable ? MVM_TYPE_FLAG_MUTABLE : 0,
  };
  return register_type(mvm, &desc);
}

static uint32_t register_function(mvm_t *mvm, uint32_t word, bool variadic) {
  const uint32_t params[] = {word};
  const mvm_type_desc_t desc = {
      .size = 8,
      .alignment = 8,
      .tag = MVM_TYPE_FUNCTION,
      .kind = MVM_KIND_FUNC,
      .flags = variadic ? MVM_TYPE_FLAG_VARIADIC : 0,
      .argc = 1,
      .param_type_ids = params,
      .result_type_id = word,
  };
  return register_type(mvm, &desc);
}

static mvm_result_t run_type(mvm_t *mvm, mvm_opcodes_t op, uint32_t type_id) {
  mvm->ram->data[0] = (uint8_t)op;
  for (size_t i = 0; i < 4; ++i)
    mvm->ram->data[1 + i] = (uint8_t)(type_id >> (i * 8));
  mvm->pc = 0;
  return mvm_update(mvm);
}

static void push(mvm_t *mvm, mvm_value_t value) {
  assert(mvm_stack_push(mvm->stack, value));
}

static void test_full_type(void) {
  Code code = {0};
  code_type(&code, MVM_OP_LIT_TYPE_ID, 0);
  code_op(&code, MVM_OP_RETURN);
  mvm_t *mvm = create_vm(&code);
  const uint32_t word = register_primitive(mvm, MVM_KIND_SWORD);
  assert(word == 0);
  run(mvm);
  assert(mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_TYPE);
  assert(mvm->stack->data[0].has_full_type);
  assert(mvm->stack->data[0].as.type == word);
  mvm_destroy(mvm);

  code = (Code){0};
  code_type(&code, MVM_OP_LIT_TYPE_ID, 0);
  code_op(&code, MVM_OP_LIT_TYPE);
  code_u8(&code, MVM_KIND_VOID);
  code_op(&code, MVM_OP_EQ);
  code_op(&code, MVM_OP_RETURN);
  mvm = create_vm(&code);
  assert(register_primitive(mvm, MVM_KIND_SWORD) == 0);
  run(mvm);
  assert(mvm->stack->data[0].kind == MVM_KIND_BOOL);
  assert(!mvm->stack->data[0].as.b);
  mvm_destroy(mvm);

  code = (Code){0};
  code_type(&code, MVM_OP_LIT_TYPE_ID, 99);
  mvm = create_vm(&code);
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->pc == 1 && mvm->stack->size == 0);
  mvm_destroy(mvm);
}

static void test_scalar_references(void) {
  Code code = {0};
  code_op(&code, MVM_OP_RETURN);
  mvm_t *mvm = create_vm(&code);
  const uint32_t uword = register_primitive(mvm, MVM_KIND_UWORD);
  const uint32_t mutable_uword = register_reference(mvm, uword, true);
  const uint32_t readonly_uword = register_reference(mvm, uword, false);
  const uint32_t ulong = register_primitive(mvm, MVM_KIND_ULONG);
  const uint32_t mutable_ulong = register_reference(mvm, ulong, true);

  push(mvm, (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 41});
  assert(run_type(mvm, MVM_OP_TYPED_INIT, uword) == MVM_RESULT_OK);
  assert(mvm->stack->size == 1);
  mvm_value_t reference = mvm->stack->data[0];
  assert(reference.kind == MVM_KIND_REFERENCE && reference.has_full_type);
  assert(reference.type_id == mutable_uword);

  push(mvm, (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 20});
  assert(run_type(mvm, MVM_OP_REF_WRITE, mutable_uword) == MVM_RESULT_OK);
  assert(mvm->stack->size == 0);

  push(mvm, reference);
  assert(run_type(mvm, MVM_OP_REF_READ, readonly_uword) == MVM_RESULT_OK);
  assert(mvm->stack->data[0].kind == MVM_KIND_UWORD);
  assert(mvm->stack->data[0].as.uw == 20);
  assert(mvm->stack->data[0].type_id == uword);
  assert(mvm->stack->data[0].has_full_type);
  mvm_stack_clear(mvm->stack);

  mvm_value_t readonly = reference;
  readonly.type_id = readonly_uword;
  push(mvm, readonly);
  push(mvm, (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 21});
  assert(run_type(mvm, MVM_OP_REF_WRITE, mutable_uword) ==
         MVM_RESULT_RUNTIME_ERROR);
  mvm_stack_clear(mvm->stack);

  push(mvm, reference);
  assert(run_type(mvm, MVM_OP_REF_READ, mutable_ulong) ==
         MVM_RESULT_RUNTIME_ERROR);
  mvm_stack_clear(mvm->stack);

  push(mvm, (mvm_value_t){.kind = MVM_KIND_NULL});
  assert(run_type(mvm, MVM_OP_REF_READ, mutable_uword) ==
         MVM_RESULT_RUNTIME_ERROR);
  mvm_stack_clear(mvm->stack);

  assert(run_type(mvm, MVM_OP_REF_WRITE, readonly_uword) ==
         MVM_RESULT_RUNTIME_ERROR);
  mvm_destroy(mvm);
}

static void test_struct_initialization(void) {
  Code code = {0};
  code_op(&code, MVM_OP_RETURN);
  mvm_t *mvm = create_vm(&code);
  const uint32_t string = register_primitive(mvm, MVM_KIND_STRING);
  const mvm_type_field_t fields[] = {
      {.offset = 0, .size = 8, .kind = MVM_KIND_STRING,
       .type_id = string, .has_type_id = true},
  };
  const mvm_type_reference_t references[] = {
      {.offset = 0, .kind = MVM_KIND_STRING},
  };
  const mvm_type_desc_t desc = {
      .size = 8,
      .alignment = 8,
      .field_count = 1,
      .fields = fields,
      .reference_count = 1,
      .references = references,
  };
  const uint32_t aggregate = register_type(mvm, &desc);
  const uint32_t mutable_aggregate = register_reference(mvm, aggregate, true);

  mvm_value_t source;
  assert(mvm_typed_alloc(mvm, aggregate, &source) == MVM_RESULT_OK);
  const mvm_value_t text = {.kind = MVM_KIND_STRING, .as.s = "shared"};
  assert(mvm_typed_field_set(mvm, source, 0, text) == MVM_RESULT_OK);
  source.type_id = aggregate;
  source.has_full_type = true;
  push(mvm, source);
  assert(run_type(mvm, MVM_OP_TYPED_INIT, aggregate) == MVM_RESULT_OK);
  mvm_value_t copy = mvm->stack->data[0];
  assert(copy.as.ptr != source.as.ptr);

  assert(run_type(mvm, MVM_OP_REF_READ, mutable_aggregate) == MVM_RESULT_OK);
  mvm_value_t output;
  assert(mvm_typed_field_get(mvm, mvm->stack->data[0], 0, &output) ==
         MVM_RESULT_OK);
  assert(output.kind == MVM_KIND_STRING && output.as.s == text.as.s);
  mvm_destroy(mvm);
}

static void test_variadic_sum(void) {
  const uint32_t types[] = {0, 0};
  Code code = {0};
  code_sword(&code, 20);
  code_sword(&code, 30);
  code_pack(&code, 2, types);
  const size_t target = code_call(&code, MVM_OP_CALL, 1);
  code_op(&code, MVM_OP_RETURN);
  const size_t callee = code.size;
  code_op(&code, MVM_OP_VA_COUNT);
  code_op(&code, MVM_OP_POP);
  code_uword(&code, 0);
  code_type(&code, MVM_OP_VA_ARG, 0);
  code_uword(&code, 1);
  code_type(&code, MVM_OP_VA_ARG, 0);
  code_op(&code, MVM_OP_ADD);
  code_op(&code, MVM_OP_RETURN);
  for (size_t i = 0; i < 4; ++i)
    code.data[target + i] = (uint8_t)(callee >> (i * 8));

  mvm_t *mvm = create_vm(&code);
  assert(register_primitive(mvm, MVM_KIND_SWORD) == 0);
  run(mvm);
  assert(mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_SWORD);
  assert(mvm->stack->data[0].as.sw == 50);
  mvm_destroy(mvm);
}

static void test_empty_and_forwarded_packets(void) {
  Code code = {0};
  code_pack(&code, 0, NULL);
  const size_t target = code_call(&code, MVM_OP_CALL, 1);
  code_op(&code, MVM_OP_RETURN);
  const size_t callee = code.size;
  code_op(&code, MVM_OP_VA_COUNT);
  code_op(&code, MVM_OP_RETURN);
  for (size_t i = 0; i < 4; ++i)
    code.data[target + i] = (uint8_t)(callee >> (i * 8));
  mvm_t *mvm = create_vm(&code);
  run(mvm);
  assert(mvm->stack->data[0].kind == MVM_KIND_ULONG);
  assert(mvm->stack->data[0].as.ul == 0);
  mvm_destroy(mvm);

  const uint32_t types[] = {0, 0};
  code = (Code){0};
  code_sword(&code, 7);
  code_sword(&code, 9);
  code_pack(&code, 2, types);
  const size_t wrapper_target = code_call(&code, MVM_OP_CALL, 1);
  code_op(&code, MVM_OP_RETURN);
  const size_t wrapper = code.size;
  code_op(&code, MVM_OP_VA_FORWARD);
  const size_t reader_target = code_call(&code, MVM_OP_TAIL_CALL, 1);
  const size_t reader = code.size;
  code_op(&code, MVM_OP_VA_COUNT);
  code_op(&code, MVM_OP_RETURN);
  for (size_t i = 0; i < 4; ++i) {
    code.data[wrapper_target + i] = (uint8_t)(wrapper >> (i * 8));
    code.data[reader_target + i] = (uint8_t)(reader >> (i * 8));
  }
  mvm = create_vm(&code);
  assert(register_primitive(mvm, MVM_KIND_SWORD) == 0);
  run(mvm);
  assert(mvm->stack->data[0].as.ul == 2);
  mvm_destroy(mvm);
}

static void test_va_arg_narrowing_and_errors(void) {
  const uint32_t types[] = {0};
  Code code = {0};
  code_sword(&code, 0x123);
  code_pack(&code, 1, types);
  size_t target = code_call(&code, MVM_OP_CALL, 1);
  code_op(&code, MVM_OP_RETURN);
  const size_t callee = code.size;
  code_uword(&code, 0);
  code_type(&code, MVM_OP_VA_ARG, 1);
  code_op(&code, MVM_OP_RETURN);
  for (size_t i = 0; i < 4; ++i)
    code.data[target + i] = (uint8_t)(callee >> (i * 8));
  mvm_t *mvm = create_vm(&code);
  assert(register_primitive(mvm, MVM_KIND_SWORD) == 0);
  assert(register_primitive(mvm, MVM_KIND_UBYTE) == 1);
  run(mvm);
  assert(mvm->stack->data[0].kind == MVM_KIND_UBYTE);
  assert(mvm->stack->data[0].as.ub == 0x23);
  mvm_destroy(mvm);

  code = (Code){0};
  code_sword(&code, 1);
  code_pack(&code, 1, types);
  target = code_call(&code, MVM_OP_CALL, 1);
  const size_t callee_index = code.size;
  code_uword(&code, 1);
  code_type(&code, MVM_OP_VA_ARG, 0);
  code_op(&code, MVM_OP_RETURN);
  for (size_t i = 0; i < 4; ++i)
    code.data[target + i] = (uint8_t)(callee_index >> (i * 8));
  mvm = create_vm(&code);
  assert(register_primitive(mvm, MVM_KIND_SWORD) == 0);
  for (size_t i = 0; i < 3; ++i)
    assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  mvm_destroy(mvm);

  code = (Code){0};
  code_op(&code, MVM_OP_VA_FORWARD);
  mvm = create_vm(&code);
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  mvm_destroy(mvm);

  code = (Code){0};
  code_pack(&code, 0, NULL);
  code_op(&code, MVM_OP_RETURN);
  mvm = create_vm(&code);
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  mvm_destroy(mvm);
}

static uint32_t native_string_type;
static bool native_adapter_checked;

static mvm_result_t native_adapter(mvm_t *mvm, const mvm_value_t *args,
                                   mvm_value_t *result) {
  size_t count = 0;
  assert(mvm_variadic_count(mvm, args[0], &count) == MVM_RESULT_OK);
  assert(count == 1);
  mvm_value_t value;
  assert(mvm_variadic_arg(mvm, args[0], 0, native_string_type, &value) ==
         MVM_RESULT_OK);
  assert(!strcmp(value.as.s, "gc-root"));
  mvm->gc_next_collection = 0;
  mvm_value_t garbage;
  assert(mvm_alloc(mvm, 32, &garbage) == MVM_RESULT_OK);
  assert(mvm_variadic_arg(mvm, args[0], 0, native_string_type, &value) ==
         MVM_RESULT_OK);
  assert(!strcmp(value.as.s, "gc-root"));
  *result = value;
  native_adapter_checked = true;
  return MVM_RESULT_OK;
}

static void test_variadic_native_adapter(void) {
  const uint32_t types[] = {0};
  Code code = {0};
  code_op(&code, MVM_OP_LIT_STRING);
  const size_t lhs = code_u32(&code, 200);
  code_op(&code, MVM_OP_LIT_STRING);
  const size_t rhs = code_u32(&code, 204);
  code_op(&code, MVM_OP_CONCAT);
  code_pack(&code, 1, types);
  code_op(&code, MVM_OP_NATIVE);
  code_u8(&code, 0);
  code_u8(&code, 0);
  code_u8(&code, 1);
  code_op(&code, MVM_OP_RETURN);
  for (size_t i = 0; i < 4; ++i) {
    code.data[lhs + i] = (uint8_t)(200 >> (i * 8));
    code.data[rhs + i] = (uint8_t)(204 >> (i * 8));
  }
  memcpy(code.data + 200, "gc-", 4);
  memcpy(code.data + 204, "root", 5);

  mvm_t *mvm = test_vm_create(NULL, 256, MVM_FRAME_DEFAULT_LIMIT);
  assert(mvm);
  memcpy(mvm->ram->data, code.data, 256);
  native_string_type = register_primitive(mvm, MVM_KIND_STRING);
  assert(native_string_type == 0);
  assert(mvm_register_builtin_variadic(mvm, "adapter", native_adapter,
                                       MVM_KIND_STRING, 0, NULL, NULL) ==
         MVM_RESULT_OK);
  native_adapter_checked = false;
  run(mvm);
  assert(native_adapter_checked);
  assert(mvm->stack->data[0].kind == MVM_KIND_STRING);
  assert(!strcmp(mvm->stack->data[0].as.s, "gc-root"));
  mvm_destroy(mvm);
}

static void test_indirect_variadic_sum(void) {
  const uint32_t types[] = {0, 0};
  Code code = {0};
  code_sword(&code, 4);
  code_sword(&code, 5);
  code_pack(&code, 2, types);
  code_op(&code, MVM_OP_LIT_FUNC);
  const size_t function = code_u32(&code, 0);
  code_op(&code, MVM_OP_CALL_INDIRECT);
  code_u8(&code, 1);
  code_op(&code, MVM_OP_RETURN);
  const size_t callee = code.size;
  code_uword(&code, 0);
  code_type(&code, MVM_OP_VA_ARG, 0);
  code_uword(&code, 1);
  code_type(&code, MVM_OP_VA_ARG, 0);
  code_op(&code, MVM_OP_ADD);
  code_op(&code, MVM_OP_RETURN);
  for (size_t i = 0; i < 4; ++i)
    code.data[function + i] = (uint8_t)(callee >> (i * 8));

  mvm_t *mvm = create_vm(&code);
  assert(register_primitive(mvm, MVM_KIND_SWORD) == 0);
  run(mvm);
  assert(mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_SWORD);
  assert(mvm->stack->data[0].as.sw == 9);
  mvm_destroy(mvm);
}

static void test_variadic_full_types(void) {
  const uint32_t function_types[] = {1};
  Code code = {0};
  code_op(&code, MVM_OP_LIT_FUNC);
  const size_t function = code_u32(&code, 0);
  code_pack(&code, 1, function_types);
  const size_t target = code_call(&code, MVM_OP_CALL, 1);
  code_op(&code, MVM_OP_RETURN);
  const size_t callee = code.size;
  code_uword(&code, 0);
  code_type(&code, MVM_OP_VA_ARG, 1);
  code_op(&code, MVM_OP_RETURN);
  for (size_t i = 0; i < 4; ++i) {
    code.data[function + i] = (uint8_t)(callee >> (i * 8));
    code.data[target + i] = (uint8_t)(callee >> (i * 8));
  }
  mvm_t *mvm = create_vm(&code);
  const uint32_t word = register_primitive(mvm, MVM_KIND_UWORD);
  const uint32_t function_type = register_function(mvm, word, false);
  assert(word == 0 && function_type == 1);
  run(mvm);
  assert(mvm->stack->data[0].kind == MVM_KIND_FUNC);
  assert(mvm->stack->data[0].as.func == callee);
  assert(mvm->stack->data[0].has_full_type);
  assert(mvm->stack->data[0].type_id == function_type);
  mvm_destroy(mvm);

  const uint32_t type_types[] = {1};
  code = (Code){0};
  code_type(&code, MVM_OP_LIT_TYPE_ID, 0);
  code_pack(&code, 1, type_types);
  const size_t type_target = code_call(&code, MVM_OP_CALL, 1);
  code_op(&code, MVM_OP_RETURN);
  const size_t type_reader = code.size;
  code_uword(&code, 0);
  code_type(&code, MVM_OP_VA_ARG, 1);
  code_op(&code, MVM_OP_RETURN);
  for (size_t i = 0; i < 4; ++i)
    code.data[type_target + i] = (uint8_t)(type_reader >> (i * 8));
  mvm = create_vm(&code);
  assert(register_primitive(mvm, MVM_KIND_SWORD) == 0);
  assert(register_primitive(mvm, MVM_KIND_TYPE) == 1);
  run(mvm);
  assert(mvm->stack->data[0].kind == MVM_KIND_TYPE);
  assert(mvm->stack->data[0].as.type == 0);
  assert(mvm->stack->data[0].type_id == 1);
  mvm_destroy(mvm);

  const uint32_t reference_types[] = {1};
  code = (Code){0};
  code_uword(&code, 41);
  code_type(&code, MVM_OP_TYPED_INIT, 0);
  code_pack(&code, 1, reference_types);
  const size_t reference_target = code_call(&code, MVM_OP_CALL, 1);
  code_op(&code, MVM_OP_RETURN);
  const size_t reference_reader = code.size;
  code_uword(&code, 0);
  code_type(&code, MVM_OP_VA_ARG, 2);
  code_type(&code, MVM_OP_REF_READ, 2);
  code_op(&code, MVM_OP_RETURN);
  for (size_t i = 0; i < 4; ++i)
    code.data[reference_target + i] =
        (uint8_t)(reference_reader >> (i * 8));
  mvm = create_vm(&code);
  const uint32_t uword = register_primitive(mvm, MVM_KIND_UWORD);
  assert(register_reference(mvm, uword, true) == 1);
  assert(register_reference(mvm, uword, false) == 2);
  run(mvm);
  assert(mvm->stack->data[0].kind == MVM_KIND_UWORD);
  assert(mvm->stack->data[0].as.uw == 41);
  mvm_destroy(mvm);

  const uint32_t wrong_types[] = {0};
  code = (Code){0};
  code_sword(&code, 1);
  code_pack(&code, 1, wrong_types);
  const size_t wrong_target = code_call(&code, MVM_OP_CALL, 1);
  code_op(&code, MVM_OP_RETURN);
  const size_t wrong_reader = code.size;
  code_uword(&code, 0);
  code_type(&code, MVM_OP_VA_ARG, 1);
  code_op(&code, MVM_OP_RETURN);
  for (size_t i = 0; i < 4; ++i)
    code.data[wrong_target + i] = (uint8_t)(wrong_reader >> (i * 8));
  mvm = create_vm(&code);
  assert(register_primitive(mvm, MVM_KIND_SWORD) == 0);
  assert(register_primitive(mvm, MVM_KIND_STRING) == 1);
  run_error(mvm);
  mvm_destroy(mvm);
}

static void test_array_and_function_references(void) {
  Code code = {0};
  code_op(&code, MVM_OP_RETURN);
  mvm_t *mvm = create_vm(&code);
  const uint32_t word = register_primitive(mvm, MVM_KIND_UWORD);
  const uint32_t array_type = register_array(mvm, word, false);
  const uint32_t array_reference = register_reference(mvm, array_type, true);
  const uint32_t function_type = register_function(mvm, word, false);
  const uint32_t function_reference =
      register_reference(mvm, function_type, true);

  mvm_value_t array;
  assert(mvm_array_new(mvm, MVM_KIND_UWORD, 2, &array) == MVM_RESULT_OK);
  push(mvm, array);
  assert(run_type(mvm, MVM_OP_TYPED_INIT, array_type) == MVM_RESULT_OK);
  const mvm_value_t array_ref = mvm->stack->data[0];
  mvm->gc_next_collection = 0;
  mvm_value_t garbage;
  assert(mvm_alloc(mvm, 32, &garbage) == MVM_RESULT_OK);
  assert(run_type(mvm, MVM_OP_REF_READ, array_reference) == MVM_RESULT_OK);
  mvm_value_t output = mvm->stack->data[0];
  assert(output.kind == MVM_KIND_ARRAY && output.has_full_type);
  assert(output.type_id == array_type);
  size_t length;
  assert(mvm_array_length(mvm, output, &length) == MVM_RESULT_OK && length == 2);

  mvm_stack_clear(mvm->stack);
  push(mvm, array_ref);
  push(mvm, array);
  assert(run_type(mvm, MVM_OP_REF_WRITE, array_reference) == MVM_RESULT_OK);
  push(mvm, array_ref);
  assert(run_type(mvm, MVM_OP_REF_READ, array_reference) == MVM_RESULT_OK);
  assert(mvm_array_length(mvm, mvm->stack->data[0], &length) == MVM_RESULT_OK);
  assert(length == 2);

  mvm_stack_clear(mvm->stack);
  push(mvm, (mvm_value_t){.kind = MVM_KIND_FUNC, .as.func = 0});
  assert(run_type(mvm, MVM_OP_TYPED_INIT, function_type) == MVM_RESULT_OK);
  assert(run_type(mvm, MVM_OP_REF_READ, function_reference) == MVM_RESULT_OK);
  output = mvm->stack->data[0];
  assert(output.kind == MVM_KIND_FUNC && output.as.func == 0);
  assert(output.has_full_type && output.type_id == function_type);

  mvm_stack_clear(mvm->stack);
  array.type_id = function_type;
  array.has_full_type = true;
  push(mvm, array_ref);
  push(mvm, array);
  assert(run_type(mvm, MVM_OP_REF_WRITE, array_reference) ==
         MVM_RESULT_RUNTIME_ERROR);
  mvm_destroy(mvm);
}

static void test_type_zero_reference(void) {
  Code code = {0};
  code_op(&code, MVM_OP_RETURN);
  mvm_t *mvm = create_vm(&code);
  const uint32_t type = register_primitive(mvm, MVM_KIND_TYPE);
  const uint32_t reference = register_reference(mvm, type, true);
  push(mvm, (mvm_value_t){.kind = MVM_KIND_TYPE, .as.type = 0});
  assert(run_type(mvm, MVM_OP_TYPED_INIT, type) == MVM_RESULT_OK);
  assert(run_type(mvm, MVM_OP_REF_READ, reference) == MVM_RESULT_OK);
  const mvm_value_t output = mvm->stack->data[0];
  assert(output.kind == MVM_KIND_TYPE && output.as.type == 0);
  assert(output.has_full_type && output.type_id == type);
  mvm_destroy(mvm);
}

static void test_full_type_identity(void) {
  Code code = {0};
  code_type(&code, MVM_OP_LIT_TYPE_ID, 0);
  code_type(&code, MVM_OP_LIT_TYPE_ID, 1);
  code_op(&code, MVM_OP_NE);
  code_type(&code, MVM_OP_LIT_TYPE_ID, 2);
  code_type(&code, MVM_OP_LIT_TYPE_ID, 3);
  code_op(&code, MVM_OP_NE);
  code_op(&code, MVM_OP_EQ);
  code_type(&code, MVM_OP_LIT_TYPE_ID, 4);
  code_type(&code, MVM_OP_LIT_TYPE_ID, 5);
  code_op(&code, MVM_OP_NE);
  code_op(&code, MVM_OP_EQ);
  code_op(&code, MVM_OP_RETURN);
  mvm_t *mvm = create_vm(&code);
  const mvm_type_desc_t desc = {
      .size = 1,
      .alignment = 1,
      .tag = MVM_TYPE_AGGREGATE,
      .kind = MVM_KIND_VOID,
  };
  const uint32_t first = register_type(mvm, &desc);
  const uint32_t second = register_type(mvm, &desc);
  assert(register_reference(mvm, first, true) == 2);
  assert(register_reference(mvm, first, false) == 3);
  assert(register_array(mvm, first, false) == 4);
  assert(register_array(mvm, second, false) == 5);
  run(mvm);
  assert(mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_BOOL);
  assert(mvm->stack->data[0].as.b);
  mvm_destroy(mvm);
}

static void test_decode_boundaries(void) {
  const mvm_opcodes_t ops[] = {
      MVM_OP_VA_ARG, MVM_OP_REF_READ, MVM_OP_REF_WRITE,
      MVM_OP_TYPED_INIT, MVM_OP_LIT_TYPE_ID,
  };
  for (size_t i = 0; i < sizeof(ops) / sizeof(*ops); ++i) {
    for (size_t available = 0; available < 4; ++available) {
      mvm_t *mvm = test_vm_create(NULL, 1 + available, 1);
      assert(mvm);
      memset(mvm->ram->data, 0, mvm->ram->size);
      mvm->ram->data[0] = (uint8_t)ops[i];
      assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
      assert(mvm->pc == 1);
      mvm_destroy(mvm);
    }
  }

  for (size_t available = 0; available < 12; ++available) {
    mvm_t *mvm = test_vm_create(NULL, 1 + available, 1);
    assert(mvm);
    memset(mvm->ram->data, 0, mvm->ram->size);
    mvm->ram->data[0] = MVM_OP_VA_PACK;
    if (available) mvm->ram->data[1] = 2;
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 1 && mvm->stack->size == 0);
    mvm_destroy(mvm);
  }
}

static void test_breakpoint(void) {
  Code code = {0};
  code_op(&code, MVM_OP_BREAKPOINT);
  code_sword(&code, 42);
  code_op(&code, MVM_OP_RETURN);
  mvm_t *mvm = create_vm(&code);
  assert(mvm_update(mvm) == MVM_RESULT_BREAKPOINT);
  assert(mvm->pc == 1 && mvm->working && mvm->stack->size == 0);
  run(mvm);
  assert(mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_SWORD);
  assert(mvm->stack->data[0].as.sw == 42);
  mvm_destroy(mvm);
}

int main(void) {
  test_full_type();
  test_scalar_references();
  test_struct_initialization();
  test_variadic_sum();
  test_empty_and_forwarded_packets();
  test_va_arg_narrowing_and_errors();
  test_variadic_native_adapter();
  test_indirect_variadic_sum();
  test_variadic_full_types();
  test_array_and_function_references();
  test_type_zero_reference();
  test_full_type_identity();
  test_decode_boundaries();
  test_breakpoint();
  return 0;
}
