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
#include "numeric_cast.h"

#include <limits.h>
#include <math.h>
#include <string.h>

mvm_result_t mvm_binary_arithmetic(const mvm_opcodes_t op,
                                          const mvm_value_t lhs,
                                          const mvm_value_t rhs,
                                          mvm_value_t *result) {
  if (lhs.kind != rhs.kind)
    return MVM_RESULT_RUNTIME_ERROR;
  *result = (mvm_value_t){.kind = lhs.kind};

#define MVM_UNSIGNED_ARITHMETIC(kind, type, field)                             \
  case kind: {                                                                 \
    const uint64_t a = lhs.as.field;                                           \
    const uint64_t b = rhs.as.field;                                           \
    if ((op == MVM_OP_DIV || op == MVM_OP_MOD) && b == 0)                      \
      return MVM_RESULT_RUNTIME_ERROR;                                         \
    uint64_t value;                                                            \
    switch (op) {                                                              \
    case MVM_OP_ADD:                                                           \
      value = a + b;                                                           \
      break;                                                                   \
    case MVM_OP_SUB:                                                           \
      value = a - b;                                                           \
      break;                                                                   \
    case MVM_OP_MUL:                                                           \
      value = a * b;                                                           \
      break;                                                                   \
    case MVM_OP_DIV:                                                           \
      value = a / b;                                                           \
      break;                                                                   \
    case MVM_OP_MOD:                                                           \
      value = a % b;                                                           \
      break;                                                                   \
    default:                                                                   \
      return MVM_RESULT_RUNTIME_ERROR;                                         \
    }                                                                          \
    result->as.field = (type)value;                                            \
  } break

#define MVM_SIGNED_ARITHMETIC(kind, type, utype, field, minimum)               \
  case kind: {                                                                 \
    const type a = lhs.as.field;                                               \
    const type b = rhs.as.field;                                               \
    if ((op == MVM_OP_DIV || op == MVM_OP_MOD) && b == 0)                      \
      return MVM_RESULT_RUNTIME_ERROR;                                         \
    if (op == MVM_OP_DIV) {                                                    \
      result->as.field = a == minimum && b == -1 ? minimum : (type)(a / b);    \
      return MVM_RESULT_OK;                                                    \
    }                                                                          \
    if (op == MVM_OP_MOD) {                                                    \
      result->as.field = a == minimum && b == -1 ? 0 : (type)(a % b);          \
      return MVM_RESULT_OK;                                                    \
    }                                                                          \
    utype bits;                                                                \
    switch (op) {                                                              \
    case MVM_OP_ADD:                                                           \
      bits = (utype)((uint64_t)(utype)a + (uint64_t)(utype)b);                 \
      break;                                                                   \
    case MVM_OP_SUB:                                                           \
      bits = (utype)((uint64_t)(utype)a - (uint64_t)(utype)b);                 \
      break;                                                                   \
    case MVM_OP_MUL:                                                           \
      bits = (utype)((uint64_t)(utype)a * (uint64_t)(utype)b);                 \
      break;                                                                   \
    default:                                                                   \
      return MVM_RESULT_RUNTIME_ERROR;                                         \
    }                                                                          \
    memcpy(&result->as.field, &bits, sizeof(bits));                            \
  } break

#define MVM_FLOAT_ARITHMETIC(kind, type, field, modulo)                        \
  case kind: {                                                                 \
    const type a = lhs.as.field;                                               \
    const type b = rhs.as.field;                                               \
    switch (op) {                                                              \
    case MVM_OP_ADD:                                                           \
      result->as.field = a + b;                                                \
      break;                                                                   \
    case MVM_OP_SUB:                                                           \
      result->as.field = a - b;                                                \
      break;                                                                   \
    case MVM_OP_MUL:                                                           \
      result->as.field = a * b;                                                \
      break;                                                                   \
    case MVM_OP_DIV:                                                           \
      result->as.field = a / b;                                                \
      break;                                                                   \
    case MVM_OP_MOD:                                                           \
      result->as.field = modulo(a, b);                                         \
      break;                                                                   \
    default:                                                                   \
      return MVM_RESULT_RUNTIME_ERROR;                                         \
    }                                                                          \
  } break

  switch (lhs.kind) {
    MVM_UNSIGNED_ARITHMETIC(MVM_KIND_UBYTE, uint8_t, ub);
    MVM_UNSIGNED_ARITHMETIC(MVM_KIND_UHALF, uint16_t, uh);
    MVM_UNSIGNED_ARITHMETIC(MVM_KIND_UWORD, uint32_t, uw);
    MVM_UNSIGNED_ARITHMETIC(MVM_KIND_ULONG, uint64_t, ul);
    MVM_SIGNED_ARITHMETIC(MVM_KIND_SBYTE, int8_t, uint8_t, sb, INT8_MIN);
    MVM_SIGNED_ARITHMETIC(MVM_KIND_SHALF, int16_t, uint16_t, sh, INT16_MIN);
    MVM_SIGNED_ARITHMETIC(MVM_KIND_SWORD, int32_t, uint32_t, sw, INT32_MIN);
    MVM_SIGNED_ARITHMETIC(MVM_KIND_SLONG, int64_t, uint64_t, sl, INT64_MIN);
    MVM_FLOAT_ARITHMETIC(MVM_KIND_FLOAT, float, f, fmodf);
    MVM_FLOAT_ARITHMETIC(MVM_KIND_DOUBLE, double, d, fmod);
  default:
    return MVM_RESULT_RUNTIME_ERROR;
  }

#undef MVM_FLOAT_ARITHMETIC
#undef MVM_SIGNED_ARITHMETIC
#undef MVM_UNSIGNED_ARITHMETIC

  return MVM_RESULT_OK;
}

mvm_result_t mvm_unary_arithmetic(const mvm_opcodes_t op,
                                         const mvm_value_t value,
                                         mvm_value_t *result) {
  *result = (mvm_value_t){.kind = value.kind};

  if (op == MVM_OP_NEG) {
#define MVM_SIGNED_NEGATE(kind, utype, field)                                  \
  case kind: {                                                                 \
    const utype bits = (utype)(0 - (uint64_t)(utype)value.as.field);           \
    memcpy(&result->as.field, &bits, sizeof(bits));                            \
  } break
    switch (value.kind) {
      MVM_SIGNED_NEGATE(MVM_KIND_SBYTE, uint8_t, sb);
      MVM_SIGNED_NEGATE(MVM_KIND_SHALF, uint16_t, sh);
      MVM_SIGNED_NEGATE(MVM_KIND_SWORD, uint32_t, sw);
      MVM_SIGNED_NEGATE(MVM_KIND_SLONG, uint64_t, sl);
    case MVM_KIND_FLOAT:
      result->as.f = -value.as.f;
      break;
    case MVM_KIND_DOUBLE:
      result->as.d = -value.as.d;
      break;
    default:
      return MVM_RESULT_RUNTIME_ERROR;
    }
#undef MVM_SIGNED_NEGATE
    return MVM_RESULT_OK;
  }

  if (op != MVM_OP_INC && op != MVM_OP_DEC)
    return MVM_RESULT_RUNTIME_ERROR;

  mvm_value_t one = {.kind = value.kind};
  switch (value.kind) {
  case MVM_KIND_UBYTE:
    one.as.ub = 1;
    break;
  case MVM_KIND_UHALF:
    one.as.uh = 1;
    break;
  case MVM_KIND_UWORD:
    one.as.uw = 1;
    break;
  case MVM_KIND_ULONG:
    one.as.ul = 1;
    break;
  case MVM_KIND_SBYTE:
    one.as.sb = 1;
    break;
  case MVM_KIND_SHALF:
    one.as.sh = 1;
    break;
  case MVM_KIND_SWORD:
    one.as.sw = 1;
    break;
  case MVM_KIND_SLONG:
    one.as.sl = 1;
    break;
  case MVM_KIND_FLOAT:
    one.as.f = 1.0f;
    break;
  case MVM_KIND_DOUBLE:
    one.as.d = 1.0;
    break;
  default:
    return MVM_RESULT_RUNTIME_ERROR;
  }

  return mvm_binary_arithmetic(op == MVM_OP_INC ? MVM_OP_ADD : MVM_OP_SUB,
                               value, one, result);
}

static bool mvm_kind_is_integer(const mvm_kind_t kind) {
  switch (kind) {
  case MVM_KIND_UBYTE:
  case MVM_KIND_UHALF:
  case MVM_KIND_UWORD:
  case MVM_KIND_ULONG:
  case MVM_KIND_SBYTE:
  case MVM_KIND_SHALF:
  case MVM_KIND_SWORD:
  case MVM_KIND_SLONG:
    return true;
  default:
    return false;
  }
}

static bool mvm_shift_count(const mvm_value_t value, uint64_t *count) {
  switch (value.kind) {
  case MVM_KIND_UBYTE:
    *count = value.as.ub;
    return true;
  case MVM_KIND_UHALF:
    *count = value.as.uh;
    return true;
  case MVM_KIND_UWORD:
    *count = value.as.uw;
    return true;
  case MVM_KIND_ULONG:
    *count = value.as.ul;
    return true;
  default:
    return false;
  }
}

mvm_result_t mvm_binary_bitwise(const mvm_opcodes_t op,
                                       const mvm_value_t lhs,
                                       const mvm_value_t rhs,
                                       mvm_value_t *result) {
  if (op == MVM_OP_SHL || op == MVM_OP_SHR) {
    uint64_t count;
    if (!mvm_kind_is_integer(lhs.kind) || !mvm_shift_count(rhs, &count))
      return MVM_RESULT_RUNTIME_ERROR;
    *result = (mvm_value_t){.kind = lhs.kind};

#define MVM_SHIFT(kind, utype, field, is_signed)                               \
  case kind: {                                                                 \
    const unsigned width = sizeof(utype) * CHAR_BIT;                           \
    const utype bits = (utype)lhs.as.field;                                    \
    utype value;                                                               \
    if (count >= width) {                                                      \
      value = op == MVM_OP_SHR && is_signed && (bits >> (width - 1)) != 0      \
                  ? (utype)UINT64_MAX                                          \
                  : 0;                                                         \
    } else if (op == MVM_OP_SHL) {                                             \
      value = (utype)((uint64_t)bits << count);                                \
    } else if (count == 0) {                                                   \
      value = bits;                                                            \
    } else if (is_signed && (bits >> (width - 1)) != 0) {                      \
      value = (utype)((bits >> count) | (UINT64_MAX << (width - count)));      \
    } else {                                                                   \
      value = bits >> count;                                                   \
    }                                                                          \
    memcpy(&result->as.field, &value, sizeof(value));                          \
  } break

    switch (lhs.kind) {
      MVM_SHIFT(MVM_KIND_UBYTE, uint8_t, ub, false);
      MVM_SHIFT(MVM_KIND_UHALF, uint16_t, uh, false);
      MVM_SHIFT(MVM_KIND_UWORD, uint32_t, uw, false);
      MVM_SHIFT(MVM_KIND_ULONG, uint64_t, ul, false);
      MVM_SHIFT(MVM_KIND_SBYTE, uint8_t, sb, true);
      MVM_SHIFT(MVM_KIND_SHALF, uint16_t, sh, true);
      MVM_SHIFT(MVM_KIND_SWORD, uint32_t, sw, true);
      MVM_SHIFT(MVM_KIND_SLONG, uint64_t, sl, true);
    default:
      return MVM_RESULT_RUNTIME_ERROR;
    }
#undef MVM_SHIFT
    return MVM_RESULT_OK;
  }

  if (lhs.kind != rhs.kind)
    return MVM_RESULT_RUNTIME_ERROR;
  *result = (mvm_value_t){.kind = lhs.kind};

#define MVM_BITWISE(kind, type, field)                                         \
  case kind: {                                                                 \
    const type a = lhs.as.field;                                               \
    const type b = rhs.as.field;                                               \
    switch (op) {                                                              \
    case MVM_OP_BAND:                                                          \
      result->as.field = (type)(a & b);                                        \
      break;                                                                   \
    case MVM_OP_BOR:                                                           \
      result->as.field = (type)(a | b);                                        \
      break;                                                                   \
    case MVM_OP_BXOR:                                                          \
      result->as.field = (type)(a ^ b);                                        \
      break;                                                                   \
    default:                                                                   \
      return MVM_RESULT_RUNTIME_ERROR;                                         \
    }                                                                          \
  } break

  switch (lhs.kind) {
    MVM_BITWISE(MVM_KIND_UBYTE, uint8_t, ub);
    MVM_BITWISE(MVM_KIND_UHALF, uint16_t, uh);
    MVM_BITWISE(MVM_KIND_UWORD, uint32_t, uw);
    MVM_BITWISE(MVM_KIND_ULONG, uint64_t, ul);
    MVM_BITWISE(MVM_KIND_SBYTE, int8_t, sb);
    MVM_BITWISE(MVM_KIND_SHALF, int16_t, sh);
    MVM_BITWISE(MVM_KIND_SWORD, int32_t, sw);
    MVM_BITWISE(MVM_KIND_SLONG, int64_t, sl);
  default:
    return MVM_RESULT_RUNTIME_ERROR;
  }
#undef MVM_BITWISE
  return MVM_RESULT_OK;
}

mvm_result_t mvm_bitwise_not(const mvm_value_t value,
                                    mvm_value_t *result) {
  *result = (mvm_value_t){.kind = value.kind};
#define MVM_BITWISE_NOT(kind, type, field)                                     \
  case kind:                                                                   \
    result->as.field = (type)~value.as.field;                                  \
    break

  switch (value.kind) {
    MVM_BITWISE_NOT(MVM_KIND_UBYTE, uint8_t, ub);
    MVM_BITWISE_NOT(MVM_KIND_UHALF, uint16_t, uh);
    MVM_BITWISE_NOT(MVM_KIND_UWORD, uint32_t, uw);
    MVM_BITWISE_NOT(MVM_KIND_ULONG, uint64_t, ul);
    MVM_BITWISE_NOT(MVM_KIND_SBYTE, int8_t, sb);
    MVM_BITWISE_NOT(MVM_KIND_SHALF, int16_t, sh);
    MVM_BITWISE_NOT(MVM_KIND_SWORD, int32_t, sw);
    MVM_BITWISE_NOT(MVM_KIND_SLONG, int64_t, sl);
  default:
    return MVM_RESULT_RUNTIME_ERROR;
  }
#undef MVM_BITWISE_NOT
  return MVM_RESULT_OK;
}

mvm_result_t mvm_cast_value(const mvm_value_t value, const mvm_kind_t kind,
                            mvm_value_t *result) {
  return mvm_cast_value_fast(value, kind, result);
}
