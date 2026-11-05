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

#pragma once

#include "internal.h"

#include <math.h>
#include <string.h>

static inline bool mvm_cast_integer_kind(const mvm_kind_t kind) {
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

static inline uint64_t mvm_cast_integer_bits(const mvm_value_t value) {
  switch (value.kind) {
  case MVM_KIND_BOOL:
    return value.as.b;
  case MVM_KIND_UBYTE:
    return value.as.ub;
  case MVM_KIND_UHALF:
    return value.as.uh;
  case MVM_KIND_UWORD:
    return value.as.uw;
  case MVM_KIND_ULONG:
    return value.as.ul;
  case MVM_KIND_SBYTE:
    return (uint64_t)value.as.sb;
  case MVM_KIND_SHALF:
    return (uint64_t)value.as.sh;
  case MVM_KIND_SWORD:
    return (uint64_t)value.as.sw;
  case MVM_KIND_SLONG:
    return (uint64_t)value.as.sl;
  default:
    return 0;
  }
}

static inline mvm_result_t mvm_cast_from_float_fast(const long double value,
                                                    const mvm_kind_t kind,
                                                    mvm_value_t *result) {
  if (!isfinite(value))
    return MVM_RESULT_RUNTIME_ERROR;
  const long double truncated = truncl(value);

#define MVM_CAST_FLOAT_TO_UNSIGNED(target_kind, type, field, maximum)           \
  case target_kind:                                                             \
    if (truncated < 0 || truncated >= (long double)(maximum) + 1.0L)            \
      return MVM_RESULT_RUNTIME_ERROR;                                          \
    result->as.field = (type)truncated;                                         \
    break

#define MVM_CAST_FLOAT_TO_SIGNED(target_kind, type, field, minimum, maximum)    \
  case target_kind:                                                             \
    if (truncated < (long double)(minimum) ||                                   \
        truncated >= (long double)(maximum) + 1.0L)                             \
      return MVM_RESULT_RUNTIME_ERROR;                                          \
    result->as.field = (type)truncated;                                         \
    break

  switch (kind) {
    MVM_CAST_FLOAT_TO_UNSIGNED(MVM_KIND_UBYTE, uint8_t, ub, UINT8_MAX);
    MVM_CAST_FLOAT_TO_UNSIGNED(MVM_KIND_UHALF, uint16_t, uh, UINT16_MAX);
    MVM_CAST_FLOAT_TO_UNSIGNED(MVM_KIND_UWORD, uint32_t, uw, UINT32_MAX);
    MVM_CAST_FLOAT_TO_UNSIGNED(MVM_KIND_ULONG, uint64_t, ul, UINT64_MAX);
    MVM_CAST_FLOAT_TO_SIGNED(MVM_KIND_SBYTE, int8_t, sb, INT8_MIN, INT8_MAX);
    MVM_CAST_FLOAT_TO_SIGNED(MVM_KIND_SHALF, int16_t, sh, INT16_MIN, INT16_MAX);
    MVM_CAST_FLOAT_TO_SIGNED(MVM_KIND_SWORD, int32_t, sw, INT32_MIN, INT32_MAX);
    MVM_CAST_FLOAT_TO_SIGNED(MVM_KIND_SLONG, int64_t, sl, INT64_MIN, INT64_MAX);
  default:
    return MVM_RESULT_RUNTIME_ERROR;
  }

#undef MVM_CAST_FLOAT_TO_SIGNED
#undef MVM_CAST_FLOAT_TO_UNSIGNED
  return MVM_RESULT_OK;
}

static inline mvm_result_t mvm_cast_value_fast(const mvm_value_t value,
                                               const mvm_kind_t kind,
                                               mvm_value_t *result) {
  if (kind >= MVM_KIND_COUNT)
    return MVM_RESULT_RUNTIME_ERROR;
  if (value.kind == kind) {
    *result = value;
    return MVM_RESULT_OK;
  }
  *result = (mvm_value_t){.kind = kind};

  if (kind == MVM_KIND_BOOL) {
    switch (value.kind) {
    case MVM_KIND_UBYTE:
      result->as.b = value.as.ub != 0;
      break;
    case MVM_KIND_UHALF:
      result->as.b = value.as.uh != 0;
      break;
    case MVM_KIND_UWORD:
      result->as.b = value.as.uw != 0;
      break;
    case MVM_KIND_ULONG:
      result->as.b = value.as.ul != 0;
      break;
    case MVM_KIND_SBYTE:
      result->as.b = value.as.sb != 0;
      break;
    case MVM_KIND_SHALF:
      result->as.b = value.as.sh != 0;
      break;
    case MVM_KIND_SWORD:
      result->as.b = value.as.sw != 0;
      break;
    case MVM_KIND_SLONG:
      result->as.b = value.as.sl != 0;
      break;
    case MVM_KIND_FLOAT:
      result->as.b = value.as.f != 0.0f;
      break;
    case MVM_KIND_DOUBLE:
      result->as.b = value.as.d != 0.0;
      break;
    default:
      return MVM_RESULT_RUNTIME_ERROR;
    }
    return MVM_RESULT_OK;
  }

  if (kind == MVM_KIND_FLOAT || kind == MVM_KIND_DOUBLE) {
#define MVM_CAST_TO_FLOAT(source_kind, field)                                   \
  case source_kind:                                                             \
    if (kind == MVM_KIND_FLOAT)                                                 \
      result->as.f = (float)value.as.field;                                     \
    else                                                                        \
      result->as.d = (double)value.as.field;                                    \
    break
    switch (value.kind) {
      MVM_CAST_TO_FLOAT(MVM_KIND_BOOL, b);
      MVM_CAST_TO_FLOAT(MVM_KIND_UBYTE, ub);
      MVM_CAST_TO_FLOAT(MVM_KIND_UHALF, uh);
      MVM_CAST_TO_FLOAT(MVM_KIND_UWORD, uw);
      MVM_CAST_TO_FLOAT(MVM_KIND_ULONG, ul);
      MVM_CAST_TO_FLOAT(MVM_KIND_SBYTE, sb);
      MVM_CAST_TO_FLOAT(MVM_KIND_SHALF, sh);
      MVM_CAST_TO_FLOAT(MVM_KIND_SWORD, sw);
      MVM_CAST_TO_FLOAT(MVM_KIND_SLONG, sl);
      MVM_CAST_TO_FLOAT(MVM_KIND_FLOAT, f);
      MVM_CAST_TO_FLOAT(MVM_KIND_DOUBLE, d);
    default:
      return MVM_RESULT_RUNTIME_ERROR;
    }
#undef MVM_CAST_TO_FLOAT
    return MVM_RESULT_OK;
  }

  if (!mvm_cast_integer_kind(kind))
    return MVM_RESULT_RUNTIME_ERROR;
  if (value.kind == MVM_KIND_FLOAT)
    return mvm_cast_from_float_fast(value.as.f, kind, result);
  if (value.kind == MVM_KIND_DOUBLE)
    return mvm_cast_from_float_fast(value.as.d, kind, result);
  if (value.kind != MVM_KIND_BOOL && !mvm_cast_integer_kind(value.kind))
    return MVM_RESULT_RUNTIME_ERROR;

  const uint64_t bits = mvm_cast_integer_bits(value);
  switch (kind) {
  case MVM_KIND_UBYTE:
    result->as.ub = (uint8_t)bits;
    break;
  case MVM_KIND_UHALF:
    result->as.uh = (uint16_t)bits;
    break;
  case MVM_KIND_UWORD:
    result->as.uw = (uint32_t)bits;
    break;
  case MVM_KIND_ULONG:
    result->as.ul = bits;
    break;
#define MVM_CAST_TO_SIGNED(target_kind, utype, field)                           \
  case target_kind: {                                                           \
    const utype signed_bits = (utype)bits;                                      \
    memcpy(&result->as.field, &signed_bits, sizeof(signed_bits));               \
  } break
    MVM_CAST_TO_SIGNED(MVM_KIND_SBYTE, uint8_t, sb);
    MVM_CAST_TO_SIGNED(MVM_KIND_SHALF, uint16_t, sh);
    MVM_CAST_TO_SIGNED(MVM_KIND_SWORD, uint32_t, sw);
    MVM_CAST_TO_SIGNED(MVM_KIND_SLONG, uint64_t, sl);
#undef MVM_CAST_TO_SIGNED
  default:
    return MVM_RESULT_RUNTIME_ERROR;
  }
  return MVM_RESULT_OK;
}
