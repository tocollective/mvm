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
#include "../source/mvm/internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <string.h>

static mvm_t *create_vm(size_t size) {
  mvm_t *mvm = test_vm_create(NULL, size, MVM_FRAME_DEFAULT_LIMIT);
  assert(mvm && mvm->ram && mvm->stack);
  return mvm;
}

static void test_literal_sequence(void) {
  const uint8_t program[] = {
      MVM_OP_LIT_FLOAT,  0x00, 0x00, 0xC0, 0x3F,                         // 1.5f
      MVM_OP_LIT_DOUBLE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0xC0, // -2.5
      MVM_OP_RETURN,
  };
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));

  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->pc == 5 && mvm->working);
  assert(mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_FLOAT);
  assert(mvm->stack->data[0].as.f == 1.5f);

  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->pc == 14 && mvm->working);
  assert(mvm->stack->size == 2);
  assert(mvm->stack->data[1].kind == MVM_KIND_DOUBLE);
  assert(mvm->stack->data[1].as.d == -2.5);
  assert(mvm->stack->data[0].as.f == 1.5f);

  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->pc == sizeof(program) && !mvm->working);
  assert(mvm->stack->size == 2);
  assert(memcmp(mvm->ram->data, program, sizeof(program)) == 0);
  mvm_destroy(mvm);
}

static void test_float_bits(void) {
  const uint32_t cases[] = {
      UINT32_C(0x00000000), // Positive zero
      UINT32_C(0x80000000), // Negative zero
      UINT32_C(0x00000001), // Smallest positive subnormal
      UINT32_C(0x7F7FFFFF), // Largest finite value
      UINT32_C(0x7F800000), // Positive infinity
      UINT32_C(0xFF800000), // Negative infinity
      UINT32_C(0x7FC01234), // Quiet NaN with a payload
  };
  mvm_t *mvm = create_vm(5);
  mvm->ram->data[0] = MVM_OP_LIT_FLOAT;
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    for (size_t byte = 0; byte < 4; ++byte)
      mvm->ram->data[1 + byte] = (uint8_t)(cases[i] >> (byte * 8));
    mvm->pc = 0;
    mvm_stack_clear(mvm->stack);
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm->pc == 5 && mvm->stack->size == 1);
    assert(mvm->stack->data[0].kind == MVM_KIND_FLOAT);
    uint32_t actual;
    memcpy(&actual, &mvm->stack->data[0].as.f, sizeof(actual));
    assert(actual == cases[i]);
  }
  mvm_destroy(mvm);
}

static void test_double_bits(void) {
  const uint64_t cases[] = {
      UINT64_C(0x0000000000000000), // Positive zero
      UINT64_C(0x8000000000000000), // Negative zero
      UINT64_C(0x0000000000000001), // Smallest positive subnormal
      UINT64_C(0x7FEFFFFFFFFFFFFF), // Largest finite value
      UINT64_C(0x7FF0000000000000), // Positive infinity
      UINT64_C(0xFFF0000000000000), // Negative infinity
      UINT64_C(0x7FF8000000001234), // Quiet NaN with a payload
  };
  mvm_t *mvm = create_vm(9);
  mvm->ram->data[0] = MVM_OP_LIT_DOUBLE;
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    for (size_t byte = 0; byte < 8; ++byte)
      mvm->ram->data[1 + byte] = (uint8_t)(cases[i] >> (byte * 8));
    mvm->pc = 0;
    mvm_stack_clear(mvm->stack);
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm->pc == 9 && mvm->stack->size == 1);
    assert(mvm->stack->data[0].kind == MVM_KIND_DOUBLE);
    uint64_t actual;
    memcpy(&actual, &mvm->stack->data[0].as.d, sizeof(actual));
    assert(actual == cases[i]);
  }
  mvm_destroy(mvm);
}

static void test_literal_errors(mvm_opcodes_t op, size_t width) {
  for (size_t available = 0; available < width; ++available) {
    mvm_t *mvm = create_vm(1 + available);
    memset(mvm->ram->data, 0xFF, mvm->ram->size);
    mvm->ram->data[0] = op;
    const mvm_value_t sentinel = {.kind = MVM_KIND_UBYTE, .as.ub = 42};
    assert(mvm_stack_push(mvm->stack, sentinel));
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 1);
    assert(mvm->stack->size == 1);
    assert(mvm->stack->data[0].kind == MVM_KIND_UBYTE);
    assert(mvm->stack->data[0].as.ub == 42);
    mvm_destroy(mvm);
  }

  mvm_t *mvm = create_vm(1 + width);
  memset(mvm->ram->data, 0, mvm->ram->size);
  mvm->ram->data[0] = op;
  mvm_stack_destroy(mvm->stack);
  mvm->stack = NULL;
  assert(mvm_update(mvm) == MVM_RESULT_VM_ERROR);
  assert(mvm->pc == 1);
  mvm_destroy(mvm);
}

static void test_integer_literals(void) {
  const uint8_t program[] = {
      MVM_OP_LIT_UBYTE, 0xAB,                                           //
      MVM_OP_LIT_UHALF, 0x34, 0x12,                                     //
      MVM_OP_LIT_UWORD, 0x78, 0x56, 0x34, 0x12,                         //
      MVM_OP_LIT_ULONG, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, //
      MVM_OP_LIT_SBYTE, 0xFF,                                           // -1
      MVM_OP_LIT_SHALF, 0xFE, 0xFF,                                     // -2
      MVM_OP_LIT_SWORD, 0xFD, 0xFF, 0xFF, 0xFF,                         // -3
      MVM_OP_LIT_SLONG, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // -4
      MVM_OP_RETURN,
  };
  const size_t ends[] = {2, 5, 10, 19, 21, 24, 29, 38};
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));

  for (size_t i = 0; i < sizeof(ends) / sizeof(ends[0]); ++i) {
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm->pc == ends[i] && mvm->stack->size == i + 1);
  }
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(!mvm->working);

  const mvm_value_t *v = mvm->stack->data;
  assert(v[0].kind == MVM_KIND_UBYTE && v[0].as.ub == 0xAB);
  assert(v[1].kind == MVM_KIND_UHALF && v[1].as.uh == 0x1234);
  assert(v[2].kind == MVM_KIND_UWORD && v[2].as.uw == 0x12345678);
  assert(v[3].kind == MVM_KIND_ULONG && v[3].as.ul == 0x0102030405060708);
  assert(v[4].kind == MVM_KIND_SBYTE && v[4].as.sb == -1);
  assert(v[5].kind == MVM_KIND_SHALF && v[5].as.sh == -2);
  assert(v[6].kind == MVM_KIND_SWORD && v[6].as.sw == -3);
  assert(v[7].kind == MVM_KIND_SLONG && v[7].as.sl == -4);
  mvm_destroy(mvm);
}

static void run(mvm_t *mvm) {
  for (size_t steps = 0; mvm->working; ++steps) {
    assert(steps < 10000);
    assert(mvm_update(mvm) == MVM_RESULT_OK);
  }
}

static void test_jump(void) {
  const uint8_t program[] = {
      MVM_OP_JUMP,      7, 0, 0, 0, // 0: skip the first value
      MVM_OP_LIT_UBYTE, 1,          // 5
      MVM_OP_LIT_UBYTE, 2,          // 7
      MVM_OP_RETURN,                // 9
  };
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));
  run(mvm);
  assert(mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_UBYTE);
  assert(mvm->stack->data[0].as.ub == 2);
  mvm_destroy(mvm);

  const uint8_t jump_to_start[] = {MVM_OP_JUMP, 0, 0, 0, 0};
  mvm = create_vm(sizeof(jump_to_start));
  memcpy(mvm->ram->data, jump_to_start, sizeof(jump_to_start));
  mvm_stack_destroy(mvm->stack);
  mvm->stack = NULL;
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->pc == 0 && mvm->working);
  mvm_destroy(mvm);
}

static void check_conditional_jump(const mvm_opcodes_t op, const bool condition,
                                   const uint8_t expected) {
  uint8_t program[] = {
      MVM_OP_LIT_TRUE, // 0: replaced below for false
      0,
      13,
      0,
      0,
      0, // 1: conditional jump
      MVM_OP_LIT_UBYTE,
      1, // 6: condition did not jump
      MVM_OP_JUMP,
      15,
      0,
      0,
      0, // 8
      MVM_OP_LIT_UBYTE,
      2,             // 13: condition jumped
      MVM_OP_RETURN, // 15
  };
  program[0] = condition ? MVM_OP_LIT_TRUE : MVM_OP_LIT_FALSE;
  program[1] = op;

  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));
  run(mvm);
  assert(mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_UBYTE);
  assert(mvm->stack->data[0].as.ub == expected);
  mvm_destroy(mvm);
}

static void test_conditional_jumps(void) {
  check_conditional_jump(MVM_OP_JUMP_IF_FALSE, false, 2);
  check_conditional_jump(MVM_OP_JUMP_IF_FALSE, true, 1);
  check_conditional_jump(MVM_OP_JUMP_IF_TRUE, false, 1);
  check_conditional_jump(MVM_OP_JUMP_IF_TRUE, true, 2);

  const uint8_t countdown[] = {
      MVM_OP_LIT_UBYTE,    3,          // 0
      MVM_OP_GLOBAL_SET,   0, 0, 0, 0, // 2
      MVM_OP_GLOBAL_GET,   0, 0, 0, 0, // 7: loop start
      MVM_OP_DEC,                      // 12
      MVM_OP_DUP,                      // 13
      MVM_OP_GLOBAL_SET,   0, 0, 0, 0, // 14
      MVM_OP_LIT_UBYTE,    0,          // 19
      MVM_OP_GT,                       // 21
      MVM_OP_JUMP_IF_TRUE, 7, 0, 0, 0, // 22
      MVM_OP_GLOBAL_GET,   0, 0, 0, 0, // 27
      MVM_OP_RETURN,                   // 32
  };
  mvm_t *mvm = create_vm(sizeof(countdown));
  memcpy(mvm->ram->data, countdown, sizeof(countdown));
  run(mvm);
  assert(mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_UBYTE);
  assert(mvm->stack->data[0].as.ub == 0);
  mvm_destroy(mvm);
}

static void test_jump_errors(void) {
  const mvm_opcodes_t ops[] = {
      MVM_OP_JUMP,
      MVM_OP_JUMP_IF_FALSE,
      MVM_OP_JUMP_IF_TRUE,
  };
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) {
    for (size_t available = 0; available < 4; ++available) {
      mvm_t *mvm = create_vm(1 + available);
      memset(mvm->ram->data, 0xFF, mvm->ram->size);
      mvm->ram->data[0] = ops[i];
      if (ops[i] != MVM_OP_JUMP)
        assert(mvm_stack_push(
            mvm->stack, (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = true}));
      assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
      assert(mvm->pc == 1);
      assert(mvm->stack->size == (ops[i] == MVM_OP_JUMP ? 0 : 1));
      mvm_destroy(mvm);
    }

    const uint8_t invalid_address[] = {ops[i], 6, 0, 0, 0};
    mvm_t *mvm = create_vm(sizeof(invalid_address));
    memcpy(mvm->ram->data, invalid_address, sizeof(invalid_address));
    if (ops[i] != MVM_OP_JUMP)
      assert(mvm_stack_push(
          mvm->stack, (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = true}));
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 1);
    assert(mvm->stack->size == (ops[i] == MVM_OP_JUMP ? 0 : 1));
    mvm_destroy(mvm);

    if (ops[i] == MVM_OP_JUMP)
      continue;

    const uint8_t valid_address[] = {ops[i], 0, 0, 0, 0};
    mvm = create_vm(sizeof(valid_address));
    memcpy(mvm->ram->data, valid_address, sizeof(valid_address));
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 1 && mvm->stack->size == 0);
    mvm_destroy(mvm);

    mvm = create_vm(sizeof(valid_address));
    memcpy(mvm->ram->data, valid_address, sizeof(valid_address));
    assert(mvm_stack_push(mvm->stack,
                          (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 1}));
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 1 && mvm->stack->size == 1);
    assert(mvm->stack->data[0].kind == MVM_KIND_UBYTE);
    assert(mvm->stack->data[0].as.ub == 1);
    mvm_destroy(mvm);

    mvm = create_vm(sizeof(valid_address));
    memcpy(mvm->ram->data, valid_address, sizeof(valid_address));
    mvm_stack_destroy(mvm->stack);
    mvm->stack = NULL;
    assert(mvm_update(mvm) == MVM_RESULT_VM_ERROR);
    assert(mvm->pc == 1);
    mvm_destroy(mvm);
  }
}

static void assert_numeric_equal(const mvm_value_t actual,
                                 const mvm_value_t expected) {
  assert(actual.kind == expected.kind);
  switch (expected.kind) {
  case MVM_KIND_BOOL:
    assert(actual.as.b == expected.as.b);
    break;
  case MVM_KIND_UBYTE:
    assert(actual.as.ub == expected.as.ub);
    break;
  case MVM_KIND_UHALF:
    assert(actual.as.uh == expected.as.uh);
    break;
  case MVM_KIND_UWORD:
    assert(actual.as.uw == expected.as.uw);
    break;
  case MVM_KIND_ULONG:
    assert(actual.as.ul == expected.as.ul);
    break;
  case MVM_KIND_SBYTE:
    assert(actual.as.sb == expected.as.sb);
    break;
  case MVM_KIND_SHALF:
    assert(actual.as.sh == expected.as.sh);
    break;
  case MVM_KIND_SWORD:
    assert(actual.as.sw == expected.as.sw);
    break;
  case MVM_KIND_SLONG:
    assert(actual.as.sl == expected.as.sl);
    break;
  case MVM_KIND_FLOAT:
    assert(actual.as.f == expected.as.f ||
           (isnan(actual.as.f) && isnan(expected.as.f)));
    break;
  case MVM_KIND_DOUBLE:
    assert(actual.as.d == expected.as.d ||
           (isnan(actual.as.d) && isnan(expected.as.d)));
    break;
  default:
    assert(false);
  }
}

static void check_binary_arithmetic(const mvm_opcodes_t op,
                                    const mvm_value_t lhs,
                                    const mvm_value_t rhs,
                                    const mvm_value_t expected) {
  mvm_t *mvm = create_vm(1);
  mvm->ram->data[0] = op;
  assert(mvm_stack_push(mvm->stack, lhs));
  assert(mvm_stack_push(mvm->stack, rhs));
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->pc == 1 && mvm->stack->size == 1);
  assert_numeric_equal(mvm->stack->data[0], expected);
  mvm_destroy(mvm);
}

static void check_unary_arithmetic(const mvm_opcodes_t op,
                                   const mvm_value_t value,
                                   const mvm_value_t expected) {
  mvm_t *mvm = create_vm(1);
  mvm->ram->data[0] = op;
  assert(mvm_stack_push(mvm->stack, value));
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->pc == 1 && mvm->stack->size == 1);
  assert_numeric_equal(mvm->stack->data[0], expected);
  mvm_destroy(mvm);
}

static void check_binary_arithmetic_error(const mvm_opcodes_t op,
                                          const mvm_value_t lhs,
                                          const mvm_value_t rhs) {
  mvm_t *mvm = create_vm(1);
  mvm->ram->data[0] = op;
  assert(mvm_stack_push(mvm->stack, lhs));
  assert(mvm_stack_push(mvm->stack, rhs));
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->pc == 1 && mvm->stack->size == 2);
  assert_numeric_equal(mvm->stack->data[0], lhs);
  assert_numeric_equal(mvm->stack->data[1], rhs);
  mvm_destroy(mvm);
}

static void test_binary_arithmetic(void) {
#define TEST_NUMERIC(value_kind, field, quotient, remainder)                   \
  do {                                                                         \
    check_binary_arithmetic(MVM_OP_ADD,                                        \
                            (mvm_value_t){.kind = value_kind, .as.field = 7},  \
                            (mvm_value_t){.kind = value_kind, .as.field = 2},  \
                            (mvm_value_t){.kind = value_kind, .as.field = 9}); \
    check_binary_arithmetic(MVM_OP_SUB,                                        \
                            (mvm_value_t){.kind = value_kind, .as.field = 7},  \
                            (mvm_value_t){.kind = value_kind, .as.field = 2},  \
                            (mvm_value_t){.kind = value_kind, .as.field = 5}); \
    check_binary_arithmetic(                                                   \
        MVM_OP_MUL, (mvm_value_t){.kind = value_kind, .as.field = 7},          \
        (mvm_value_t){.kind = value_kind, .as.field = 2},                      \
        (mvm_value_t){.kind = value_kind, .as.field = 14});                    \
    check_binary_arithmetic(                                                   \
        MVM_OP_DIV, (mvm_value_t){.kind = value_kind, .as.field = 7},          \
        (mvm_value_t){.kind = value_kind, .as.field = 2},                      \
        (mvm_value_t){.kind = value_kind, .as.field = quotient});              \
    check_binary_arithmetic(                                                   \
        MVM_OP_MOD, (mvm_value_t){.kind = value_kind, .as.field = 7},          \
        (mvm_value_t){.kind = value_kind, .as.field = 2},                      \
        (mvm_value_t){.kind = value_kind, .as.field = remainder});             \
  } while (0)

  TEST_NUMERIC(MVM_KIND_UBYTE, ub, 3, 1);
  TEST_NUMERIC(MVM_KIND_UHALF, uh, 3, 1);
  TEST_NUMERIC(MVM_KIND_UWORD, uw, 3, 1);
  TEST_NUMERIC(MVM_KIND_ULONG, ul, 3, 1);
  TEST_NUMERIC(MVM_KIND_SBYTE, sb, 3, 1);
  TEST_NUMERIC(MVM_KIND_SHALF, sh, 3, 1);
  TEST_NUMERIC(MVM_KIND_SWORD, sw, 3, 1);
  TEST_NUMERIC(MVM_KIND_SLONG, sl, 3, 1);
  TEST_NUMERIC(MVM_KIND_FLOAT, f, 3.5f, 1.0f);
  TEST_NUMERIC(MVM_KIND_DOUBLE, d, 3.5, 1.0);
#undef TEST_NUMERIC

  check_binary_arithmetic(MVM_OP_SUB,
                          (mvm_value_t){.kind = MVM_KIND_SWORD, .as.sw = -7},
                          (mvm_value_t){.kind = MVM_KIND_SWORD, .as.sw = 2},
                          (mvm_value_t){.kind = MVM_KIND_SWORD, .as.sw = -9});
  check_binary_arithmetic(MVM_OP_MOD,
                          (mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = -7.5},
                          (mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = 2.0},
                          (mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = -1.5});
}

static void test_unary_arithmetic(void) {
#define TEST_INC_DEC(value_kind, field)                                        \
  do {                                                                         \
    check_unary_arithmetic(MVM_OP_INC,                                         \
                           (mvm_value_t){.kind = value_kind, .as.field = 7},   \
                           (mvm_value_t){.kind = value_kind, .as.field = 8});  \
    check_unary_arithmetic(MVM_OP_DEC,                                         \
                           (mvm_value_t){.kind = value_kind, .as.field = 7},   \
                           (mvm_value_t){.kind = value_kind, .as.field = 6});  \
  } while (0)

  TEST_INC_DEC(MVM_KIND_UBYTE, ub);
  TEST_INC_DEC(MVM_KIND_UHALF, uh);
  TEST_INC_DEC(MVM_KIND_UWORD, uw);
  TEST_INC_DEC(MVM_KIND_ULONG, ul);
  TEST_INC_DEC(MVM_KIND_SBYTE, sb);
  TEST_INC_DEC(MVM_KIND_SHALF, sh);
  TEST_INC_DEC(MVM_KIND_SWORD, sw);
  TEST_INC_DEC(MVM_KIND_SLONG, sl);
  TEST_INC_DEC(MVM_KIND_FLOAT, f);
  TEST_INC_DEC(MVM_KIND_DOUBLE, d);
#undef TEST_INC_DEC

#define TEST_NEGATE(value_kind, field)                                         \
  check_unary_arithmetic(MVM_OP_NEG,                                           \
                         (mvm_value_t){.kind = value_kind, .as.field = 7},     \
                         (mvm_value_t){.kind = value_kind, .as.field = -7})
  TEST_NEGATE(MVM_KIND_SBYTE, sb);
  TEST_NEGATE(MVM_KIND_SHALF, sh);
  TEST_NEGATE(MVM_KIND_SWORD, sw);
  TEST_NEGATE(MVM_KIND_SLONG, sl);
  TEST_NEGATE(MVM_KIND_FLOAT, f);
  TEST_NEGATE(MVM_KIND_DOUBLE, d);
#undef TEST_NEGATE

  check_unary_arithmetic(
      MVM_OP_INC, (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = UINT8_MAX},
      (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 0});
  check_unary_arithmetic(
      MVM_OP_DEC, (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 0},
      (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = UINT8_MAX});
  check_unary_arithmetic(
      MVM_OP_INC, (mvm_value_t){.kind = MVM_KIND_SBYTE, .as.sb = INT8_MAX},
      (mvm_value_t){.kind = MVM_KIND_SBYTE, .as.sb = INT8_MIN});
  check_unary_arithmetic(
      MVM_OP_NEG, (mvm_value_t){.kind = MVM_KIND_SBYTE, .as.sb = INT8_MIN},
      (mvm_value_t){.kind = MVM_KIND_SBYTE, .as.sb = INT8_MIN});
  check_binary_arithmetic(
      MVM_OP_DIV, (mvm_value_t){.kind = MVM_KIND_SBYTE, .as.sb = INT8_MIN},
      (mvm_value_t){.kind = MVM_KIND_SBYTE, .as.sb = -1},
      (mvm_value_t){.kind = MVM_KIND_SBYTE, .as.sb = INT8_MIN});
  check_binary_arithmetic(
      MVM_OP_MOD, (mvm_value_t){.kind = MVM_KIND_SBYTE, .as.sb = INT8_MIN},
      (mvm_value_t){.kind = MVM_KIND_SBYTE, .as.sb = -1},
      (mvm_value_t){.kind = MVM_KIND_SBYTE, .as.sb = 0});
}

static void test_arithmetic_errors(void) {
  const mvm_opcodes_t binary_ops[] = {
      MVM_OP_ADD, MVM_OP_SUB, MVM_OP_MUL, MVM_OP_DIV, MVM_OP_MOD,
  };
  const mvm_opcodes_t unary_ops[] = {MVM_OP_NEG, MVM_OP_INC, MVM_OP_DEC};

  for (size_t i = 0; i < sizeof(binary_ops) / sizeof(binary_ops[0]); ++i) {
    mvm_t *mvm = create_vm(1);
    mvm->ram->data[0] = binary_ops[i];
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->stack->size == 0);
    mvm_destroy(mvm);

    mvm = create_vm(1);
    mvm->ram->data[0] = binary_ops[i];
    mvm_stack_destroy(mvm->stack);
    mvm->stack = NULL;
    assert(mvm_update(mvm) == MVM_RESULT_VM_ERROR);
    mvm_destroy(mvm);
  }

  for (size_t i = 0; i < sizeof(unary_ops) / sizeof(unary_ops[0]); ++i) {
    mvm_t *mvm = create_vm(1);
    mvm->ram->data[0] = unary_ops[i];
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->stack->size == 0);
    mvm_destroy(mvm);

    mvm = create_vm(1);
    mvm->ram->data[0] = unary_ops[i];
    mvm_stack_destroy(mvm->stack);
    mvm->stack = NULL;
    assert(mvm_update(mvm) == MVM_RESULT_VM_ERROR);
    mvm_destroy(mvm);
  }

  check_binary_arithmetic_error(
      MVM_OP_ADD, (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 1},
      (mvm_value_t){.kind = MVM_KIND_UHALF, .as.uh = 2});
  check_binary_arithmetic_error(
      MVM_OP_DIV, (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 7},
      (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 0});
  check_binary_arithmetic_error(
      MVM_OP_MOD, (mvm_value_t){.kind = MVM_KIND_SLONG, .as.sl = -7},
      (mvm_value_t){.kind = MVM_KIND_SLONG, .as.sl = 0});

  mvm_t *mvm = create_vm(1);
  mvm->ram->data[0] = MVM_OP_NEG;
  assert(mvm_stack_push(mvm->stack,
                        (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 7}));
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->stack->size == 1 && mvm->stack->data[0].as.ub == 7);
  mvm_destroy(mvm);

  mvm = create_vm(1);
  mvm->ram->data[0] = MVM_OP_INC;
  assert(mvm_stack_push(mvm->stack,
                        (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = true}));
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->stack->size == 1 && mvm->stack->data[0].as.b);
  mvm_destroy(mvm);

  check_binary_arithmetic(
      MVM_OP_DIV, (mvm_value_t){.kind = MVM_KIND_FLOAT, .as.f = 1.0f},
      (mvm_value_t){.kind = MVM_KIND_FLOAT, .as.f = 0.0f},
      (mvm_value_t){.kind = MVM_KIND_FLOAT, .as.f = INFINITY});

  mvm = create_vm(1);
  mvm->ram->data[0] = MVM_OP_MOD;
  assert(mvm_stack_push(mvm->stack,
                        (mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = 1.0}));
  assert(mvm_stack_push(mvm->stack,
                        (mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = 0.0}));
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->stack->size == 1 && isnan(mvm->stack->data[0].as.d));
  mvm_destroy(mvm);
}

static void test_arithmetic_program(void) {
  const uint8_t program[] = {
      MVM_OP_LIT_SWORD, 7, 0, 0, 0, //
      MVM_OP_LIT_SWORD, 2, 0, 0, 0, //
      MVM_OP_MUL,                   //
      MVM_OP_INC,                   //
      MVM_OP_RETURN,
  };
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));
  run(mvm);
  assert(mvm->stack->size == 1);
  assert_numeric_equal(mvm->stack->data[0],
                       (mvm_value_t){.kind = MVM_KIND_SWORD, .as.sw = 15});
  mvm_destroy(mvm);
}

static void test_bitwise(void) {
#define TEST_BITWISE(value_kind, field)                                        \
  do {                                                                         \
    check_binary_arithmetic(MVM_OP_BAND,                                       \
                            (mvm_value_t){.kind = value_kind, .as.field = 12}, \
                            (mvm_value_t){.kind = value_kind, .as.field = 10}, \
                            (mvm_value_t){.kind = value_kind, .as.field = 8}); \
    check_binary_arithmetic(MVM_OP_BOR,                                        \
                            (mvm_value_t){.kind = value_kind, .as.field = 12}, \
                            (mvm_value_t){.kind = value_kind, .as.field = 10}, \
                            (mvm_value_t){.kind = value_kind, .as.field = 14}); \
    check_binary_arithmetic(MVM_OP_BXOR,                                       \
                            (mvm_value_t){.kind = value_kind, .as.field = 12}, \
                            (mvm_value_t){.kind = value_kind, .as.field = 10}, \
                            (mvm_value_t){.kind = value_kind, .as.field = 6}); \
    check_unary_arithmetic(MVM_OP_BNOT,                                        \
                           (mvm_value_t){.kind = value_kind, .as.field = 0},   \
                           (mvm_value_t){.kind = value_kind, .as.field = -1}); \
  } while (0)

  TEST_BITWISE(MVM_KIND_UBYTE, ub);
  TEST_BITWISE(MVM_KIND_UHALF, uh);
  TEST_BITWISE(MVM_KIND_UWORD, uw);
  TEST_BITWISE(MVM_KIND_ULONG, ul);
  TEST_BITWISE(MVM_KIND_SBYTE, sb);
  TEST_BITWISE(MVM_KIND_SHALF, sh);
  TEST_BITWISE(MVM_KIND_SWORD, sw);
  TEST_BITWISE(MVM_KIND_SLONG, sl);
#undef TEST_BITWISE

  check_binary_arithmetic(MVM_OP_BAND,
                          (mvm_value_t){.kind = MVM_KIND_SBYTE, .as.sb = -1},
                          (mvm_value_t){.kind = MVM_KIND_SBYTE, .as.sb = 15},
                          (mvm_value_t){.kind = MVM_KIND_SBYTE, .as.sb = 15});
}

static void test_shifts(void) {
#define TEST_SHIFT(value_kind, field)                                          \
  do {                                                                         \
    check_binary_arithmetic(MVM_OP_SHL,                                        \
                            (mvm_value_t){.kind = value_kind, .as.field = 3},  \
                            (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 1}, \
                            (mvm_value_t){.kind = value_kind, .as.field = 6}); \
    check_binary_arithmetic(MVM_OP_SHR,                                        \
                            (mvm_value_t){.kind = value_kind, .as.field = 3},  \
                            (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 1}, \
                            (mvm_value_t){.kind = value_kind, .as.field = 1}); \
  } while (0)

  TEST_SHIFT(MVM_KIND_UBYTE, ub);
  TEST_SHIFT(MVM_KIND_UHALF, uh);
  TEST_SHIFT(MVM_KIND_UWORD, uw);
  TEST_SHIFT(MVM_KIND_ULONG, ul);
  TEST_SHIFT(MVM_KIND_SBYTE, sb);
  TEST_SHIFT(MVM_KIND_SHALF, sh);
  TEST_SHIFT(MVM_KIND_SWORD, sw);
  TEST_SHIFT(MVM_KIND_SLONG, sl);
#undef TEST_SHIFT

  check_binary_arithmetic(MVM_OP_SHL,
                          (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 0x81},
                          (mvm_value_t){.kind = MVM_KIND_ULONG, .as.ul = 1},
                          (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 2});
  check_binary_arithmetic(
      MVM_OP_SHL, (mvm_value_t){.kind = MVM_KIND_SBYTE, .as.sb = 64},
      (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 1},
      (mvm_value_t){.kind = MVM_KIND_SBYTE, .as.sb = INT8_MIN});
  check_binary_arithmetic(MVM_OP_SHL,
                          (mvm_value_t){.kind = MVM_KIND_SLONG, .as.sl = -1},
                          (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 1},
                          (mvm_value_t){.kind = MVM_KIND_SLONG, .as.sl = -2});
  check_binary_arithmetic(MVM_OP_SHL,
                          (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 1},
                          (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 8},
                          (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 0});
  check_binary_arithmetic(MVM_OP_SHL,
                          (mvm_value_t){.kind = MVM_KIND_ULONG, .as.ul = 1},
                          (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 64},
                          (mvm_value_t){.kind = MVM_KIND_ULONG, .as.ul = 0});
  check_binary_arithmetic(
      MVM_OP_SHL, (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 1},
      (mvm_value_t){.kind = MVM_KIND_ULONG, .as.ul = UINT64_MAX},
      (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 0});

  check_binary_arithmetic(MVM_OP_SHR,
                          (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 0x80},
                          (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 7},
                          (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 1});
  check_binary_arithmetic(MVM_OP_SHR,
                          (mvm_value_t){.kind = MVM_KIND_SBYTE, .as.sb = -8},
                          (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 1},
                          (mvm_value_t){.kind = MVM_KIND_SBYTE, .as.sb = -4});
  check_binary_arithmetic(MVM_OP_SHR,
                          (mvm_value_t){.kind = MVM_KIND_SBYTE, .as.sb = -8},
                          (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 0},
                          (mvm_value_t){.kind = MVM_KIND_SBYTE, .as.sb = -8});
  check_binary_arithmetic(MVM_OP_SHR,
                          (mvm_value_t){.kind = MVM_KIND_SWORD, .as.sw = -16},
                          (mvm_value_t){.kind = MVM_KIND_UHALF, .as.uh = 2},
                          (mvm_value_t){.kind = MVM_KIND_SWORD, .as.sw = -4});
  check_binary_arithmetic(
      MVM_OP_SHR, (mvm_value_t){.kind = MVM_KIND_SLONG, .as.sl = -1},
      (mvm_value_t){.kind = MVM_KIND_ULONG, .as.ul = UINT64_MAX},
      (mvm_value_t){.kind = MVM_KIND_SLONG, .as.sl = -1});
  check_binary_arithmetic(MVM_OP_SHR,
                          (mvm_value_t){.kind = MVM_KIND_SWORD, .as.sw = 7},
                          (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 32},
                          (mvm_value_t){.kind = MVM_KIND_SWORD, .as.sw = 0});
  check_binary_arithmetic(
      MVM_OP_SHR, (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = UINT32_MAX},
      (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 32},
      (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 0});
}

static void test_bitwise_errors(void) {
  const mvm_opcodes_t binary_ops[] = {
      MVM_OP_BAND, MVM_OP_BOR, MVM_OP_BXOR, MVM_OP_SHL, MVM_OP_SHR,
  };
  for (size_t i = 0; i < sizeof(binary_ops) / sizeof(binary_ops[0]); ++i) {
    mvm_t *mvm = create_vm(1);
    mvm->ram->data[0] = binary_ops[i];
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 1 && mvm->stack->size == 0);
    mvm_destroy(mvm);

    mvm = create_vm(1);
    mvm->ram->data[0] = binary_ops[i];
    assert(mvm_stack_push(mvm->stack,
                          (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 1}));
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 1 && mvm->stack->size == 1);
    mvm_destroy(mvm);

    mvm = create_vm(1);
    mvm->ram->data[0] = binary_ops[i];
    mvm_stack_destroy(mvm->stack);
    mvm->stack = NULL;
    assert(mvm_update(mvm) == MVM_RESULT_VM_ERROR);
    assert(mvm->pc == 1);
    mvm_destroy(mvm);
  }

  const mvm_opcodes_t logic_ops[] = {MVM_OP_BAND, MVM_OP_BOR, MVM_OP_BXOR};
  for (size_t i = 0; i < sizeof(logic_ops) / sizeof(logic_ops[0]); ++i) {
    check_binary_arithmetic_error(
        logic_ops[i], (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 1},
        (mvm_value_t){.kind = MVM_KIND_UHALF, .as.uh = 1});
    check_binary_arithmetic_error(
        logic_ops[i], (mvm_value_t){.kind = MVM_KIND_FLOAT, .as.f = 1.0f},
        (mvm_value_t){.kind = MVM_KIND_FLOAT, .as.f = 2.0f});
    check_binary_arithmetic_error(
        logic_ops[i], (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = true},
        (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = false});
  }

  const mvm_opcodes_t shift_ops[] = {MVM_OP_SHL, MVM_OP_SHR};
  for (size_t i = 0; i < sizeof(shift_ops) / sizeof(shift_ops[0]); ++i) {
    check_binary_arithmetic_error(
        shift_ops[i], (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 1},
        (mvm_value_t){.kind = MVM_KIND_SBYTE, .as.sb = 1});
    check_binary_arithmetic_error(
        shift_ops[i], (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 1},
        (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = true});
    check_binary_arithmetic_error(
        shift_ops[i], (mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = 2.0},
        (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 1});
  }

  const mvm_value_t invalid_not[] = {
      {.kind = MVM_KIND_BOOL, .as.b = true},
      {.kind = MVM_KIND_FLOAT, .as.f = 1.0f},
      {.kind = MVM_KIND_DOUBLE, .as.d = 1.0},
  };
  for (size_t i = 0; i < sizeof(invalid_not) / sizeof(invalid_not[0]); ++i) {
    mvm_t *mvm = create_vm(1);
    mvm->ram->data[0] = MVM_OP_BNOT;
    assert(mvm_stack_push(mvm->stack, invalid_not[i]));
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 1 && mvm->stack->size == 1);
    assert_numeric_equal(mvm->stack->data[0], invalid_not[i]);
    mvm_destroy(mvm);
  }

  mvm_t *mvm = create_vm(1);
  mvm->ram->data[0] = MVM_OP_BNOT;
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->pc == 1 && mvm->stack->size == 0);
  mvm_destroy(mvm);

  mvm = create_vm(1);
  mvm->ram->data[0] = MVM_OP_BNOT;
  mvm_stack_destroy(mvm->stack);
  mvm->stack = NULL;
  assert(mvm_update(mvm) == MVM_RESULT_VM_ERROR);
  assert(mvm->pc == 1);
  mvm_destroy(mvm);
}

static void test_bitwise_program(void) {
  const uint8_t program[] = {
      MVM_OP_LIT_UBYTE, 0x81,
      MVM_OP_LIT_ULONG, 1, 0, 0, 0, 0, 0, 0, 0,
      MVM_OP_SHL,
      MVM_OP_BNOT,
      MVM_OP_RETURN,
  };
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));
  run(mvm);
  assert(mvm->stack->size == 1);
  assert_numeric_equal(mvm->stack->data[0],
                       (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 0xFD});
  mvm_destroy(mvm);
}

static void check_cast(const mvm_value_t value, const mvm_kind_t kind,
                       const mvm_value_t expected) {
  mvm_t *mvm = create_vm(2);
  mvm->ram->data[0] = MVM_OP_CAST;
  mvm->ram->data[1] = (uint8_t)kind;
  assert(mvm_stack_push(mvm->stack, value));
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->pc == 2 && mvm->stack->size == 1);
  assert_numeric_equal(mvm->stack->data[0], expected);
  mvm_destroy(mvm);
}

static void check_cast_error(const mvm_value_t value, const mvm_kind_t kind) {
  mvm_t *mvm = create_vm(2);
  mvm->ram->data[0] = MVM_OP_CAST;
  mvm->ram->data[1] = (uint8_t)kind;
  assert(mvm_stack_push(mvm->stack, value));
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->pc == 1 && mvm->stack->size == 1);
  assert_numeric_equal(mvm->stack->data[0], value);
  mvm_destroy(mvm);
}

static void test_cast(void) {
  check_cast((mvm_value_t){.kind = MVM_KIND_SWORD, .as.sw = -42},
             MVM_KIND_DOUBLE,
             (mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = -42.0});
  check_cast((mvm_value_t){.kind = MVM_KIND_ULONG, .as.ul = 0x0102},
             MVM_KIND_UBYTE, (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 2});
  check_cast((mvm_value_t){.kind = MVM_KIND_SLONG, .as.sl = -1}, MVM_KIND_ULONG,
             (mvm_value_t){.kind = MVM_KIND_ULONG, .as.ul = UINT64_MAX});
  check_cast((mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = UINT32_MAX},
             MVM_KIND_SWORD,
             (mvm_value_t){.kind = MVM_KIND_SWORD, .as.sw = -1});
  check_cast((mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = true}, MVM_KIND_UBYTE,
             (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 1});
  check_cast((mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = false},
             MVM_KIND_DOUBLE,
             (mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = 0.0});
  check_cast((mvm_value_t){.kind = MVM_KIND_SLONG, .as.sl = 42}, MVM_KIND_FLOAT,
             (mvm_value_t){.kind = MVM_KIND_FLOAT, .as.f = 42.0f});
  check_cast((mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = 255.9},
             MVM_KIND_UBYTE,
             (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 255});
  check_cast((mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = -42.75},
             MVM_KIND_SHALF,
             (mvm_value_t){.kind = MVM_KIND_SHALF, .as.sh = -42});
  check_cast((mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 0}, MVM_KIND_BOOL,
             (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = false});
  check_cast((mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = -0.0},
             MVM_KIND_BOOL,
             (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = false});
  check_cast((mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = NAN}, MVM_KIND_BOOL,
             (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = true});

  const mvm_value_t nonzero[] = {
      {.kind = MVM_KIND_BOOL, .as.b = true},
      {.kind = MVM_KIND_UBYTE, .as.ub = 1},
      {.kind = MVM_KIND_UHALF, .as.uh = 1},
      {.kind = MVM_KIND_UWORD, .as.uw = 1},
      {.kind = MVM_KIND_ULONG, .as.ul = 1},
      {.kind = MVM_KIND_SBYTE, .as.sb = 1},
      {.kind = MVM_KIND_SHALF, .as.sh = 1},
      {.kind = MVM_KIND_SWORD, .as.sw = 1},
      {.kind = MVM_KIND_SLONG, .as.sl = 1},
      {.kind = MVM_KIND_FLOAT, .as.f = 1.0f},
      {.kind = MVM_KIND_DOUBLE, .as.d = 1.0},
  };
  for (size_t i = 0; i < sizeof(nonzero) / sizeof(nonzero[0]); ++i) {
    check_cast(nonzero[i], MVM_KIND_BOOL,
               (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = true});
    check_cast(nonzero[i], MVM_KIND_ULONG,
               (mvm_value_t){.kind = MVM_KIND_ULONG, .as.ul = 1});
    check_cast(nonzero[i], MVM_KIND_SLONG,
               (mvm_value_t){.kind = MVM_KIND_SLONG, .as.sl = 1});
    check_cast(nonzero[i], MVM_KIND_FLOAT,
               (mvm_value_t){.kind = MVM_KIND_FLOAT, .as.f = 1.0f});
    check_cast(nonzero[i], MVM_KIND_DOUBLE,
               (mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = 1.0});
  }

  const mvm_value_t integer_targets[] = {
      {.kind = MVM_KIND_UBYTE, .as.ub = 42},
      {.kind = MVM_KIND_UHALF, .as.uh = 42},
      {.kind = MVM_KIND_UWORD, .as.uw = 42},
      {.kind = MVM_KIND_ULONG, .as.ul = 42},
      {.kind = MVM_KIND_SBYTE, .as.sb = 42},
      {.kind = MVM_KIND_SHALF, .as.sh = 42},
      {.kind = MVM_KIND_SWORD, .as.sw = 42},
      {.kind = MVM_KIND_SLONG, .as.sl = 42},
  };
  for (size_t i = 0; i < sizeof(integer_targets) / sizeof(integer_targets[0]); ++i)
    check_cast((mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 42},
               integer_targets[i].kind, integer_targets[i]);

  check_cast_error((mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = INFINITY},
                   MVM_KIND_SLONG);
  check_cast_error((mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = NAN},
                   MVM_KIND_UBYTE);
  check_cast_error((mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = 256.0},
                   MVM_KIND_UBYTE);
  check_cast_error((mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = -1.0},
                   MVM_KIND_UBYTE);
  check_cast_error((mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = 2147483648.0},
                   MVM_KIND_SWORD);

  mvm_t *mvm = create_vm(2);
  mvm->ram->data[0] = MVM_OP_CAST;
  mvm->ram->data[1] = MVM_KIND_NULL;
  assert(mvm_stack_push(mvm->stack, (mvm_value_t){.kind = MVM_KIND_NULL}));
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->stack->size == 1 && mvm->stack->data[0].kind == MVM_KIND_NULL);
  mvm_destroy(mvm);

  mvm = create_vm(2);
  mvm->ram->data[0] = MVM_OP_CAST;
  mvm->ram->data[1] = MVM_KIND_UBYTE;
  assert(mvm_stack_push(mvm->stack, (mvm_value_t){.kind = MVM_KIND_NULL}));
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->pc == 1 && mvm->stack->size == 1 &&
         mvm->stack->data[0].kind == MVM_KIND_NULL);
  mvm_destroy(mvm);
}

static void test_typeof_is_and_literal_type(void) {
  for (size_t i = 0; i < MVM_KIND_COUNT; ++i) {
    const mvm_kind_t kind = (mvm_kind_t)i;
    const mvm_value_t value = {.kind = kind};

    mvm_t *mvm = create_vm(1);
    mvm->ram->data[0] = MVM_OP_TYPEOF;
    assert(mvm_stack_push(mvm->stack, value));
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm->pc == 1 && mvm->stack->size == 1);
    assert(mvm->stack->data[0].kind == MVM_KIND_TYPE);
    assert(mvm->stack->data[0].as.type == kind);
    mvm_destroy(mvm);

    mvm = create_vm(2);
    mvm->ram->data[0] = MVM_OP_IS;
    mvm->ram->data[1] = (uint8_t)kind;
    assert(mvm_stack_push(mvm->stack, value));
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm->pc == 2 && mvm->stack->size == 1);
    assert(mvm->stack->data[0].kind == MVM_KIND_BOOL);
    assert(mvm->stack->data[0].as.b);
    mvm_destroy(mvm);

    mvm = create_vm(2);
    mvm->ram->data[0] = MVM_OP_IS;
    mvm->ram->data[1] = (uint8_t)((i + 1) % MVM_KIND_COUNT);
    assert(mvm_stack_push(mvm->stack, value));
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm->stack->data[0].kind == MVM_KIND_BOOL);
    assert(!mvm->stack->data[0].as.b);
    mvm_destroy(mvm);

    mvm = create_vm(2);
    mvm->ram->data[0] = MVM_OP_LIT_TYPE;
    mvm->ram->data[1] = (uint8_t)kind;
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm->pc == 2 && mvm->stack->size == 1);
    assert(mvm->stack->data[0].kind == MVM_KIND_TYPE);
    assert(mvm->stack->data[0].as.type == kind);
    mvm_destroy(mvm);
  }
}

static void test_type_program(void) {
  const uint8_t program[] = {
      MVM_OP_LIT_SWORD,
      0xD6,
      0xFF,
      0xFF,
      0xFF, // -42
      MVM_OP_CAST,
      MVM_KIND_DOUBLE,
      MVM_OP_LIT_NULL,
      MVM_OP_TYPEOF,
      MVM_OP_LIT_TRUE,
      MVM_OP_IS,
      MVM_KIND_UBYTE,
      MVM_OP_LIT_TYPE,
      MVM_KIND_STRING,
      MVM_OP_RETURN,
  };
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));
  run(mvm);

  assert(mvm->pc == sizeof(program) && mvm->stack->size == 4);
  assert_numeric_equal(mvm->stack->data[0],
                       (mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = -42.0});
  assert(mvm->stack->data[1].kind == MVM_KIND_TYPE);
  assert(mvm->stack->data[1].as.type == MVM_KIND_NULL);
  assert(mvm->stack->data[2].kind == MVM_KIND_BOOL);
  assert(!mvm->stack->data[2].as.b);
  assert(mvm->stack->data[3].kind == MVM_KIND_TYPE);
  assert(mvm->stack->data[3].as.type == MVM_KIND_STRING);
  mvm_destroy(mvm);
}

static void test_type_op_errors(void) {
  const mvm_opcodes_t operand_ops[] = {
      MVM_OP_CAST,
      MVM_OP_IS,
      MVM_OP_LIT_TYPE,
  };
  for (size_t i = 0; i < sizeof(operand_ops) / sizeof(operand_ops[0]); ++i) {
    mvm_t *mvm = create_vm(1);
    mvm->ram->data[0] = operand_ops[i];
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 1 && mvm->stack->size == 0);
    mvm_destroy(mvm);

    const uint8_t invalid_kinds[] = {MVM_KIND_COUNT, UINT8_MAX};
    for (size_t j = 0; j < sizeof(invalid_kinds); ++j) {
      mvm = create_vm(2);
      mvm->ram->data[0] = operand_ops[i];
      mvm->ram->data[1] = invalid_kinds[j];
      assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
      assert(mvm->pc == 1 && mvm->stack->size == 0);
      mvm_destroy(mvm);
    }

    mvm = create_vm(2);
    mvm->ram->data[0] = operand_ops[i];
    mvm->ram->data[1] = MVM_KIND_NULL;
    mvm_stack_destroy(mvm->stack);
    mvm->stack = NULL;
    assert(mvm_update(mvm) == MVM_RESULT_VM_ERROR);
    assert(mvm->pc == 1);
    mvm_destroy(mvm);
  }

  const mvm_opcodes_t unary_ops[] = {MVM_OP_CAST, MVM_OP_TYPEOF, MVM_OP_IS};
  for (size_t i = 0; i < sizeof(unary_ops) / sizeof(unary_ops[0]); ++i) {
    mvm_t *mvm = create_vm(2);
    mvm->ram->data[0] = unary_ops[i];
    mvm->ram->data[1] = MVM_KIND_NULL;
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 1 && mvm->stack->size == 0);
    mvm_destroy(mvm);
  }
}

static void test_stack_ops(void) {
  const uint8_t program[] = {
      MVM_OP_LIT_UBYTE, 42, // 0
      MVM_OP_DUP,           // 2: 42 -> 42 42
      MVM_OP_LIT_TRUE,      // 3: 42 42 true
      MVM_OP_SWAP,          // 4: 42 true 42
      MVM_OP_POP,           // 5: 42 true
      MVM_OP_RETURN,        // 6
  };
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));

  run(mvm);
  assert(mvm->pc == sizeof(program) && !mvm->working);
  assert(mvm->stack->size == 2);
  assert(mvm->stack->data[0].kind == MVM_KIND_UBYTE);
  assert(mvm->stack->data[0].as.ub == 42);
  assert(mvm->stack->data[1].kind == MVM_KIND_BOOL);
  assert(mvm->stack->data[1].as.b);
  mvm_destroy(mvm);
}

static void test_stack_op_errors(void) {
  const mvm_opcodes_t ops[] = {MVM_OP_POP, MVM_OP_DUP, MVM_OP_SWAP};
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) {
    mvm_t *mvm = create_vm(1);
    mvm->ram->data[0] = ops[i];
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 1 && mvm->working && mvm->stack->size == 0);
    mvm_destroy(mvm);

    mvm = create_vm(1);
    mvm->ram->data[0] = ops[i];
    mvm_stack_destroy(mvm->stack);
    mvm->stack = NULL;
    assert(mvm_update(mvm) == MVM_RESULT_VM_ERROR);
    assert(mvm->pc == 1 && mvm->working);
    mvm_destroy(mvm);
  }

  // SWAP requires two values, not just one.
  {
    const uint8_t program[] = {MVM_OP_LIT_TRUE, MVM_OP_SWAP};
    mvm_t *mvm = create_vm(sizeof(program));
    memcpy(mvm->ram->data, program, sizeof(program));
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 2 && mvm->stack->size == 1);
    assert(mvm->stack->data[0].kind == MVM_KIND_BOOL);
    assert(mvm->stack->data[0].as.b);
    mvm_destroy(mvm);
  }

  // A callee cannot pop or duplicate values belonging to its caller.
  for (size_t i = 0; i < 2; ++i) {
    const uint8_t program[] = {
        MVM_OP_LIT_NULL,                // 0
        MVM_OP_CALL,     8, 0, 0, 0, 0, // 1: f()
        MVM_OP_RETURN,                  // 7
        ops[i],                         // 8: f
    };
    mvm_t *mvm = create_vm(sizeof(program));
    memcpy(mvm->ram->data, program, sizeof(program));
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 9 && mvm->frame_count == 1);
    assert(mvm->stack->size == 1);
    assert(mvm->stack->data[0].kind == MVM_KIND_NULL);
    mvm_destroy(mvm);
  }

  // The caller's value does not count toward SWAP's two operands.
  {
    const uint8_t program[] = {
        MVM_OP_LIT_NULL,                // 0
        MVM_OP_CALL,     8, 0, 0, 0, 0, // 1: f()
        MVM_OP_RETURN,                  // 7
        MVM_OP_LIT_TRUE,                // 8: f
        MVM_OP_SWAP,                    // 9
    };
    mvm_t *mvm = create_vm(sizeof(program));
    memcpy(mvm->ram->data, program, sizeof(program));
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 10 && mvm->frame_count == 1);
    assert(mvm->stack->size == 2);
    assert(mvm->stack->data[0].kind == MVM_KIND_NULL);
    assert(mvm->stack->data[1].kind == MVM_KIND_BOOL);
    assert(mvm->stack->data[1].as.b);
    mvm_destroy(mvm);
  }
}

static void test_local_ops(void) {
  const uint8_t program[] = {
      MVM_OP_LIT_UBYTE, 10,    // 0: local 0
      MVM_OP_LIT_UBYTE, 20,    // 2: local 1
      MVM_OP_LOCAL_GET, 0,  0, // 4: push local 0
      MVM_OP_LIT_UBYTE, 30,    // 7
      MVM_OP_LOCAL_SET, 1,  0, // 9: local 1 = 30, pop it
      MVM_OP_LOCAL_GET, 1,  0, // 12: push local 1
      MVM_OP_RETURN,           // 15
  };
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));

  run(mvm);
  assert(mvm->pc == sizeof(program) && !mvm->working);
  assert(mvm->stack->size == 4);
  assert(mvm->stack->data[0].kind == MVM_KIND_UBYTE);
  assert(mvm->stack->data[0].as.ub == 10);
  assert(mvm->stack->data[1].kind == MVM_KIND_UBYTE);
  assert(mvm->stack->data[1].as.ub == 30);
  assert(mvm->stack->data[2].kind == MVM_KIND_UBYTE);
  assert(mvm->stack->data[2].as.ub == 10);
  assert(mvm->stack->data[3].kind == MVM_KIND_UBYTE);
  assert(mvm->stack->data[3].as.ub == 30);
  mvm_destroy(mvm);
}

static void test_local_ops_in_call(void) {
  const uint8_t program[] = {
      MVM_OP_LIT_UBYTE, 42,             // 0: caller's local
      MVM_OP_LIT_UBYTE, 7,              // 2: argument
      MVM_OP_CALL,      11, 0, 0, 0, 1, // 4: f(7)
      MVM_OP_RETURN,                    // 10
      MVM_OP_LIT_TRUE,                  // 11: f
      MVM_OP_LOCAL_SET, 0,  0,          // 12: argument = true
      MVM_OP_LOCAL_GET, 0,  0,          // 15: return argument
      MVM_OP_RETURN,                    // 18
  };
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));

  run(mvm);
  assert(mvm->frame_count == 0 && mvm->stack->size == 2);
  assert(mvm->stack->data[0].kind == MVM_KIND_UBYTE);
  assert(mvm->stack->data[0].as.ub == 42);
  assert(mvm->stack->data[1].kind == MVM_KIND_BOOL);
  assert(mvm->stack->data[1].as.b);
  mvm_destroy(mvm);
}

static void test_local_op_errors(void) {
  const mvm_opcodes_t ops[] = {MVM_OP_LOCAL_GET, MVM_OP_LOCAL_SET,
                               MVM_OP_INC_LOCAL, MVM_OP_DEC_LOCAL};
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) {
    // Truncated index.
    for (size_t available = 0; available < 2; ++available) {
      mvm_t *mvm = create_vm(1 + available);
      memset(mvm->ram->data, 0xFF, mvm->ram->size);
      mvm->ram->data[0] = ops[i];
      const mvm_value_t sentinel = {.kind = MVM_KIND_UBYTE, .as.ub = 42};
      assert(mvm_stack_push(mvm->stack, sentinel));
      assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
      assert(mvm->pc == 1 && mvm->stack->size == 1);
      assert(mvm->stack->data[0].kind == MVM_KIND_UBYTE);
      assert(mvm->stack->data[0].as.ub == 42);
      mvm_destroy(mvm);
    }

    // Empty current frame.
    mvm_t *mvm = create_vm(3);
    mvm->ram->data[0] = ops[i];
    mvm->ram->data[1] = mvm->ram->data[2] = 0;
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 1 && mvm->stack->size == 0);
    mvm_destroy(mvm);

    // Index outside the current frame leaves the stack unchanged.
    const uint8_t program[] = {MVM_OP_LIT_NULL, ops[i], 1, 0};
    mvm = create_vm(sizeof(program));
    memcpy(mvm->ram->data, program, sizeof(program));
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 2 && mvm->stack->size == 1);
    assert(mvm->stack->data[0].kind == MVM_KIND_NULL);
    mvm_destroy(mvm);

    mvm = create_vm(3);
    mvm->ram->data[0] = ops[i];
    mvm->ram->data[1] = mvm->ram->data[2] = 0;
    mvm_stack_destroy(mvm->stack);
    mvm->stack = NULL;
    assert(mvm_update(mvm) == MVM_RESULT_VM_ERROR);
    assert(mvm->pc == 1);
    mvm_destroy(mvm);

    // A callee cannot access the caller's slots.
    const uint8_t call_program[] = {
        MVM_OP_LIT_NULL,                // 0
        MVM_OP_CALL,     8, 0, 0, 0, 0, // 1: f()
        MVM_OP_RETURN,                  // 7
        ops[i],          0, 0,          // 8: f
    };
    mvm = create_vm(sizeof(call_program));
    memcpy(mvm->ram->data, call_program, sizeof(call_program));
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 9 && mvm->frame_count == 1);
    assert(mvm->stack->size == 1);
    assert(mvm->stack->data[0].kind == MVM_KIND_NULL);
    mvm_destroy(mvm);
  }
}

static void test_global_ops(void) {
  const uint8_t program[] = {
      MVM_OP_LIT_UBYTE,  42,          // 0
      MVM_OP_GLOBAL_SET, 0,  0, 0, 0, // 2: global 0 = 42, pop it
      MVM_OP_GLOBAL_GET, 0,  0, 0, 0, // 7: push global 0
      MVM_OP_LIT_TRUE,                // 12
      MVM_OP_GLOBAL_SET, 2,  0, 0, 0, // 13: grow through global 2
      MVM_OP_GLOBAL_GET, 2,  0, 0, 0, // 18: push global 2
      MVM_OP_RETURN,                  // 23
  };
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));

  run(mvm);
  assert(mvm->pc == sizeof(program) && !mvm->working);
  assert(mvm->global_count == 3 && mvm->global_capacity >= 3);
  assert(mvm->globals[0].kind == MVM_KIND_UBYTE);
  assert(mvm->globals[0].as.ub == 42);
  assert(mvm->globals[1].kind == MVM_KIND_NULL);
  assert(mvm->globals[2].kind == MVM_KIND_BOOL);
  assert(mvm->globals[2].as.b);
  assert(mvm->stack->size == 2);
  assert(mvm->stack->data[0].kind == MVM_KIND_UBYTE);
  assert(mvm->stack->data[0].as.ub == 42);
  assert(mvm->stack->data[1].kind == MVM_KIND_BOOL);
  assert(mvm->stack->data[1].as.b);
  mvm_destroy(mvm);
}

static void test_global_ops_in_call(void) {
  const uint8_t program[] = {
      MVM_OP_LIT_UBYTE,  7,              // 0
      MVM_OP_GLOBAL_SET, 0,  0, 0, 0,    // 2
      MVM_OP_CALL,       20, 0, 0, 0, 0, // 7: f()
      MVM_OP_POP,                        // 13: discard f's result
      MVM_OP_GLOBAL_GET, 0,  0, 0, 0,    // 14
      MVM_OP_RETURN,                     // 19
      MVM_OP_GLOBAL_GET, 0,  0, 0, 0,    // 20: f reads global 0
      MVM_OP_POP,                        // 25
      MVM_OP_LIT_TRUE,                   // 26
      MVM_OP_GLOBAL_SET, 0,  0, 0, 0,    // 27: f overwrites global 0
      MVM_OP_LIT_NULL,                   // 32
      MVM_OP_RETURN,                     // 33
  };
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));

  run(mvm);
  assert(mvm->frame_count == 0 && mvm->global_count == 1);
  assert(mvm->globals[0].kind == MVM_KIND_BOOL);
  assert(mvm->globals[0].as.b);
  assert(mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_BOOL);
  assert(mvm->stack->data[0].as.b);
  mvm_destroy(mvm);
}

static void test_global_op_errors(void) {
  const mvm_opcodes_t ops[] = {MVM_OP_GLOBAL_GET, MVM_OP_GLOBAL_SET};
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) {
    // Truncated index.
    for (size_t available = 0; available < 4; ++available) {
      mvm_t *mvm = create_vm(1 + available);
      memset(mvm->ram->data, 0xFF, mvm->ram->size);
      mvm->ram->data[0] = ops[i];
      const mvm_value_t sentinel = {.kind = MVM_KIND_UBYTE, .as.ub = 42};
      assert(mvm_stack_push(mvm->stack, sentinel));
      assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
      assert(mvm->pc == 1 && mvm->stack->size == 1);
      assert(mvm->stack->data[0].kind == MVM_KIND_UBYTE);
      assert(mvm->stack->data[0].as.ub == 42);
      assert(mvm->global_count == 0);
      mvm_destroy(mvm);
    }

    // Reading a missing global or setting from an empty stack fails cleanly.
    mvm_t *mvm = create_vm(5);
    mvm->ram->data[0] = ops[i];
    mvm->ram->data[1] = mvm->ram->data[2] = 0;
    mvm->ram->data[3] = mvm->ram->data[4] = 0;
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 1 && mvm->stack->size == 0);
    assert(mvm->global_count == 0);
    mvm_destroy(mvm);

    mvm = create_vm(5);
    mvm->ram->data[0] = ops[i];
    mvm->ram->data[1] = mvm->ram->data[2] = 0;
    mvm->ram->data[3] = mvm->ram->data[4] = 0;
    mvm_stack_destroy(mvm->stack);
    mvm->stack = NULL;
    assert(mvm_update(mvm) == MVM_RESULT_VM_ERROR);
    assert(mvm->pc == 1 && mvm->global_count == 0);
    mvm_destroy(mvm);
  }

  // A callee cannot consume the caller's value for GLOBAL_SET.
  const uint8_t program[] = {
      MVM_OP_LIT_UBYTE,  7,              // 0
      MVM_OP_GLOBAL_SET, 0,  0, 0, 0,    // 2
      MVM_OP_LIT_NULL,                   // 7: caller's local
      MVM_OP_CALL,       16, 0, 0, 0, 0, // 9: f()
      MVM_OP_RETURN,                     // 15
      MVM_OP_GLOBAL_SET, 0,  0, 0, 0,    // 16: f has no value to set
  };
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->pc == 17 && mvm->frame_count == 1);
  assert(mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_NULL);
  assert(mvm->global_count == 1);
  assert(mvm->globals[0].kind == MVM_KIND_UBYTE);
  assert(mvm->globals[0].as.ub == 7);
  mvm_destroy(mvm);
}

static void test_call_return(void) {
  const uint8_t program[] = {
      MVM_OP_LIT_UBYTE, 1,              // 0
      MVM_OP_LIT_UBYTE, 2,              // 2: argument
      MVM_OP_LIT_UBYTE, 3,              // 4: argument
      MVM_OP_CALL,      13, 0, 0, 0, 2, // 6: f(2, 3)
      MVM_OP_RETURN,                    // 12
      MVM_OP_LIT_TRUE,                  // 13: f
      MVM_OP_RETURN,                    // 14
  };
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));

  for (int i = 0; i < 4; ++i)
    assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->pc == 13 && mvm->frame_count == 1);
  assert(mvm->frames[0].return_pc == 12 && mvm->frames[0].base == 1);

  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->pc == 12 && mvm->frame_count == 0 && mvm->working);
  assert(mvm->stack->size == 2);
  assert(mvm->stack->data[0].kind == MVM_KIND_UBYTE);
  assert(mvm->stack->data[0].as.ub == 1);
  assert(mvm->stack->data[1].kind == MVM_KIND_BOOL);
  assert(mvm->stack->data[1].as.b);

  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(!mvm->working && mvm->stack->size == 2);
  mvm_destroy(mvm);
}

static void test_nested_calls(void) {
  // Function i calls function i + 1 and returns its result; the last one
  // returns 42. The depth exceeds the initial frame capacity.
  enum { DEPTH = 20, CALL_SIZE = 7 };
  uint8_t program[DEPTH * CALL_SIZE + 3];
  for (size_t i = 0; i < DEPTH; ++i) {
    uint8_t *code = program + i * CALL_SIZE;
    code[0] = MVM_OP_CALL;
    code[1] = (uint8_t)((i + 1) * CALL_SIZE);
    code[2] = code[3] = code[4] = 0;
    code[5] = 0;
    code[6] = MVM_OP_RETURN;
  }
  program[DEPTH * CALL_SIZE] = MVM_OP_LIT_UBYTE;
  program[DEPTH * CALL_SIZE + 1] = 42;
  program[DEPTH * CALL_SIZE + 2] = MVM_OP_RETURN;

  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));
  for (size_t i = 0; i < DEPTH; ++i)
    assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->frame_count == DEPTH && mvm->frame_capacity >= DEPTH);

  run(mvm);
  assert(mvm->frame_count == 0);
  assert(mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_UBYTE);
  assert(mvm->stack->data[0].as.ub == 42);
  mvm_destroy(mvm);
}

static void test_call_stack_overflow(void) {
  const uint8_t program[] = {MVM_OP_CALL, 0, 0, 0, 0, 0};
  mvm_t *mvm = test_vm_create(NULL, sizeof(program), 16);
  assert(mvm);
  memcpy(mvm->ram->data, program, sizeof(program));

  for (size_t i = 0; i < 16; ++i)
    assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->frame_count == 16 && mvm->frame_capacity == 16);
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->frame_count == 16 && mvm->pc == 1);
  mvm_destroy(mvm);

  mvm = test_vm_create(NULL, sizeof(program), 0);
  assert(mvm);
  memcpy(mvm->ram->data, program, sizeof(program));
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->frame_count == 0);
  mvm_destroy(mvm);
}

static void test_call_errors(void) {
  // Address outside RAM.
  {
    const uint8_t program[] = {MVM_OP_CALL, 6, 0, 0, 0, 0};
    mvm_t *mvm = create_vm(sizeof(program));
    memcpy(mvm->ram->data, program, sizeof(program));
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 1 && mvm->frame_count == 0);
    mvm_destroy(mvm);
  }
  // Truncated operands.
  for (size_t available = 0; available < 5; ++available) {
    mvm_t *mvm = create_vm(1 + available);
    memset(mvm->ram->data, 0, mvm->ram->size);
    mvm->ram->data[0] = MVM_OP_CALL;
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 1 && mvm->frame_count == 0);
    mvm_destroy(mvm);
  }
  // More arguments than values on the stack.
  {
    const uint8_t program[] = {
        MVM_OP_LIT_NULL, //
        MVM_OP_CALL,     0, 0, 0, 0, 2,
    };
    mvm_t *mvm = create_vm(sizeof(program));
    memcpy(mvm->ram->data, program, sizeof(program));
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 2 && mvm->frame_count == 0 && mvm->stack->size == 1);
    mvm_destroy(mvm);
  }
  // Arguments taken from below the caller's frame.
  {
    const uint8_t program[] = {
        MVM_OP_LIT_NULL,                // 0
        MVM_OP_CALL,     7, 0, 0, 0, 0, // 1: f()
        MVM_OP_CALL,     0, 0, 0, 0, 1, // 7: f takes the caller's value
    };
    mvm_t *mvm = create_vm(sizeof(program));
    memcpy(mvm->ram->data, program, sizeof(program));
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->frame_count == 1 && mvm->stack->size == 1);
    mvm_destroy(mvm);
  }
}

static void test_tail_call_recursion(void) {
  const uint8_t program[] = {
      MVM_OP_LIT_UHALF,   0xE8, 0x03,    // 0: countdown(1000)
      MVM_OP_CALL,        10, 0, 0, 0, 1, // 3
      MVM_OP_RETURN,                       // 9
      MVM_OP_LOCAL_GET,   0, 0,            // 10: countdown(n)
      MVM_OP_LIT_UHALF,   0, 0,            // 13
      MVM_OP_EQ,                           // 16
      MVM_OP_JUMP_IF_TRUE, 32, 0, 0, 0,   // 17
      MVM_OP_LOCAL_GET,   0, 0,            // 22
      MVM_OP_DEC,                          // 25
      MVM_OP_TAIL_CALL,   10, 0, 0, 0, 1, // 26
      MVM_OP_LIT_UHALF,   42, 0,           // 32
      MVM_OP_RETURN,                       // 35
  };
  mvm_t *mvm = test_vm_create(NULL, sizeof(program), 1);
  assert(mvm);
  memcpy(mvm->ram->data, program, sizeof(program));

  for (size_t i = 0; i < 9; ++i)
    assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->pc == 10 && mvm->frame_count == 1);
  assert(mvm->frames[0].base == 0 && mvm->frames[0].return_pc == 9);
  assert(mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_UHALF);
  assert(mvm->stack->data[0].as.uh == 999);

  run(mvm);
  assert(mvm->frame_count == 0 && mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_UHALF);
  assert(mvm->stack->data[0].as.uh == 42);
  mvm_destroy(mvm);
}

static void test_tail_call_reuses_frame(void) {
  const uint8_t program[] = {
      MVM_OP_LIT_UBYTE, 99,                // 0: caller local
      MVM_OP_CALL,      9, 0, 0, 0, 0,    // 2: f()
      MVM_OP_RETURN,                       // 8
      MVM_OP_LIT_UBYTE, 1,                 // 9: f's old local
      MVM_OP_LIT_UBYTE, 20,                // 11: first new argument
      MVM_OP_LIT_UBYTE, 22,                // 13: second new argument
      MVM_OP_TAIL_CALL, 21, 0, 0, 0, 2,   // 15: tail add(20, 22)
      MVM_OP_LOCAL_GET, 0, 0,              // 21: add(a, b)
      MVM_OP_LOCAL_GET, 1, 0,              // 24
      MVM_OP_ADD,                          // 27
      MVM_OP_RETURN,                       // 28
  };
  mvm_t *mvm = test_vm_create(NULL, sizeof(program), 1);
  assert(mvm);
  memcpy(mvm->ram->data, program, sizeof(program));

  for (size_t i = 0; i < 6; ++i)
    assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->pc == 21 && mvm->frame_count == 1);
  assert(mvm->frames[0].base == 1 && mvm->frames[0].return_pc == 8);
  assert(mvm->stack->size == 3);
  assert(mvm->stack->data[0].as.ub == 99);
  assert(mvm->stack->data[1].as.ub == 20);
  assert(mvm->stack->data[2].as.ub == 22);

  run(mvm);
  assert(mvm->frame_count == 0 && mvm->stack->size == 2);
  assert(mvm->stack->data[0].as.ub == 99);
  assert(mvm->stack->data[1].as.ub == 42);
  mvm_destroy(mvm);

  const uint8_t zero_args[] = {
      MVM_OP_LIT_NULL,                    // 0: discarded top-level local
      MVM_OP_TAIL_CALL, 7, 0, 0, 0, 0,   // 1
      MVM_OP_LIT_UBYTE, 42,               // 7
      MVM_OP_RETURN,                      // 9
  };
  mvm = create_vm(sizeof(zero_args));
  memcpy(mvm->ram->data, zero_args, sizeof(zero_args));
  run(mvm);
  assert(mvm->frame_count == 0 && mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_UBYTE);
  assert(mvm->stack->data[0].as.ub == 42);
  mvm_destroy(mvm);
}

static void test_tail_call_errors(void) {
  for (size_t available = 0; available < 5; ++available) {
    mvm_t *mvm = create_vm(1 + available);
    memset(mvm->ram->data, 0, mvm->ram->size);
    mvm->ram->data[0] = MVM_OP_TAIL_CALL;
    assert(mvm_stack_push(mvm->stack, (mvm_value_t){.kind = MVM_KIND_NULL}));
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 1 && mvm->frame_count == 0 && mvm->stack->size == 1);
    assert(mvm->stack->data[0].kind == MVM_KIND_NULL);
    mvm_destroy(mvm);
  }

  const uint8_t invalid_address[] = {
      MVM_OP_LIT_NULL,
      MVM_OP_TAIL_CALL, 7, 0, 0, 0, 0,
  };
  mvm_t *mvm = create_vm(sizeof(invalid_address));
  memcpy(mvm->ram->data, invalid_address, sizeof(invalid_address));
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->pc == 2 && mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_NULL);
  mvm_destroy(mvm);

  const uint8_t missing_args[] = {
      MVM_OP_LIT_NULL,
      MVM_OP_TAIL_CALL, 0, 0, 0, 0, 2,
  };
  mvm = create_vm(sizeof(missing_args));
  memcpy(mvm->ram->data, missing_args, sizeof(missing_args));
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->pc == 2 && mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_NULL);
  mvm_destroy(mvm);

  const uint8_t caller_args[] = {
      MVM_OP_LIT_NULL,                     // 0: caller local, not an argument
      MVM_OP_CALL,      8, 0, 0, 0, 0,    // 1
      MVM_OP_RETURN,                       // 7
      MVM_OP_TAIL_CALL, 0, 0, 0, 0, 1,    // 8
  };
  mvm = create_vm(sizeof(caller_args));
  memcpy(mvm->ram->data, caller_args, sizeof(caller_args));
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->pc == 9 && mvm->frame_count == 1);
  assert(mvm->frames[0].base == 1 && mvm->frames[0].return_pc == 7);
  assert(mvm->stack->size == 1 && mvm->stack->data[0].kind == MVM_KIND_NULL);
  mvm_destroy(mvm);

  mvm = create_vm(6);
  mvm->ram->data[0] = MVM_OP_TAIL_CALL;
  mvm_stack_destroy(mvm->stack);
  mvm->stack = NULL;
  assert(mvm_update(mvm) == MVM_RESULT_VM_ERROR);
  assert(mvm->pc == 1 && mvm->frame_count == 0);
  mvm_destroy(mvm);
}

static void test_return_errors(void) {
  // Top-level return without a value.
  {
    mvm_t *mvm = create_vm(1);
    mvm->ram->data[0] = MVM_OP_RETURN;
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->working);
    mvm_destroy(mvm);
  }
  // Callee returns without pushing a value; the caller's value is not used.
  {
    const uint8_t program[] = {
        MVM_OP_LIT_NULL,                // 0
        MVM_OP_CALL,     8, 0, 0, 0, 0, // 1: f()
        MVM_OP_RETURN,                  // 7
        MVM_OP_RETURN,                  // 8: f
    };
    mvm_t *mvm = create_vm(sizeof(program));
    memcpy(mvm->ram->data, program, sizeof(program));
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->frame_count == 1 && mvm->stack->size == 1 && mvm->working);
    mvm_destroy(mvm);
  }
}

static void test_pc_out_of_bounds(void) {
  // Running off the end must not decode the zero from peek as RETURN.
  {
    mvm_t *mvm = create_vm(1);
    mvm->ram->data[0] = MVM_OP_LIT_TRUE;
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm->pc == 1 && mvm->stack->size == 1);
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 1 && mvm->working && mvm->stack->size == 1);
    mvm_destroy(mvm);
  }
  // Empty RAM.
  {
    mvm_t *mvm = create_vm(0);
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 0 && mvm->working);
    mvm_destroy(mvm);
  }
}

static void test_string_program(void) {
  const uint8_t program[] = {
      MVM_OP_LIT_STRING,
      16,
      0,
      0,
      0, // "A😀"
      MVM_OP_LIT_STRING,
      22,
      0,
      0,
      0, // UTF-8 string: U+044F followed by Z.
      MVM_OP_CONCAT,
      MVM_OP_DUP,
      MVM_OP_STRING_LEN,
      MVM_OP_RETURN,
      0,
      0,
      'A',
      0xF0,
      0x9F,
      0x98,
      0x80,
      0,
      0xD1,
      0x8F,
      'Z',
      0,
  };
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));

  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->pc == 5 && mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_STRING);
  assert(mvm->stack->data[0].as.s == (char *)mvm->ram->data + 16);
  assert(strcmp(mvm->stack->data[0].as.s, "A\xF0\x9F\x98\x80") == 0);
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->pc == 10 && mvm->stack->size == 2);
  assert(strcmp(mvm->stack->data[1].as.s, "\xD1\x8FZ") == 0);

  run(mvm);
  assert(mvm->pc == 14 && mvm->stack->size == 2);
  assert(mvm->stack->data[0].kind == MVM_KIND_STRING);
  assert(strcmp(mvm->stack->data[0].as.s, "A\xF0\x9F\x98\x80\xD1\x8FZ") == 0);
  assert(mvm->stack->data[1].kind == MVM_KIND_UWORD);
  assert(mvm->stack->data[1].as.uw == 4);
  assert(mvm->objects);
  mvm_destroy(mvm);
}

static void check_string_get(const mvm_value_t index, const uint32_t expected) {
  mvm_t *mvm = create_vm(1);
  mvm->ram->data[0] = MVM_OP_STRING_GET;
  assert(mvm_stack_push(mvm->stack,
                        (mvm_value_t){.kind = MVM_KIND_STRING,
                                      .as.s = "A\xF0\x9F\x98\x80\xD1\x8FZ"}));
  assert(mvm_stack_push(mvm->stack, index));
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->pc == 1 && mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_UWORD);
  assert(mvm->stack->data[0].as.uw == expected);
  mvm_destroy(mvm);
}

static void test_string_get(void) {
  check_string_get((mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 0}, 0x41);
  check_string_get((mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 1}, 0x1F600);
  check_string_get((mvm_value_t){.kind = MVM_KIND_SHALF, .as.sh = 2}, 0x44F);
  check_string_get((mvm_value_t){.kind = MVM_KIND_ULONG, .as.ul = 3}, 0x5A);

  mvm_t *mvm = create_vm(1);
  mvm->ram->data[0] = MVM_OP_STRING_LEN;
  assert(mvm_stack_push(mvm->stack,
                        (mvm_value_t){.kind = MVM_KIND_STRING, .as.s = ""}));
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_UWORD);
  assert(mvm->stack->data[0].as.uw == 0);
  mvm_destroy(mvm);
}

static void check_comparison(const mvm_opcodes_t op, const mvm_value_t lhs,
                             const mvm_value_t rhs, const bool expected) {
  mvm_t *mvm = create_vm(1);
  mvm->ram->data[0] = op;
  assert(mvm_stack_push(mvm->stack, lhs));
  assert(mvm_stack_push(mvm->stack, rhs));
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->pc == 1 && mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_BOOL);
  assert(mvm->stack->data[0].as.b == expected);
  mvm_destroy(mvm);
}

static void check_equal(const mvm_value_t lhs, const mvm_value_t rhs,
                        const bool expected) {
  check_comparison(MVM_OP_EQ, lhs, rhs, expected);
}

static void check_comparison_error(const mvm_opcodes_t op,
                                   const mvm_value_t lhs,
                                   const mvm_value_t rhs) {
  mvm_t *mvm = create_vm(1);
  mvm->ram->data[0] = op;
  assert(mvm_stack_push(mvm->stack, lhs));
  assert(mvm_stack_push(mvm->stack, rhs));
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->pc == 1 && mvm->stack->size == 2);
  assert_numeric_equal(mvm->stack->data[0], lhs);
  assert_numeric_equal(mvm->stack->data[1], rhs);
  mvm_destroy(mvm);
}

static void test_comparisons(void) {
#define TEST_ORDERED(value_kind, field)                                        \
  do {                                                                         \
    check_comparison(MVM_OP_LT,                                                \
                     (mvm_value_t){.kind = value_kind, .as.field = 2},         \
                     (mvm_value_t){.kind = value_kind, .as.field = 7}, true);  \
    check_comparison(MVM_OP_LE,                                                \
                     (mvm_value_t){.kind = value_kind, .as.field = 7},         \
                     (mvm_value_t){.kind = value_kind, .as.field = 7}, true);  \
    check_comparison(MVM_OP_GT,                                                \
                     (mvm_value_t){.kind = value_kind, .as.field = 7},         \
                     (mvm_value_t){.kind = value_kind, .as.field = 2}, true);  \
    check_comparison(MVM_OP_GE,                                                \
                     (mvm_value_t){.kind = value_kind, .as.field = 2},         \
                     (mvm_value_t){.kind = value_kind, .as.field = 7}, false); \
  } while (0)

  TEST_ORDERED(MVM_KIND_UBYTE, ub);
  TEST_ORDERED(MVM_KIND_UHALF, uh);
  TEST_ORDERED(MVM_KIND_UWORD, uw);
  TEST_ORDERED(MVM_KIND_ULONG, ul);
  TEST_ORDERED(MVM_KIND_SBYTE, sb);
  TEST_ORDERED(MVM_KIND_SHALF, sh);
  TEST_ORDERED(MVM_KIND_SWORD, sw);
  TEST_ORDERED(MVM_KIND_SLONG, sl);
  TEST_ORDERED(MVM_KIND_FLOAT, f);
  TEST_ORDERED(MVM_KIND_DOUBLE, d);
#undef TEST_ORDERED

  check_comparison(MVM_OP_LT,
                   (mvm_value_t){.kind = MVM_KIND_SWORD, .as.sw = -7},
                   (mvm_value_t){.kind = MVM_KIND_SWORD, .as.sw = -2}, true);
  check_comparison(MVM_OP_NE,
                   (mvm_value_t){.kind = MVM_KIND_SWORD, .as.sw = 42},
                   (mvm_value_t){.kind = MVM_KIND_SWORD, .as.sw = 42}, false);
  check_comparison(MVM_OP_NE,
                   (mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = 1.5},
                   (mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = 2.5}, true);
  check_comparison(MVM_OP_NE, (mvm_value_t){.kind = MVM_KIND_NULL},
                   (mvm_value_t){.kind = MVM_KIND_NULL}, false);
  check_comparison(MVM_OP_NE,
                   (mvm_value_t){.kind = MVM_KIND_STRING, .as.s = "1"},
                   (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 1}, true);
  check_comparison(MVM_OP_NE,
                   (mvm_value_t){.kind = MVM_KIND_STRING, .as.s = "a"},
                   (mvm_value_t){.kind = MVM_KIND_STRING, .as.s = "b"}, true);
  check_comparison(MVM_OP_NE,
                   (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = true},
                   (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = false}, true);
  check_comparison(
      MVM_OP_NE, (mvm_value_t){.kind = MVM_KIND_TYPE, .as.type = MVM_KIND_NULL},
      (mvm_value_t){.kind = MVM_KIND_TYPE, .as.type = MVM_KIND_BOOL}, true);
  check_comparison(MVM_OP_EQ,
                   (mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = NAN},
                   (mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = NAN}, false);
  check_comparison(MVM_OP_NE,
                   (mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = NAN},
                   (mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = NAN}, true);
  check_comparison(MVM_OP_LE,
                   (mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = NAN},
                   (mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = 1.0}, false);
}

static void test_comparison_errors(void) {
  const mvm_opcodes_t ordered_ops[] = {
      MVM_OP_LT,
      MVM_OP_LE,
      MVM_OP_GT,
      MVM_OP_GE,
  };
  for (size_t i = 0; i < sizeof(ordered_ops) / sizeof(ordered_ops[0]); ++i) {
    check_comparison_error(ordered_ops[i],
                           (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 1},
                           (mvm_value_t){.kind = MVM_KIND_UHALF, .as.uh = 2});
    check_comparison_error(ordered_ops[i],
                           (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = false},
                           (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = true});
  }
}

static void test_not(void) {
  const bool inputs[] = {false, true};
  for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
    mvm_t *mvm = create_vm(1);
    mvm->ram->data[0] = MVM_OP_NOT;
    assert(mvm_stack_push(
        mvm->stack, (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = inputs[i]}));
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm->pc == 1 && mvm->stack->size == 1);
    assert(mvm->stack->data[0].kind == MVM_KIND_BOOL);
    assert(mvm->stack->data[0].as.b == !inputs[i]);
    mvm_destroy(mvm);
  }

  mvm_t *mvm = create_vm(1);
  mvm->ram->data[0] = MVM_OP_NOT;
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->pc == 1 && mvm->stack->size == 0);
  mvm_destroy(mvm);

  mvm = create_vm(1);
  mvm->ram->data[0] = MVM_OP_NOT;
  assert(mvm_stack_push(mvm->stack,
                        (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 1}));
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->pc == 1 && mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_UBYTE);
  assert(mvm->stack->data[0].as.ub == 1);
  mvm_destroy(mvm);

  mvm = create_vm(1);
  mvm->ram->data[0] = MVM_OP_NOT;
  mvm_stack_destroy(mvm->stack);
  mvm->stack = NULL;
  assert(mvm_update(mvm) == MVM_RESULT_VM_ERROR);
  mvm_destroy(mvm);
}

static void test_comparison_program(void) {
  const uint8_t program[] = {
      MVM_OP_LIT_SWORD,
      2,
      0,
      0,
      0, //
      MVM_OP_LIT_SWORD,
      7,
      0,
      0,
      0, //
      MVM_OP_LT,
      MVM_OP_NOT,
      MVM_OP_RETURN,
  };
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));
  run(mvm);
  assert(mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_BOOL);
  assert(!mvm->stack->data[0].as.b);
  mvm_destroy(mvm);
}

static void test_string_equality(void) {
  char same[] = "A\xF0\x9F\x98\x80";
  check_equal(
      (mvm_value_t){.kind = MVM_KIND_STRING, .as.s = same},
      (mvm_value_t){.kind = MVM_KIND_STRING, .as.s = "A\xF0\x9F\x98\x80"},
      true);
  check_equal((mvm_value_t){.kind = MVM_KIND_STRING, .as.s = "same"},
              (mvm_value_t){.kind = MVM_KIND_STRING, .as.s = "different"},
              false);
  check_equal((mvm_value_t){.kind = MVM_KIND_SWORD, .as.sw = 42},
              (mvm_value_t){.kind = MVM_KIND_SWORD, .as.sw = 42}, true);
  check_equal((mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = 1.5},
              (mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = 2.5}, false);
  check_equal((mvm_value_t){.kind = MVM_KIND_NULL},
              (mvm_value_t){.kind = MVM_KIND_NULL}, true);
  check_equal((mvm_value_t){.kind = MVM_KIND_STRING, .as.s = "1"},
              (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 1}, false);
}

static void check_string_literal_error(const uint8_t *data, size_t size) {
  mvm_t *mvm = create_vm(5 + size);
  mvm->ram->data[0] = MVM_OP_LIT_STRING;
  mvm->ram->data[1] = 5;
  memcpy(mvm->ram->data + 5, data, size);
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->pc == 1 && mvm->stack->size == 0);
  mvm_destroy(mvm);
}

static void test_string_errors(void) {
  const uint8_t unterminated[] = {'a'};
  const uint8_t overlong[] = {0xC0, 0x80, 0};
  const uint8_t truncated[] = {0xE2, 0x82, 0};
  const uint8_t surrogate[] = {0xED, 0xA0, 0x80, 0};
  const uint8_t too_large[] = {0xF4, 0x90, 0x80, 0x80, 0};
  const uint8_t bad_continuation[] = {0xE2, 0x28, 0xA1, 0};
  check_string_literal_error(unterminated, sizeof(unterminated));
  check_string_literal_error(overlong, sizeof(overlong));
  check_string_literal_error(truncated, sizeof(truncated));
  check_string_literal_error(surrogate, sizeof(surrogate));
  check_string_literal_error(too_large, sizeof(too_large));
  check_string_literal_error(bad_continuation, sizeof(bad_continuation));

  mvm_t *mvm = create_vm(5);
  mvm->ram->data[0] = MVM_OP_LIT_STRING;
  mvm->ram->data[1] = 5;
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->pc == 1 && mvm->stack->size == 0);
  mvm_destroy(mvm);

  mvm = create_vm(1);
  mvm->ram->data[0] = MVM_OP_CONCAT;
  assert(mvm_stack_push(mvm->stack,
                        (mvm_value_t){.kind = MVM_KIND_STRING, .as.s = "a"}));
  assert(mvm_stack_push(mvm->stack,
                        (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 1}));
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->pc == 1 && mvm->stack->size == 2);
  mvm_destroy(mvm);

  mvm = create_vm(1);
  mvm->ram->data[0] = MVM_OP_STRING_LEN;
  assert(mvm_stack_push(mvm->stack,
                        (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 1}));
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->pc == 1 && mvm->stack->size == 1);
  mvm_destroy(mvm);

  mvm = create_vm(1);
  mvm->ram->data[0] = MVM_OP_EQ;
  assert(mvm_stack_push(mvm->stack, (mvm_value_t){.kind = MVM_KIND_STRING}));
  assert(mvm_stack_push(mvm->stack,
                        (mvm_value_t){.kind = MVM_KIND_STRING, .as.s = "a"}));
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->pc == 1 && mvm->stack->size == 2);
  mvm_destroy(mvm);

  const mvm_value_t invalid_indices[] = {
      {.kind = MVM_KIND_UBYTE, .as.ub = 4},
      {.kind = MVM_KIND_SWORD, .as.sw = -1},
      {.kind = MVM_KIND_BOOL, .as.b = false},
  };
  for (size_t i = 0; i < sizeof(invalid_indices) / sizeof(invalid_indices[0]);
       ++i) {
    mvm = create_vm(1);
    mvm->ram->data[0] = MVM_OP_STRING_GET;
    assert(mvm_stack_push(mvm->stack, (mvm_value_t){.kind = MVM_KIND_STRING,
                                                    .as.s = "\xD1\x8F"}));
    assert(mvm_stack_push(mvm->stack, invalid_indices[i]));
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 1 && mvm->stack->size == 2);
    mvm_destroy(mvm);
  }

  const mvm_opcodes_t binary_ops[] = {
      MVM_OP_EQ,
      MVM_OP_CONCAT,
      MVM_OP_STRING_GET,
  };
  for (size_t i = 0; i < sizeof(binary_ops) / sizeof(binary_ops[0]); ++i) {
    mvm = create_vm(1);
    mvm->ram->data[0] = binary_ops[i];
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->stack->size == 0);
    mvm_destroy(mvm);

    mvm = create_vm(1);
    mvm->ram->data[0] = binary_ops[i];
    mvm_stack_destroy(mvm->stack);
    mvm->stack = NULL;
    assert(mvm_update(mvm) == MVM_RESULT_VM_ERROR);
    mvm_destroy(mvm);
  }

  mvm = create_vm(1);
  mvm->ram->data[0] = MVM_OP_STRING_LEN;
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  mvm_destroy(mvm);
}

static char *concat_managed_string(mvm_t *mvm, const char *lhs,
                                   const char *rhs) {
  assert(mvm_stack_push(mvm->stack,
                        (mvm_value_t){.kind = MVM_KIND_STRING, .as.s = (char *)lhs}));
  assert(mvm_stack_push(mvm->stack,
                        (mvm_value_t){.kind = MVM_KIND_STRING, .as.s = (char *)rhs}));
  mvm->pc = 0;
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->stack->data[mvm->stack->size - 1].kind == MVM_KIND_STRING);
  return mvm->stack->data[mvm->stack->size - 1].as.s;
}

static void test_gc_stack_root(void) {
  mvm_t *mvm = create_vm(1);
  mvm->ram->data[0] = MVM_OP_CONCAT;
  char *string = concat_managed_string(mvm, "hello", " world");
  assert(mvm->objects && mvm->gc_bytes > 0);

  assert(mvm_gc_collect(mvm) == 0);
  assert(mvm->gc_collections == 1);
  assert(mvm->stack->data[0].as.s == string);
  assert(strcmp(string, "hello world") == 0);

  mvm_stack_clear(mvm->stack);
  assert(mvm_gc_collect(mvm) == 1);
  assert(!mvm->objects && mvm->gc_bytes == 0);
  assert(mvm_gc_collect(NULL) == 0);
  mvm_destroy(mvm);
}

static void test_gc_global_root(void) {
  const uint8_t program[] = {
      MVM_OP_CONCAT,
      MVM_OP_GLOBAL_SET, 0, 0, 0, 0,
  };
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));
  char *string = concat_managed_string(mvm, "global", " string");
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->stack->size == 0 && mvm->global_count == 1);
  assert(mvm->globals[0].as.s == string);

  assert(mvm_gc_collect(mvm) == 0);
  assert(strcmp(mvm->globals[0].as.s, "global string") == 0);
  mvm->globals[0] = (mvm_value_t){.kind = MVM_KIND_NULL};
  assert(mvm_gc_collect(mvm) == 1);
  assert(!mvm->objects && mvm->gc_bytes == 0);
  mvm_destroy(mvm);
}

static void test_gc_automatic_collection(void) {
  mvm_t *mvm = create_vm(1);
  mvm->ram->data[0] = MVM_OP_CONCAT;
  mvm->gc_next_collection = 1;
  concat_managed_string(mvm, "x", "x");

  for (size_t i = 0; i < 20; ++i) {
    const mvm_value_t current = mvm->stack->data[0];
    assert(mvm_stack_push(mvm->stack, current));
    assert(mvm_stack_push(mvm->stack, current));
    mvm->pc = 0;
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    mvm->stack->data[0] = mvm->stack->data[1];
    assert(mvm_stack_truncate(mvm->stack, 1));
  }

  assert(mvm->gc_collections > 0);
  assert(mvm->stack->size == 1);
  assert(strlen(mvm->stack->data[0].as.s) == (2u << 20));
  assert(mvm_gc_collect(mvm) == 1);
  assert(mvm->objects);
  assert(strlen(mvm->stack->data[0].as.s) == (2u << 20));
  mvm_stack_clear(mvm->stack);
  assert(mvm_gc_collect(mvm) == 1);
  assert(!mvm->objects && mvm->gc_bytes == 0);
  mvm_destroy(mvm);
}

#define DEFINE_NATIVE_IDENTITY(name, type)                                   \
  static type name(type value) { return value; }

DEFINE_NATIVE_IDENTITY(native_identity_bool, bool)
DEFINE_NATIVE_IDENTITY(native_identity_ubyte, uint8_t)
DEFINE_NATIVE_IDENTITY(native_identity_uhalf, uint16_t)
DEFINE_NATIVE_IDENTITY(native_identity_uword, uint32_t)
DEFINE_NATIVE_IDENTITY(native_identity_ulong, uint64_t)
DEFINE_NATIVE_IDENTITY(native_identity_sbyte, int8_t)
DEFINE_NATIVE_IDENTITY(native_identity_shalf, int16_t)
DEFINE_NATIVE_IDENTITY(native_identity_sword, int32_t)
DEFINE_NATIVE_IDENTITY(native_identity_slong, int64_t)
DEFINE_NATIVE_IDENTITY(native_identity_float, float)
DEFINE_NATIVE_IDENTITY(native_identity_double, double)

#undef DEFINE_NATIVE_IDENTITY

static void check_native_identity(void (*function)(void),
                                  const mvm_value_t value) {
  const uint8_t program[] = {MVM_OP_NATIVE, 0, 0, 1};
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));
  const mvm_kind_t arg_kind = value.kind;
  uint16_t id = UINT16_MAX;
  assert(mvm_register_native(mvm, "identity", function, value.kind, 1,
                             &arg_kind, &id) == MVM_RESULT_OK);
  assert(id == 0);
  assert(mvm_stack_push(mvm->stack, value));

  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->pc == sizeof(program));
  assert(mvm->frame_count == 0 && mvm->stack->size == 1);
  assert_numeric_equal(mvm->stack->data[0], value);
  mvm_destroy(mvm);
}

static void test_native_scalar_types(void) {
  check_native_identity((void (*)(void))native_identity_bool,
                        (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = true});
  check_native_identity((void (*)(void))native_identity_ubyte,
                        (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 250});
  check_native_identity(
      (void (*)(void))native_identity_uhalf,
      (mvm_value_t){.kind = MVM_KIND_UHALF, .as.uh = 60000});
  check_native_identity(
      (void (*)(void))native_identity_uword,
      (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 4000000000U});
  check_native_identity(
      (void (*)(void))native_identity_ulong,
      (mvm_value_t){.kind = MVM_KIND_ULONG,
                    .as.ul = UINT64_C(18000000000000000000)});
  check_native_identity((void (*)(void))native_identity_sbyte,
                        (mvm_value_t){.kind = MVM_KIND_SBYTE, .as.sb = -100});
  check_native_identity(
      (void (*)(void))native_identity_shalf,
      (mvm_value_t){.kind = MVM_KIND_SHALF, .as.sh = -30000});
  check_native_identity(
      (void (*)(void))native_identity_sword,
      (mvm_value_t){.kind = MVM_KIND_SWORD, .as.sw = -2000000000});
  check_native_identity(
      (void (*)(void))native_identity_slong,
      (mvm_value_t){.kind = MVM_KIND_SLONG,
                    .as.sl = -INT64_C(9000000000000000000)});
  check_native_identity((void (*)(void))native_identity_float,
                        (mvm_value_t){.kind = MVM_KIND_FLOAT, .as.f = 1.5f});
  check_native_identity((void (*)(void))native_identity_double,
                        (mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = -2.5});
}

static int32_t native_add(int32_t lhs, int32_t rhs) { return lhs + rhs; }
static double native_answer(void) { return 42.5; }
static uint64_t native_string_length(const char *text) {
  return text ? strlen(text) : 0;
}

static void test_native_calls(void) {
  const mvm_kind_t add_args[] = {MVM_KIND_SWORD, MVM_KIND_SWORD};
  const uint8_t add_program[] = {
      MVM_OP_LIT_SWORD, 40, 0, 0, 0, MVM_OP_LIT_SWORD, 2, 0, 0, 0,
      MVM_OP_NATIVE,    0,  0, 2, MVM_OP_RETURN,
  };
  mvm_t *mvm = create_vm(sizeof(add_program));
  memcpy(mvm->ram->data, add_program, sizeof(add_program));
  uint16_t id;
  assert(mvm_register_native(mvm, "add", (void (*)(void))native_add,
                             MVM_KIND_SWORD, 2, add_args,
                             &id) == MVM_RESULT_OK);
  assert(id == 0);
  run(mvm);
  assert(mvm->stack->size == 1 && mvm->frame_count == 0);
  assert_numeric_equal(mvm->stack->data[0],
                       (mvm_value_t){.kind = MVM_KIND_SWORD, .as.sw = 42});
  mvm_destroy(mvm);

  const uint8_t no_args_program[] = {MVM_OP_NATIVE, 0, 0, 0, MVM_OP_RETURN};
  mvm = create_vm(sizeof(no_args_program));
  memcpy(mvm->ram->data, no_args_program, sizeof(no_args_program));
  assert(mvm_register_native(mvm, "answer", (void (*)(void))native_answer,
                             MVM_KIND_DOUBLE, 0, NULL,
                             &id) == MVM_RESULT_OK);
  run(mvm);
  assert(mvm->stack->size == 1);
  assert_numeric_equal(mvm->stack->data[0],
                       (mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = 42.5});
  mvm_destroy(mvm);

  const uint8_t string_program[] = {
      MVM_OP_LIT_STRING, 10, 0, 0, 0, MVM_OP_NATIVE, 0, 0, 1,
      MVM_OP_RETURN,     'h', 'e', 'l', 'l', 'o', 0,
  };
  mvm = create_vm(sizeof(string_program));
  memcpy(mvm->ram->data, string_program, sizeof(string_program));
  const mvm_kind_t string_arg = MVM_KIND_STRING;
  assert(mvm_register_native(mvm, "stringLength",
                             (void (*)(void))native_string_length,
                             MVM_KIND_ULONG, 1, &string_arg,
                             &id) == MVM_RESULT_OK);
  run(mvm);
  assert(mvm->stack->size == 1);
  assert_numeric_equal(mvm->stack->data[0],
                       (mvm_value_t){.kind = MVM_KIND_ULONG, .as.ul = 5});
  mvm_destroy(mvm);
}

static bool native_received_bool;
static mvm_t *native_received_vm;
static void native_write_bool(mvm_t *mvm, bool value) {
  native_received_vm = mvm;
  native_received_bool = value;
}

static void test_native_void_and_reference(void) {
  const uint8_t program[] = {MVM_OP_NATIVE, 0, 0, 2};
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));
  const mvm_kind_t args[] = {MVM_KIND_REFERENCE, MVM_KIND_BOOL};
  assert(mvm_register_native(mvm, "writeBool", (void (*)(void))native_write_bool,
                             MVM_KIND_VOID, 2, args,
                             NULL) == MVM_RESULT_OK);
  assert(mvm_stack_push(mvm->stack, (mvm_value_t){
                                        .kind = MVM_KIND_REFERENCE,
                                        .as.ptr = mvm,
                                    }));
  assert(mvm_stack_push(mvm->stack,
                        (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = true}));

  native_received_vm = NULL;
  native_received_bool = false;
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(native_received_vm == mvm && native_received_bool);
  assert(mvm->pc == sizeof(program) && mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_VOID);
  mvm_destroy(mvm);
}

static double native_fail(mvm_t *mvm, double value) {
  mvm->native_error = MVM_RESULT_RUNTIME_ERROR;
  return value;
}

static void test_native_errors(void) {
  for (size_t available = 0; available < 3; ++available) {
    mvm_t *mvm = create_vm(1 + available);
    memset(mvm->ram->data, 0xFF, mvm->ram->size);
    mvm->ram->data[0] = MVM_OP_NATIVE;
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 1 && mvm->stack->size == 0);
    mvm_destroy(mvm);
  }

  const uint8_t program[] = {MVM_OP_NATIVE, 0, 0, 0};
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->pc == 1 && mvm->stack->size == 0);
  mvm_destroy(mvm);

  mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));
  assert(mvm_register_native(mvm, "answer", (void (*)(void))native_answer,
                             MVM_KIND_DOUBLE, 0, NULL,
                             NULL) == MVM_RESULT_OK);
  mvm_stack_destroy(mvm->stack);
  mvm->stack = NULL;
  assert(mvm_update(mvm) == MVM_RESULT_VM_ERROR);
  mvm_destroy(mvm);

  const uint8_t one_arg_program[] = {MVM_OP_NATIVE, 0, 0, 1};
  mvm = create_vm(sizeof(one_arg_program));
  memcpy(mvm->ram->data, one_arg_program, sizeof(one_arg_program));
  const mvm_kind_t sword_arg = MVM_KIND_SWORD;
  assert(mvm_register_native(mvm, "identity", (void (*)(void))native_identity_sword,
                             MVM_KIND_SWORD, 1, &sword_arg,
                             NULL) == MVM_RESULT_OK);
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->stack->size == 0);
  assert(mvm_stack_push(mvm->stack,
                        (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 42}));
  mvm->pc = MVM_PC_START;
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_UBYTE);
  assert(mvm->stack->data[0].as.ub == 42);
  mvm_destroy(mvm);

  const uint8_t fail_program[] = {MVM_OP_NATIVE, 0, 0, 2};
  mvm = create_vm(sizeof(fail_program));
  memcpy(mvm->ram->data, fail_program, sizeof(fail_program));
  const mvm_kind_t fail_args[] = {MVM_KIND_REFERENCE, MVM_KIND_DOUBLE};
  assert(mvm_register_native(mvm, "fail", (void (*)(void))native_fail,
                             MVM_KIND_DOUBLE, 2, fail_args,
                             NULL) == MVM_RESULT_OK);
  assert(mvm_stack_push(mvm->stack, (mvm_value_t){
                                        .kind = MVM_KIND_REFERENCE,
                                        .as.ptr = mvm,
                                    }));
  assert(mvm_stack_push(mvm->stack,
                        (mvm_value_t){.kind = MVM_KIND_DOUBLE, .as.d = 7.5}));
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->native_error == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->pc == 1 && mvm->stack->size == 2);
  assert(mvm->stack->data[0].kind == MVM_KIND_REFERENCE);
  assert(mvm->stack->data[0].as.ptr == mvm);
  assert(mvm->stack->data[1].kind == MVM_KIND_DOUBLE);
  assert(mvm->stack->data[1].as.d == 7.5);
  mvm_destroy(mvm);

  mvm = create_vm(0);
  const mvm_kind_t void_arg = MVM_KIND_VOID;
  assert(mvm_register_native(mvm, "invalid", (void (*)(void))native_answer,
                             MVM_KIND_DOUBLE, 1, &void_arg,
                             NULL) == MVM_RESULT_VM_ERROR);
  assert(mvm->native_count == 0);
  mvm_destroy(mvm);
}

static mvm_result_t native_null_string(mvm_t *mvm, const mvm_value_t *args,
                                       mvm_value_t *result) {
  (void)mvm;
  (void)args;
  *result = (mvm_value_t){.kind = MVM_KIND_NULL};
  return MVM_RESULT_OK;
}

static mvm_result_t native_empty_string(mvm_t *mvm, const mvm_value_t *args,
                                        mvm_value_t *result) {
  (void)mvm;
  (void)args;
  *result = (mvm_value_t){.kind = MVM_KIND_STRING, .as.s = ""};
  return MVM_RESULT_OK;
}

static void check_native_string_result(uint32_t flags, mvm_builtin_fn function,
                                       mvm_result_t expected, bool empty) {
  const uint8_t program[] = {MVM_OP_NATIVE, 0, 0, 0, MVM_OP_RETURN};
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));
  const mvm_type_desc_t desc = {.size = 8,
                                .alignment = 8,
                                .tag = MVM_TYPE_PRIMITIVE,
                                .kind = MVM_KIND_STRING,
                                .flags = flags};
  uint32_t type;
  assert(mvm_type_register(mvm, &desc, &type) == MVM_RESULT_OK);
  uint16_t id;
  assert(mvm_register_builtin(mvm, "stringResult", function, MVM_KIND_STRING,
                              0, NULL, &id) == MVM_RESULT_OK);
  assert(mvm_native_bind_signature(mvm, id, type, NULL) == MVM_RESULT_OK);
  assert(mvm_update(mvm) == expected);
  if (expected == MVM_RESULT_OK) {
    assert(mvm->stack->size == 1);
    if (empty) {
      assert(mvm->stack->data[0].kind == MVM_KIND_STRING);
      assert(!strcmp(mvm->stack->data[0].as.s, ""));
    } else {
      assert(mvm->stack->data[0].kind == MVM_KIND_NULL);
    }
  }
  mvm_destroy(mvm);
}

static void test_native_string_results(void) {
  check_native_string_result(0, native_null_string, MVM_RESULT_RUNTIME_ERROR,
                             false);
  check_native_string_result(MVM_TYPE_FLAG_NULLABLE, native_null_string,
                             MVM_RESULT_OK, false);
  check_native_string_result(0, native_empty_string, MVM_RESULT_OK, true);
}

static void test_native_library(void) {
  mvm_t *mvm = create_vm(0);
  assert(mvm_load_native_library(NULL, MVM_TEST_LIBRARY_PATH) == MVM_RESULT_VM_ERROR);
  assert(mvm_load_native_library(mvm, NULL) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm_load_native_library(mvm, "") == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm_load_native_library(mvm, MVM_TEST_LIBRARY_PATH ".missing") == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm_load_native_library(mvm, MVM_INVALID_ABI_LIBRARY_PATH) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm_load_native_library(mvm, MVM_INVALID_EXPORTS_LIBRARY_PATH) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->native_count == 0 && !mvm->native_libraries);
  uint16_t rejected_id;
  assert(!mvm_find_native(mvm, "nativeTestAnswer", &rejected_id));
  assert(!mvm_find_native(mvm, "invalidArgument", &rejected_id));

  assert(mvm_load_native_library(mvm, MVM_TEST_LIBRARY_PATH) == MVM_RESULT_OK);
  assert(mvm->native_count == 1 && mvm->native_libraries);
  mvm_native_library_t *const library = mvm->native_libraries;
  assert(mvm_load_native_library(mvm, MVM_INVALID_EXPORTS_LIBRARY_PATH) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->native_count == 1 && mvm->native_libraries == library);

  uint16_t answer_id;
  assert(mvm_find_native(mvm, "nativeTestAnswer", &answer_id));
  assert(!mvm_find_native(mvm, "missing", &rejected_id));

  uint8_t program[] = {
      MVM_OP_NATIVE, 0, 0, 0, MVM_OP_RETURN,
  };
  program[1] = (uint8_t)answer_id;
  program[2] = (uint8_t)(answer_id >> 8);
  mvm_ram_destroy(mvm->ram);
  mvm->ram = mvm_ram_create(sizeof(program));
  assert(mvm->ram);
  memcpy(mvm->ram->data, program, sizeof(program));

  run(mvm);
  assert(mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_SLONG);
  assert(mvm->stack->data[0].as.sl == 42);
  mvm_destroy(mvm);
}

static void test_native_library_dispose(void) {
  mvm_t *vm = create_vm(0);
  uint16_t deferred;
  uint16_t original;
  uint16_t current;
  assert(mvm_dispose_native_library(NULL, MVM_TEST_LIBRARY_PATH) ==
         MVM_RESULT_VM_ERROR);
  assert(mvm_dispose_native_library(vm, NULL) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm_dispose_native_library(vm, "") == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm_dispose_native_library(vm, MVM_TEST_LIBRARY_PATH) ==
         MVM_RESULT_RUNTIME_ERROR);
  assert(mvm_native_defer(vm, "nativeTestAnswer", MVM_KIND_SLONG, 0, NULL,
                          false, &deferred) == MVM_RESULT_OK);
  assert(mvm_load_native_library(vm, MVM_TEST_LIBRARY_PATH) == MVM_RESULT_OK);
  original = (uint16_t)(vm->native_count - 1);
  assert(mvm_execute_native(vm, deferred, 0) == MVM_RESULT_OK);
  assert(vm->stack->data[0].as.sl == 42);
  mvm_stack_clear(vm->stack);

  // Each clone owns its list; disposing one must preserve the other's handle.
  mvm_t *clone = create_vm(0);
  assert(mvm_native_clone(clone, vm) == MVM_RESULT_OK);
  assert(mvm_dispose_native_library(vm, MVM_TEST_LIBRARY_PATH) ==
         MVM_RESULT_OK);
  assert(!vm->native_libraries);
  assert(mvm_execute_native(vm, original, 0) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm_execute_native(vm, deferred, 0) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm_execute_native(clone, original, 0) == MVM_RESULT_OK);
  assert(clone->stack->data[0].as.sl == 42);
  mvm_destroy(vm);
  mvm_stack_clear(clone->stack);
  assert(mvm_execute_native(clone, deferred, 0) == MVM_RESULT_OK);
  assert(clone->stack->data[0].as.sl == 42);
  mvm_stack_clear(clone->stack);
  assert(mvm_dispose_native_library(clone, MVM_TEST_LIBRARY_PATH) ==
         MVM_RESULT_OK);
  assert(mvm_dispose_native_library(clone, MVM_TEST_LIBRARY_PATH) ==
         MVM_RESULT_RUNTIME_ERROR);
  assert(mvm_execute_native(clone, original, 0) == MVM_RESULT_RUNTIME_ERROR);

  // Reloading resolves deferred imports without reviving disposed numeric IDs.
  assert(mvm_load_native_library(clone, MVM_TEST_LIBRARY_PATH) ==
         MVM_RESULT_OK);
  assert(mvm_execute_native(clone, deferred, 0) == MVM_RESULT_OK);
  assert(clone->stack->data[0].as.sl == 42);
  mvm_stack_clear(clone->stack);
  assert(mvm_execute_native(clone, original, 0) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm_load_native_library(clone, MVM_TEST_LIBRARY_PATH) ==
         MVM_RESULT_OK);
  assert(mvm_dispose_native_library(clone, MVM_TEST_LIBRARY_PATH) ==
         MVM_RESULT_OK);
  assert(mvm_execute_native(clone, deferred, 0) == MVM_RESULT_OK);
  mvm_stack_clear(clone->stack);
  assert(mvm_dispose_native_library(clone, MVM_TEST_LIBRARY_PATH) ==
         MVM_RESULT_OK);
  assert(mvm_execute_native(clone, deferred, 0) == MVM_RESULT_RUNTIME_ERROR);
  // A disposed export is no longer discoverable by name.
  mvm_t *plain = create_vm(0);
  assert(mvm_load_native_library(plain, MVM_TEST_LIBRARY_PATH) ==
         MVM_RESULT_OK);
  assert(mvm_find_native(plain, "nativeTestAnswer", &current));
  assert(mvm_dispose_native_library(plain, MVM_TEST_LIBRARY_PATH) ==
         MVM_RESULT_OK);
  assert(!mvm_find_native(plain, "nativeTestAnswer", &current));
  mvm_destroy(plain);
  mvm_destroy(clone);
}

static void check_ram_scalar(mvm_value_t value, size_t width, uint64_t bits,
                             size_t address) {
  mvm_t *mvm = create_vm(24);
  memset(mvm->ram->data, 0xA5, mvm->ram->size);
  mvm->ram->data[0] = MVM_OP_STORE;
  mvm->ram->data[1] = (uint8_t)value.kind;
  const mvm_value_t sentinel = {.kind = MVM_KIND_NULL};
  const mvm_value_t offset = {.kind = MVM_KIND_ULONG, .as.ul = address};
  assert(mvm_stack_push(mvm->stack, sentinel));
  assert(mvm_stack_push(mvm->stack, offset));
  assert(mvm_stack_push(mvm->stack, value));
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->pc == 2 && mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_NULL);
  for (size_t i = 2; i < mvm->ram->size; ++i) {
    const uint8_t expected = i >= address && i - address < width
                                 ? (uint8_t)(bits >> ((i - address) * 8))
                                 : 0xA5;
    assert(mvm->ram->data[i] == expected);
  }

  // Read independent fixture bytes so a matching STORE/LOAD bug cannot pass.
  for (size_t i = 0; i < width; ++i)
    mvm->ram->data[address + i] = (uint8_t)(bits >> (i * 8));
  mvm->ram->data[0] = MVM_OP_LOAD;
  uint8_t snapshot[24];
  memcpy(snapshot, mvm->ram->data, sizeof(snapshot));
  mvm->pc = 0;
  assert(mvm_stack_push(mvm->stack, offset));
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->pc == 2 && mvm->stack->size == 2);
  const mvm_value_t actual = mvm->stack->data[1];
  assert(actual.kind == value.kind);
  if (value.kind == MVM_KIND_BOOL) {
    assert(actual.as.b == value.as.b);
  } else if (value.kind == MVM_KIND_FLOAT) {
    uint32_t actual_bits;
    memcpy(&actual_bits, &actual.as.f, sizeof(actual_bits));
    assert(actual_bits == (uint32_t)bits);
  } else if (value.kind == MVM_KIND_DOUBLE) {
    uint64_t actual_bits;
    memcpy(&actual_bits, &actual.as.d, sizeof(actual_bits));
    assert(actual_bits == bits);
  } else {
    assert_numeric_equal(actual, value);
  }
  assert(memcmp(snapshot, mvm->ram->data, sizeof(snapshot)) == 0);
  mvm_destroy(mvm);
}

static void test_ram_scalars(void) {
  const struct {
    mvm_value_t value;
    size_t width;
    uint64_t bits;
  } cases[] = {
      {{.kind = MVM_KIND_BOOL, .as.b = false}, 1, 0},
      {{.kind = MVM_KIND_BOOL, .as.b = true}, 1, 1},
      {{.kind = MVM_KIND_UBYTE, .as.ub = UINT8_MAX}, 1, UINT8_MAX},
      {{.kind = MVM_KIND_UHALF, .as.uh = 0xABCD}, 2, 0xABCD},
      {{.kind = MVM_KIND_UWORD, .as.uw = 0x89ABCDEF}, 4, 0x89ABCDEF},
      {{.kind = MVM_KIND_ULONG, .as.ul = UINT64_C(0xFEDCBA9876543210)},
       8, UINT64_C(0xFEDCBA9876543210)},
      {{.kind = MVM_KIND_SBYTE, .as.sb = INT8_MIN}, 1, 0x80},
      {{.kind = MVM_KIND_SHALF, .as.sh = INT16_MIN}, 2, 0x8000},
      {{.kind = MVM_KIND_SWORD, .as.sw = INT32_MIN}, 4, 0x80000000},
      {{.kind = MVM_KIND_SLONG, .as.sl = INT64_MIN},
       8, UINT64_C(0x8000000000000000)},
      {{.kind = MVM_KIND_SLONG, .as.sl = -1}, 8, UINT64_MAX},
      {{.kind = MVM_KIND_FLOAT, .as.f = 1.5f}, 4, 0x3FC00000},
      {{.kind = MVM_KIND_DOUBLE, .as.d = -2.5},
       8, UINT64_C(0xC004000000000000)},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    check_ram_scalar(cases[i].value, cases[i].width, cases[i].bits, 9);
    check_ram_scalar(cases[i].value, cases[i].width, cases[i].bits,
                     24 - cases[i].width);
  }
  const uint64_t bit_cases[] = {
      0, UINT64_C(0x8000000000000000), 1, UINT64_C(0x7FF0000000000000),
      UINT64_C(0xFFF0000000000000), UINT64_C(0x7FF8000000001234),
  };
  const uint32_t float_cases[] = {0, 0x80000000, 1, 0x7F800000,
                                 0xFF800000, 0x7FC01234};
  for (size_t i = 0; i < sizeof(bit_cases) / sizeof(bit_cases[0]); ++i) {
    mvm_value_t value = {.kind = MVM_KIND_DOUBLE};
    memcpy(&value.as.d, &bit_cases[i], sizeof(bit_cases[i]));
    check_ram_scalar(value, 8, bit_cases[i], 9);
    value.kind = MVM_KIND_FLOAT;
    memcpy(&value.as.f, &float_cases[i], sizeof(float_cases[i]));
    check_ram_scalar(value, 4, float_cases[i], 9);
  }
}

static void check_ram_error(mvm_opcodes_t op, uint8_t kind,
                             const mvm_value_t *args, size_t count) {
  mvm_t *mvm = create_vm(16);
  memset(mvm->ram->data, 0xA5, mvm->ram->size);
  mvm->ram->data[0] = op;
  mvm->ram->data[1] = kind;
  uint8_t snapshot[16];
  memcpy(snapshot, mvm->ram->data, sizeof(snapshot));
  for (size_t i = 0; i < count; ++i)
    assert(mvm_stack_push(mvm->stack, args[i]));
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->pc == 1 && mvm->stack->size == count && mvm->working);
  assert(mvm->frame_count == 0);
  for (size_t i = 0; i < count; ++i)
    assert(memcmp(&mvm->stack->data[i], &args[i], sizeof(args[i])) == 0);
  assert(memcmp(snapshot, mvm->ram->data, sizeof(snapshot)) == 0);
  mvm_destroy(mvm);
}

static void test_ram_errors(void) {
  const mvm_opcodes_t ops[] = {MVM_OP_LOAD, MVM_OP_STORE};
  const mvm_value_t bad_addresses[] = {
      {.kind = MVM_KIND_UBYTE, .as.ub = 16},
      {.kind = MVM_KIND_UHALF, .as.uh = 17},
      {.kind = MVM_KIND_ULONG, .as.ul = UINT64_MAX},
      {.kind = MVM_KIND_SBYTE, .as.sb = -1},
      {.kind = MVM_KIND_SHALF, .as.sh = -1},
      {.kind = MVM_KIND_SWORD, .as.sw = -1},
      {.kind = MVM_KIND_SLONG, .as.sl = INT64_MIN},
      {.kind = MVM_KIND_BOOL, .as.b = true},
      {.kind = MVM_KIND_FLOAT, .as.f = 8},
      {.kind = MVM_KIND_DOUBLE, .as.d = 8},
      {.kind = MVM_KIND_NULL}, {.kind = MVM_KIND_VOID},
      {.kind = MVM_KIND_REFERENCE}, {.kind = MVM_KIND_STRING},
      {.kind = MVM_KIND_ARRAY}, {.kind = MVM_KIND_TYPE},
  };
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) {
    const size_t count = ops[i] == MVM_OP_STORE ? 2 : 1;
    mvm_value_t args[] = {{.kind = MVM_KIND_UWORD, .as.uw = 8},
                          {.kind = MVM_KIND_UBYTE, .as.ub = 42}};
    for (size_t j = 0; j < sizeof(bad_addresses) / sizeof(bad_addresses[0]); ++j) {
      args[0] = bad_addresses[j];
      check_ram_error(ops[i], MVM_KIND_UBYTE, args, count);
    }
    args[0] = (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 8};
    for (unsigned kind = 0; kind <= UINT8_MAX; ++kind) {
      const bool scalar = kind >= MVM_KIND_BOOL && kind <= MVM_KIND_DOUBLE;
      if (!scalar)
        check_ram_error(ops[i], (uint8_t)kind, args, count);
    }
    for (size_t available = 0; available < count; ++available)
      check_ram_error(ops[i], MVM_KIND_UBYTE, args, available);

    const mvm_kind_t kinds[] = {MVM_KIND_UHALF, MVM_KIND_UWORD, MVM_KIND_ULONG,
                                MVM_KIND_SHALF, MVM_KIND_SWORD, MVM_KIND_SLONG,
                                MVM_KIND_FLOAT, MVM_KIND_DOUBLE};
    for (size_t j = 0; j < sizeof(kinds) / sizeof(kinds[0]); ++j) {
      args[0].as.uw = 15; // First byte fits, complete payload does not.
      args[1] = (mvm_value_t){.kind = kinds[j]};
      check_ram_error(ops[i], (uint8_t)kinds[j], args, count);
    }
    // BOOL must have the canonical byte representation 0 or 1.
    args[0].as.uw = 8;
    if (ops[i] == MVM_OP_LOAD)
      check_ram_error(ops[i], MVM_KIND_BOOL, args, count);
    else {
      for (unsigned kind = 0; kind <= MVM_KIND_COUNT; ++kind) {
        if (kind == MVM_KIND_UBYTE)
          continue;
        args[1] = (mvm_value_t){.kind = (mvm_kind_t)kind};
        check_ram_error(ops[i], MVM_KIND_UBYTE, args, count);
      }
    }
    mvm_t *mvm = create_vm(1);
    mvm->ram->data[0] = ops[i]; // Missing kind operand.
    assert(mvm_stack_push(mvm->stack, args[0]));
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 1 && mvm->stack->size == 1);
    mvm_destroy(mvm);

    mvm = create_vm(2);
    mvm->ram->data[0] = ops[i];
    mvm->ram->data[1] = MVM_KIND_UBYTE;
    mvm_stack_t *stack = mvm->stack;
    mvm->stack = NULL;
    assert(mvm_update(mvm) == MVM_RESULT_VM_ERROR);
    assert(mvm->pc == 1);
    mvm->stack = stack;
    mvm_destroy(mvm);
  }
}

static void test_ram_addresses_and_frames(void) {
  const mvm_value_t addresses[] = {
      {.kind = MVM_KIND_UBYTE, .as.ub = 15},
      {.kind = MVM_KIND_UHALF, .as.uh = 15},
      {.kind = MVM_KIND_UWORD, .as.uw = 15},
      {.kind = MVM_KIND_ULONG, .as.ul = 15},
      {.kind = MVM_KIND_SBYTE, .as.sb = 15},
      {.kind = MVM_KIND_SHALF, .as.sh = 15},
      {.kind = MVM_KIND_SWORD, .as.sw = 15},
      {.kind = MVM_KIND_SLONG, .as.sl = 15},
  };
  for (size_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); ++i) {
    mvm_t *mvm = create_vm(16);
    const uint8_t code[] = {MVM_OP_STORE, MVM_KIND_UBYTE,
                            MVM_OP_LOAD, MVM_KIND_UBYTE};
    memcpy(mvm->ram->data, code, sizeof(code));
    assert(mvm_stack_push(mvm->stack, addresses[i]));
    assert(mvm_stack_push(mvm->stack,
                          (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 42}));
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm->stack->size == 0 && mvm->ram->data[15] == 42);
    assert(mvm_stack_push(mvm->stack, addresses[i]));
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm->stack->size == 1 && mvm->stack->data[0].as.ub == 42);
    mvm_destroy(mvm);
  }

  const mvm_opcodes_t ops[] = {MVM_OP_LOAD, MVM_OP_STORE};
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) {
    const uint8_t program[] = {MVM_OP_CALL, 6, 0, 0, 0, 0,
                               ops[i], MVM_KIND_UBYTE};
    for (size_t available = 0; available < (i == 0 ? 1u : 2u); ++available) {
      mvm_t *mvm = create_vm(sizeof(program));
      memcpy(mvm->ram->data, program, sizeof(program));
      assert(mvm_stack_push(mvm->stack,
                            (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 0}));
      assert(mvm_update(mvm) == MVM_RESULT_OK);
      if (available)
        assert(mvm_stack_push(mvm->stack,
                              (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 0}));
      assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
      assert(mvm->pc == 7 && mvm->stack->size == 1 + available);
      assert(mvm->frame_count == 1 && mvm->frames[0].base == 1);
      assert(memcmp(mvm->ram->data, program, sizeof(program)) == 0);
      mvm_destroy(mvm);
    }
  }
}

static void test_ram_program(void) {
  const uint8_t program[] = {
      MVM_OP_LIT_UWORD, 31, 0, 0, 0,
      MVM_OP_LIT_SWORD, 0xFE, 0xFF, 0xFF, 0xFF,
      MVM_OP_STORE, MVM_KIND_SWORD,
      MVM_OP_LIT_UWORD, 31, 0, 0, 0,
      MVM_OP_LOAD, MVM_KIND_SWORD,
      MVM_OP_LIT_SWORD, 44, 0, 0, 0,
      MVM_OP_ADD, MVM_OP_RETURN,
  };
  mvm_t *mvm = create_vm(35);
  memcpy(mvm->ram->data, program, sizeof(program));
  while (mvm->working)
    assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->pc == sizeof(program) && mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_SWORD);
  assert(mvm->stack->data[0].as.sw == 42);
  assert(mvm_ram_peek_sword(mvm->ram, 31) == -2);
  mvm_destroy(mvm);
}

static void test_ram_code_access(void) {
  mvm_t *mvm = create_vm(4);
  const uint8_t code[] = {MVM_OP_STORE, MVM_KIND_UBYTE,
                          MVM_OP_LOAD, MVM_KIND_UBYTE};
  memcpy(mvm->ram->data, code, sizeof(code));
  // Overwrite STORE's own kind operand after it has been decoded.
  assert(mvm_stack_push(mvm->stack,
                        (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 1}));
  assert(mvm_stack_push(mvm->stack,
                        (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 0xFF}));
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->pc == 2 && mvm->stack->size == 0);
  assert(mvm->ram->data[1] == 0xFF);
  // Byte offset zero is a valid data address even though it contains code.
  assert(mvm_stack_push(mvm->stack,
                        (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 0}));
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->pc == 4 && mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_UBYTE);
  assert(mvm->stack->data[0].as.ub == MVM_OP_STORE);
  mvm_destroy(mvm);
}

static uint32_t native_ram_string_length(const char *text) {
  return (uint32_t)strlen(text);
}

static void test_ram_modified_strings(void) {
  const mvm_opcodes_t ops[] = {MVM_OP_STRING_LEN, MVM_OP_STRING_GET,
                              MVM_OP_CONCAT, MVM_OP_EQ, MVM_OP_NE, MVM_OP_NATIVE};
  // Remove the terminator, or replace the character with invalid UTF-8.
  for (size_t mutation = 0; mutation < 2; ++mutation) {
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) {
      mvm_t *mvm = create_vm(34);
      const uint8_t program[] = {MVM_OP_LIT_STRING, 32, 0, 0, 0,
                                 MVM_OP_STORE, MVM_KIND_UBYTE, ops[i], 0, 0, 1};
      memcpy(mvm->ram->data, program, sizeof(program));
      mvm->ram->data[32] = 'a';
      const mvm_kind_t string_kind = MVM_KIND_STRING;
      assert(mvm_register_native(mvm, "ramStringLength",
                                 (void (*)(void))native_ram_string_length,
                                 MVM_KIND_UWORD, 1, &string_kind, NULL) == MVM_RESULT_OK);
      assert(mvm_update(mvm) == MVM_RESULT_OK);
      assert(mvm_stack_push(mvm->stack,
                            (mvm_value_t){.kind = MVM_KIND_UWORD,
                                          .as.uw = mutation ? 32 : 33}));
      assert(mvm_stack_push(mvm->stack,
                            (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 0xFF}));
      assert(mvm_update(mvm) == MVM_RESULT_OK);
      if (ops[i] == MVM_OP_STRING_GET) {
        assert(mvm_stack_push(mvm->stack,
                              (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 0}));
      } else if (ops[i] == MVM_OP_CONCAT || ops[i] == MVM_OP_EQ || ops[i] == MVM_OP_NE) {
        assert(mvm_stack_push(mvm->stack,
                              (mvm_value_t){.kind = MVM_KIND_STRING, .as.s = "a"}));
      }
      const size_t count = mvm->stack->size;
      assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
      assert(mvm->pc == 8 && mvm->stack->size == count);
      mvm_destroy(mvm);
    }
  }
}

static void test_function_values(void) {
  // A factory returns a function value; store it in a global and call it.
  const uint8_t program[] = {
      MVM_OP_LIT_UBYTE, 99,             // 0: caller local
      MVM_OP_LIT_UBYTE, 20,             // 2: first argument
      MVM_OP_LIT_UBYTE, 22,             // 4: second argument
      MVM_OP_CALL, 25, 0, 0, 0, 0,     // 6: factory()
      MVM_OP_GLOBAL_SET, 0, 0, 0, 0,   // 12
      MVM_OP_GLOBAL_GET, 0, 0, 0, 0,   // 17
      MVM_OP_CALL_INDIRECT, 2,          // 22: add(20, 22)
      MVM_OP_RETURN,                    // 24
      MVM_OP_LIT_FUNC, 31, 0, 0, 0,    // 25: factory
      MVM_OP_RETURN,                    // 30
      MVM_OP_LOCAL_GET, 0, 0,           // 31: add
      MVM_OP_LOCAL_GET, 1, 0,           // 34
      MVM_OP_ADD,                       // 37
      MVM_OP_RETURN,                    // 38
  };
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));
  while (mvm->pc != 31)
    assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->frame_count == 1 && mvm->frames[0].base == 1);
  assert(mvm->frames[0].return_pc == 24 && mvm->stack->size == 3);
  assert(mvm->globals[0].kind == MVM_KIND_FUNC);
  assert(mvm->globals[0].as.func == 31);
  run(mvm);
  assert(mvm->frame_count == 0 && mvm->stack->size == 2);
  assert(mvm->stack->data[0].as.ub == 99);
  assert(mvm->stack->data[1].kind == MVM_KIND_UBYTE);
  assert(mvm->stack->data[1].as.ub == 42);
  mvm_destroy(mvm);

  const mvm_value_t a = {.kind = MVM_KIND_FUNC, .as.func = 0};
  const mvm_value_t b = {.kind = MVM_KIND_FUNC, .as.func = 31};
  check_equal(a, a, true);
  check_equal(a, b, false);
  check_comparison(MVM_OP_NE, a, b, true);
  check_equal(a, (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 0}, false);
  mvm = create_vm(1);
  mvm->ram->data[0] = MVM_OP_LT;
  assert(mvm_stack_push(mvm->stack, a));
  assert(mvm_stack_push(mvm->stack, b));
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->pc == 1 && mvm->stack->size == 2);
  assert(mvm->stack->data[0].as.func == 0);
  assert(mvm->stack->data[1].as.func == 31);
  mvm_destroy(mvm);
}

static void test_indirect_recursion(void) {
  const uint8_t program[] = {
      MVM_OP_LIT_FUNC, 20, 0, 0, 0,    // 0
      MVM_OP_GLOBAL_SET, 0, 0, 0, 0,   // 5
      MVM_OP_LIT_UBYTE, 20,             // 10
      MVM_OP_GLOBAL_GET, 0, 0, 0, 0,   // 12
      MVM_OP_CALL_INDIRECT, 1,          // 17
      MVM_OP_RETURN,                    // 19
      MVM_OP_LOCAL_GET, 0, 0,           // 20: countdown(n)
      MVM_OP_LIT_UBYTE, 0,              // 23
      MVM_OP_EQ,                        // 25
      MVM_OP_JUMP_IF_TRUE, 43, 0, 0, 0, // 26
      MVM_OP_LOCAL_GET, 0, 0,           // 31
      MVM_OP_DEC,                       // 34
      MVM_OP_GLOBAL_GET, 0, 0, 0, 0,   // 35
      MVM_OP_CALL_INDIRECT, 1,          // 40
      MVM_OP_RETURN,                    // 42
      MVM_OP_LIT_UBYTE, 42,             // 43
      MVM_OP_RETURN,                    // 45
  };
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));
  for (size_t steps = 0; mvm->frame_count < 21; ++steps) {
    assert(steps < 200);
    assert(mvm_update(mvm) == MVM_RESULT_OK);
  }
  assert(mvm->frame_capacity >= 21);
  run(mvm);
  assert(mvm->frame_count == 0 && mvm->stack->size == 1);
  assert(mvm->stack->data[0].as.ub == 42);
  mvm_destroy(mvm);
}

static void test_indirect_zero_args(void) {
  // Address zero is usable, and a zero-argument call removes its function slot.
  const uint8_t program[] = {
      MVM_OP_LIT_NULL,                 // 0: void function
      MVM_OP_RETURN,                   // 1
      MVM_OP_LIT_FUNC, 0, 0, 0, 0,    // 2: entry
      MVM_OP_CALL_INDIRECT, 0,         // 7
      MVM_OP_RETURN,                   // 9
  };
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));
  mvm->pc = 2;
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm->pc == 0 && mvm->frame_count == 1);
  assert(mvm->stack->size == 0 && mvm->frames[0].base == 0);
  run(mvm);
  assert(mvm->frame_count == 0 && mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_NULL);
  mvm_destroy(mvm);
}

static void test_function_array(void) {
  const uint8_t program[] = {
      MVM_OP_ARRAY_GET,                // 0: function table and index from host
      MVM_OP_CALL_INDIRECT, 0,         // 1
      MVM_OP_RETURN,                   // 3
      MVM_OP_LIT_UBYTE, 42,            // 4: function
      MVM_OP_RETURN,                   // 6
  };
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));
  mvm_value_t table, value;
  assert(mvm_array_new(mvm, MVM_KIND_FUNC, 2, &table) == MVM_RESULT_OK);
  assert(mvm_stack_push(mvm->stack, table));
  assert(mvm_array_get(mvm, table, 0, &value) == MVM_RESULT_OK);
  assert(value.kind == MVM_KIND_NULL);
  const mvm_value_t function = {.kind = MVM_KIND_FUNC, .as.func = 4};
  assert(mvm_array_set(mvm, table, 1, function) == MVM_RESULT_OK);
  assert(mvm_array_set(mvm, table, 0, function) == MVM_RESULT_OK);
  assert(mvm_array_set(mvm, table, 0, (mvm_value_t){.kind = MVM_KIND_NULL}) ==
         MVM_RESULT_OK);
  assert(mvm_array_set(mvm, table, 1,
                       (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = 4}) ==
         MVM_RESULT_RUNTIME_ERROR);
  assert(mvm_gc_collect(mvm) == 0);
  assert(mvm_stack_push(mvm->stack,
                        (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = 1}));
  run(mvm);
  assert(mvm->frame_count == 0 && mvm->stack->size == 1);
  assert(mvm->stack->data[0].as.ub == 42);
  assert(mvm_gc_collect(mvm) == 1);
  mvm_destroy(mvm);
}

static void test_function_argument_gc(void) {
  // apply(fn, array) calls its function argument. Allocation inside that
  // function collects while both caller locals and callee arguments are live.
  const uint8_t program[] = {
      MVM_OP_CALL, 7, 0, 0, 0, 2,     // 0: apply(fn, array)
      MVM_OP_RETURN,                   // 6
      MVM_OP_LOCAL_GET, 1, 0,          // 7: array
      MVM_OP_LOCAL_GET, 0, 0,          // 10: fn
      MVM_OP_CALL_INDIRECT, 1,         // 13
      MVM_OP_RETURN,                   // 15
      MVM_OP_LIT_UBYTE, 1,             // 16: callback
      MVM_OP_ALLOC,                    // 18
      MVM_OP_POP,                      // 19
      MVM_OP_LOCAL_GET, 0, 0,          // 20
      MVM_OP_ARRAY_LEN,                // 23
      MVM_OP_RETURN,                   // 24
  };
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));
  assert(mvm_stack_push(mvm->stack,
                        (mvm_value_t){.kind = MVM_KIND_FUNC, .as.func = 16}));
  mvm_value_t array;
  assert(mvm_array_new(mvm, MVM_KIND_NULL, 42, &array) == MVM_RESULT_OK);
  assert(mvm_stack_push(mvm->stack, array));
  mvm->gc_next_collection = 0;
  run(mvm);
  assert(mvm->gc_collections > 0);
  assert(mvm->frame_count == 0 && mvm->stack->size == 1);
  assert(mvm->stack->data[0].kind == MVM_KIND_UWORD);
  assert(mvm->stack->data[0].as.uw == 42);
  assert(mvm_gc_collect(mvm) == 2); // array and temporary raw object
  mvm_destroy(mvm);
}

static void test_indirect_max_args(void) {
  const uint8_t program[] = {
      MVM_OP_CALL_INDIRECT, UINT8_MAX, // 0
      MVM_OP_RETURN,                  // 2
      MVM_OP_LOCAL_GET, 254, 0,        // 3: return the last argument
      MVM_OP_RETURN,                  // 6
  };
  mvm_t *mvm = create_vm(sizeof(program));
  memcpy(mvm->ram->data, program, sizeof(program));
  assert(mvm_stack_push(mvm->stack, (mvm_value_t){.kind = MVM_KIND_NULL}));
  for (size_t i = 0; i < UINT8_MAX; ++i)
    assert(mvm_stack_push(mvm->stack,
                          (mvm_value_t){.kind = MVM_KIND_UBYTE, .as.ub = (uint8_t)i}));
  assert(mvm_stack_push(mvm->stack,
                        (mvm_value_t){.kind = MVM_KIND_FUNC, .as.func = 3}));
  run(mvm);
  assert(mvm->frame_count == 0 && mvm->stack->size == 2);
  assert(mvm->stack->data[0].kind == MVM_KIND_NULL);
  assert(mvm->stack->data[1].kind == MVM_KIND_UBYTE);
  assert(mvm->stack->data[1].as.ub == 254);
  mvm_destroy(mvm);
}

static void test_indirect_call_errors(void) {
  const mvm_value_t invalid[] = {
      {.kind = MVM_KIND_NULL},
      {.kind = MVM_KIND_UWORD, .as.uw = 0},
      {.kind = MVM_KIND_REFERENCE},
      {.kind = MVM_KIND_FUNC, .as.func = 2}, // exactly past RAM
      {.kind = MVM_KIND_FUNC, .as.func = UINT32_MAX},
  };
  for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
    mvm_t *mvm = create_vm(2);
    mvm->ram->data[0] = MVM_OP_CALL_INDIRECT;
    assert(mvm_stack_push(mvm->stack, invalid[i]));
    mvm_value_t before = mvm->stack->data[0];
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 1 && mvm->working && mvm->frame_count == 0);
    assert(mvm->stack->size == 1);
    assert(!memcmp(&before, mvm->stack->data, sizeof(before)));
    mvm_destroy(mvm);
  }

  // Missing function or arguments; also cover the maximum argc byte.
  const uint8_t argcs[] = {0, 1, UINT8_MAX};
  for (size_t i = 0; i < sizeof(argcs); ++i) {
    mvm_t *mvm = create_vm(2);
    mvm->ram->data[0] = MVM_OP_CALL_INDIRECT;
    mvm->ram->data[1] = argcs[i];
    if (argcs[i])
      assert(mvm_stack_push(mvm->stack,
                            (mvm_value_t){.kind = MVM_KIND_FUNC, .as.func = 0}));
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 1 && mvm->frame_count == 0);
    assert(mvm->stack->size == (argcs[i] ? 1u : 0u));
    mvm_destroy(mvm);
  }

  // Neither function nor arguments can be borrowed from the caller's locals.
  for (size_t local_function = 0; local_function < 2; ++local_function) {
    const uint8_t program[] = {MVM_OP_CALL, 6, 0, 0, 0, 0,
                               MVM_OP_CALL_INDIRECT, 1};
    mvm_t *mvm = create_vm(sizeof(program));
    memcpy(mvm->ram->data, program, sizeof(program));
    assert(mvm_stack_push(mvm->stack,
                          (mvm_value_t){.kind = MVM_KIND_FUNC, .as.func = 0}));
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    if (!local_function)
      assert(mvm_stack_push(mvm->stack, mvm->stack->data[0]));
    const size_t count = mvm->stack->size;
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 7 && mvm->frame_count == 1);
    assert(mvm->frames[0].base == 1 && mvm->stack->size == count);
    mvm_destroy(mvm);
  }

  // Overflow must preserve the function value, including with a zero limit.
  const uint8_t recursive[] = {MVM_OP_LIT_FUNC, 0, 0, 0, 0,
                                MVM_OP_CALL_INDIRECT, 0};
  const size_t limits[] = {0, 16};
  for (size_t i = 0; i < sizeof(limits) / sizeof(*limits); ++i) {
    mvm_t *mvm = test_vm_create(NULL, sizeof(recursive), limits[i]);
    assert(mvm);
    memcpy(mvm->ram->data, recursive, sizeof(recursive));
    for (size_t depth = 0; depth < limits[i]; ++depth) {
      assert(mvm_update(mvm) == MVM_RESULT_OK);
      assert(mvm_update(mvm) == MVM_RESULT_OK);
    }
    assert(mvm_update(mvm) == MVM_RESULT_OK);
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 6 && mvm->frame_count == limits[i]);
    assert(mvm->stack->size == 1 && mvm->stack->data[0].kind == MVM_KIND_FUNC);
    mvm_destroy(mvm);
  }

  // A function value cannot serve as the result of an empty callee.
  const uint8_t empty[] = {MVM_OP_LIT_FUNC, 7, 0, 0, 0,
                           MVM_OP_CALL_INDIRECT, 0, MVM_OP_RETURN};
  mvm_t *mvm = create_vm(sizeof(empty));
  memcpy(mvm->ram->data, empty, sizeof(empty));
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm_update(mvm) == MVM_RESULT_OK);
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->frame_count == 1 && mvm->stack->size == 0);
  mvm_destroy(mvm);

  // Missing argc, and missing VM value stack.
  mvm = create_vm(1);
  mvm->ram->data[0] = MVM_OP_CALL_INDIRECT;
  assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm->pc == 1 && mvm->stack->size == 0 && mvm->frame_count == 0);
  mvm_destroy(mvm);
  mvm = create_vm(2);
  mvm->ram->data[0] = MVM_OP_CALL_INDIRECT;
  mvm_stack_destroy(mvm->stack);
  mvm->stack = NULL;
  assert(mvm_update(mvm) == MVM_RESULT_VM_ERROR);
  assert(mvm->pc == 1 && mvm->frame_count == 0);
  mvm_destroy(mvm);
}

static void test_function_literal_errors(void) {
  const uint32_t addresses[] = {5, UINT32_MAX};
  for (size_t i = 0; i < sizeof(addresses) / sizeof(*addresses); ++i) {
    mvm_t *mvm = create_vm(5);
    mvm->ram->data[0] = MVM_OP_LIT_FUNC;
    mvm_ram_poke_uword(mvm->ram, 1, addresses[i]);
    assert(mvm_stack_push(mvm->stack, (mvm_value_t){.kind = MVM_KIND_NULL}));
    assert(mvm_update(mvm) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm->pc == 1 && mvm->stack->size == 1 && mvm->frame_count == 0);
    assert(mvm->stack->data[0].kind == MVM_KIND_NULL);
    mvm_destroy(mvm);
  }
}

int main(void) {
  test_function_values();
  test_indirect_recursion();
  test_indirect_zero_args();
  test_function_array();
  test_function_argument_gc();
  test_indirect_max_args();
  test_indirect_call_errors();
  test_function_literal_errors();
  test_literal_errors(MVM_OP_LIT_FUNC, 4);
  test_ram_scalars();
  test_ram_errors();
  test_ram_addresses_and_frames();
  test_ram_program();
  test_ram_code_access();
  test_ram_modified_strings();
  test_literal_sequence();
  test_float_bits();
  test_double_bits();
  test_integer_literals();
  test_literal_errors(MVM_OP_LIT_UBYTE, 1);
  test_literal_errors(MVM_OP_LIT_UHALF, 2);
  test_literal_errors(MVM_OP_LIT_UWORD, 4);
  test_literal_errors(MVM_OP_LIT_ULONG, 8);
  test_literal_errors(MVM_OP_LIT_SBYTE, 1);
  test_literal_errors(MVM_OP_LIT_SHALF, 2);
  test_literal_errors(MVM_OP_LIT_SWORD, 4);
  test_literal_errors(MVM_OP_LIT_SLONG, 8);
  test_literal_errors(MVM_OP_LIT_FLOAT, 4);
  test_literal_errors(MVM_OP_LIT_DOUBLE, 8);
  test_literal_errors(MVM_OP_LIT_STRING, 4);
  test_string_program();
  test_string_get();
  test_string_equality();
  test_string_errors();
  test_gc_stack_root();
  test_gc_global_root();
  test_gc_automatic_collection();
  test_comparisons();
  test_comparison_errors();
  test_not();
  test_comparison_program();
  test_jump();
  test_conditional_jumps();
  test_jump_errors();
  test_binary_arithmetic();
  test_unary_arithmetic();
  test_arithmetic_errors();
  test_arithmetic_program();
  test_bitwise();
  test_shifts();
  test_bitwise_errors();
  test_bitwise_program();
  test_cast();
  test_typeof_is_and_literal_type();
  test_type_program();
  test_type_op_errors();
  test_stack_ops();
  test_stack_op_errors();
  test_local_ops();
  test_local_ops_in_call();
  test_local_op_errors();
  test_global_ops();
  test_global_ops_in_call();
  test_global_op_errors();
  test_call_return();
  test_nested_calls();
  test_call_stack_overflow();
  test_call_errors();
  test_tail_call_recursion();
  test_tail_call_reuses_frame();
  test_tail_call_errors();
  test_return_errors();
  test_pc_out_of_bounds();
  test_native_scalar_types();
  test_native_calls();
  test_native_void_and_reference();
  test_native_errors();
  test_native_string_results();
  test_native_library();
  test_native_library_dispose();
  return 0;
}
