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

#include <string.h>

// Convert an index or RAM byte offset without truncation or signed wraparound.
mvm_result_t mvm_value_index(const mvm_value_t value, size_t *index) {
  uint64_t bits;
  switch (value.kind) {
  case MVM_KIND_UBYTE:
    bits = value.as.ub;
    break;
  case MVM_KIND_UHALF:
    bits = value.as.uh;
    break;
  case MVM_KIND_UWORD:
    bits = value.as.uw;
    break;
  case MVM_KIND_ULONG:
    bits = value.as.ul;
    break;
  case MVM_KIND_SBYTE:
    if (value.as.sb < 0)
      return MVM_RESULT_RUNTIME_ERROR;
    bits = (uint64_t)value.as.sb;
    break;
  case MVM_KIND_SHALF:
    if (value.as.sh < 0)
      return MVM_RESULT_RUNTIME_ERROR;
    bits = (uint64_t)value.as.sh;
    break;
  case MVM_KIND_SWORD:
    if (value.as.sw < 0)
      return MVM_RESULT_RUNTIME_ERROR;
    bits = (uint64_t)value.as.sw;
    break;
  case MVM_KIND_SLONG:
    if (value.as.sl < 0)
      return MVM_RESULT_RUNTIME_ERROR;
    bits = (uint64_t)value.as.sl;
    break;
  default:
    return MVM_RESULT_RUNTIME_ERROR;
  }
  if (bits > SIZE_MAX)
    return MVM_RESULT_RUNTIME_ERROR;
  *index = (size_t)bits;
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_equal_values(const mvm_t *mvm, const mvm_value_t lhs,
                                     const mvm_value_t rhs, bool *equal) {
  if (lhs.kind != rhs.kind) {
    *equal = false;
    return MVM_RESULT_OK;
  }

  switch (lhs.kind) {
  case MVM_KIND_VOID:
  case MVM_KIND_NULL:
    *equal = true;
    break;
  case MVM_KIND_BOOL:
    *equal = lhs.as.b == rhs.as.b;
    break;
  case MVM_KIND_UBYTE:
    *equal = lhs.as.ub == rhs.as.ub;
    break;
  case MVM_KIND_UHALF:
    *equal = lhs.as.uh == rhs.as.uh;
    break;
  case MVM_KIND_UWORD:
    *equal = lhs.as.uw == rhs.as.uw;
    break;
  case MVM_KIND_ULONG:
    *equal = lhs.as.ul == rhs.as.ul;
    break;
  case MVM_KIND_SBYTE:
    *equal = lhs.as.sb == rhs.as.sb;
    break;
  case MVM_KIND_SHALF:
    *equal = lhs.as.sh == rhs.as.sh;
    break;
  case MVM_KIND_SWORD:
    *equal = lhs.as.sw == rhs.as.sw;
    break;
  case MVM_KIND_SLONG:
    *equal = lhs.as.sl == rhs.as.sl;
    break;
  case MVM_KIND_FLOAT:
    *equal = lhs.as.f == rhs.as.f;
    break;
  case MVM_KIND_DOUBLE:
    *equal = lhs.as.d == rhs.as.d;
    break;
  case MVM_KIND_STRING: {
    size_t lhs_bytes;
    size_t rhs_bytes;
    size_t characters;
    mvm_result_t status = mvm_measure_string(mvm, lhs, &lhs_bytes, &characters);
    if (status != MVM_RESULT_OK)
      return status;
    status = mvm_measure_string(mvm, rhs, &rhs_bytes, &characters);
    if (status != MVM_RESULT_OK)
      return status;
    *equal =
        lhs_bytes == rhs_bytes && memcmp(lhs.as.s, rhs.as.s, lhs_bytes) == 0;
  } break;
  case MVM_KIND_ARRAY:
    if (!mvm_checked_array(mvm, lhs) || !mvm_checked_array(mvm, rhs))
      return MVM_RESULT_RUNTIME_ERROR;
    *equal = lhs.as.array == rhs.as.array;
    break;
  case MVM_KIND_REFERENCE:
    *equal = lhs.as.ptr == rhs.as.ptr;
    break;
  case MVM_KIND_TYPE:
    *equal = lhs.has_full_type == rhs.has_full_type && lhs.as.type == rhs.as.type;
    break;
  case MVM_KIND_FUNC:
    *equal = lhs.as.func == rhs.as.func;
    break;
  case MVM_KIND_THREAD:
    if (!mvm_checked_thread(mvm, lhs) || !mvm_checked_thread(mvm, rhs))
      return MVM_RESULT_RUNTIME_ERROR;
    *equal = lhs.as.thread == rhs.as.thread;
    break;
  case MVM_KIND_MUTEX:
    if (!mvm_checked_mutex(mvm, lhs) || !mvm_checked_mutex(mvm, rhs))
      return MVM_RESULT_RUNTIME_ERROR;
    *equal = lhs.as.mutex == rhs.as.mutex;
    break;
  case MVM_KIND_CONDITION:
    if (!mvm_checked_condition(mvm, lhs) || !mvm_checked_condition(mvm, rhs))
      return MVM_RESULT_RUNTIME_ERROR;
    *equal = lhs.as.condition == rhs.as.condition;
    break;
  case MVM_KIND_ATOMIC:
    if (!mvm_checked_atomic(mvm, lhs) || !mvm_checked_atomic(mvm, rhs))
      return MVM_RESULT_RUNTIME_ERROR;
    *equal = lhs.as.atomic == rhs.as.atomic;
    break;
  default:
    return MVM_RESULT_RUNTIME_ERROR;
  }
  return MVM_RESULT_OK;
}

mvm_result_t mvm_compare_values(const mvm_t *mvm, const mvm_opcodes_t op,
                                       const mvm_value_t lhs,
                                       const mvm_value_t rhs,
                                       mvm_value_t *result) {
  if (op == MVM_OP_EQ || op == MVM_OP_NE) {
    bool equal;
    const mvm_result_t status = mvm_equal_values(mvm, lhs, rhs, &equal);
    if (status != MVM_RESULT_OK)
      return status;
    *result = (mvm_value_t){.kind = MVM_KIND_BOOL,
                            .as.b = op == MVM_OP_EQ ? equal : !equal};
    return MVM_RESULT_OK;
  }

  if (lhs.kind != rhs.kind)
    return MVM_RESULT_RUNTIME_ERROR;

  bool comparison;
#define MVM_ORDERED_COMPARISON(kind, field)                                    \
  case kind:                                                                   \
    switch (op) {                                                              \
    case MVM_OP_LT:                                                            \
      comparison = lhs.as.field < rhs.as.field;                                \
      break;                                                                   \
    case MVM_OP_LE:                                                            \
      comparison = lhs.as.field <= rhs.as.field;                               \
      break;                                                                   \
    case MVM_OP_GT:                                                            \
      comparison = lhs.as.field > rhs.as.field;                                \
      break;                                                                   \
    case MVM_OP_GE:                                                            \
      comparison = lhs.as.field >= rhs.as.field;                               \
      break;                                                                   \
    default:                                                                   \
      return MVM_RESULT_RUNTIME_ERROR;                                         \
    }                                                                          \
    break

  switch (lhs.kind) {
    MVM_ORDERED_COMPARISON(MVM_KIND_UBYTE, ub);
    MVM_ORDERED_COMPARISON(MVM_KIND_UHALF, uh);
    MVM_ORDERED_COMPARISON(MVM_KIND_UWORD, uw);
    MVM_ORDERED_COMPARISON(MVM_KIND_ULONG, ul);
    MVM_ORDERED_COMPARISON(MVM_KIND_SBYTE, sb);
    MVM_ORDERED_COMPARISON(MVM_KIND_SHALF, sh);
    MVM_ORDERED_COMPARISON(MVM_KIND_SWORD, sw);
    MVM_ORDERED_COMPARISON(MVM_KIND_SLONG, sl);
    MVM_ORDERED_COMPARISON(MVM_KIND_FLOAT, f);
    MVM_ORDERED_COMPARISON(MVM_KIND_DOUBLE, d);
  default:
    return MVM_RESULT_RUNTIME_ERROR;
  }

#undef MVM_ORDERED_COMPARISON
  *result = (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = comparison};
  return MVM_RESULT_OK;
}
