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

static bool mvm_utf8_decode(const unsigned char *text, const size_t length,
                            size_t *offset, uint32_t *code_point) {
  const size_t start = *offset;
  const uint8_t first = text[start];
  size_t width;
  uint32_t value;
  uint32_t minimum;

  if (first <= 0x7F) {
    width = 1;
    value = first;
    minimum = 0;
  } else if (first >= 0xC2 && first <= 0xDF) {
    width = 2;
    value = first & 0x1F;
    minimum = 0x80;
  } else if (first >= 0xE0 && first <= 0xEF) {
    width = 3;
    value = first & 0x0F;
    minimum = 0x800;
  } else if (first >= 0xF0 && first <= 0xF4) {
    width = 4;
    value = first & 0x07;
    minimum = 0x10000;
  } else {
    return false;
  }

  if (width > length - start)
    return false;
  for (size_t i = 1; i < width; ++i) {
    const uint8_t next = text[start + i];
    if ((next & 0xC0) != 0x80)
      return false;
    value = (value << 6) | (next & 0x3F);
  }
  if (value < minimum || value > 0x10FFFF ||
      (value >= 0xD800 && value <= 0xDFFF))
    return false;

  *offset = start + width;
  *code_point = value;
  return true;
}

static mvm_result_t mvm_string_byte_length(const mvm_t *mvm,
                                           const mvm_value_t value,
                                           size_t *length) {
  if (value.kind != MVM_KIND_STRING || !value.as.s)
    return MVM_RESULT_RUNTIME_ERROR;

  // STORE can overwrite a literal's terminator after LITERAL_STRING validated
  // it. Bound every later read of RAM-backed strings to the remaining RAM.
  const uintptr_t pointer = (uintptr_t)value.as.s;
  const uintptr_t base = (uintptr_t)mvm->ram->data;
  if (pointer >= base && pointer - base < mvm->ram->size) {
    const char *end =
        memchr(value.as.s, '\0', mvm->ram->size - (pointer - base));
    if (!end)
      return MVM_RESULT_RUNTIME_ERROR;
    *length = (size_t)(end - value.as.s);
  } else {
    const mvm_object_t *object = mvm_gc_find(mvm, value.as.s);
    if (object) {
      const size_t offset = pointer - (uintptr_t)object->data;
      const char *end = memchr(value.as.s, '\0', object->size - offset);
      if (!end)
        return MVM_RESULT_RUNTIME_ERROR;
      *length = (size_t)(end - value.as.s);
    } else {
      // Host strings obey the native C API's lifetime/termination rules.
      *length = strlen(value.as.s);
    }
  }
  return MVM_RESULT_OK;
}

mvm_result_t mvm_measure_string(const mvm_t *mvm,
                                       const mvm_value_t value,
                                       size_t *byte_length,
                                       size_t *character_length) {
  const mvm_result_t status = mvm_string_byte_length(mvm, value, byte_length);
  if (status != MVM_RESULT_OK)
    return status;

  const unsigned char *text = (const unsigned char *)value.as.s;
  *character_length = 0;
  size_t offset = 0;
  while (offset < *byte_length) {
    uint32_t code_point;
    if (!mvm_utf8_decode(text, *byte_length, &offset, &code_point))
      return MVM_RESULT_RUNTIME_ERROR;
    ++*character_length;
  }
  return MVM_RESULT_OK;
}

mvm_result_t mvm_concat_strings(mvm_t *mvm, const mvm_value_t lhs,
                                       const mvm_value_t rhs,
                                       mvm_value_t *result) {
  size_t lhs_bytes;
  size_t rhs_bytes;
  size_t characters;
  mvm_result_t status = mvm_measure_string(mvm, lhs, &lhs_bytes, &characters);
  if (status != MVM_RESULT_OK)
    return status;
  status = mvm_measure_string(mvm, rhs, &rhs_bytes, &characters);
  if (status != MVM_RESULT_OK)
    return status;
  if (lhs_bytes > SIZE_MAX - rhs_bytes - 1)
    return MVM_RESULT_VM_ERROR;

  mvm_object_t *object =
      mvm_gc_allocate(mvm, MVM_OBJECT_STRING, lhs_bytes + rhs_bytes + 1);
  if (!object)
    return MVM_RESULT_VM_ERROR;
  char *data = (char *)object->data;
  memcpy(data, lhs.as.s, lhs_bytes);
  memcpy(data + lhs_bytes, rhs.as.s, rhs_bytes + 1);
  *result = (mvm_value_t){.kind = MVM_KIND_STRING, .as.s = data};
  return MVM_RESULT_OK;
}

mvm_result_t mvm_string_get(const mvm_t *mvm, const mvm_value_t string,
                                   const mvm_value_t index,
                                   mvm_value_t *result) {
  size_t wanted;
  mvm_result_t status = mvm_value_index(index, &wanted);
  if (status != MVM_RESULT_OK)
    return status;
  size_t length;
  size_t characters;
  status = mvm_measure_string(mvm, string, &length, &characters);
  if (status != MVM_RESULT_OK)
    return status;

  const unsigned char *text = (const unsigned char *)string.as.s;
  size_t offset = 0;
  for (size_t current = 0; offset < length; ++current) {
    uint32_t code_point;
    if (!mvm_utf8_decode(text, length, &offset, &code_point))
      return MVM_RESULT_RUNTIME_ERROR;
    if (current == wanted) {
      *result = (mvm_value_t){.kind = MVM_KIND_UWORD, .as.uw = code_point};
      return MVM_RESULT_OK;
    }
  }
  return MVM_RESULT_RUNTIME_ERROR;
}

mvm_result_t mvm_string_from_bytes(mvm_t *mvm, const unsigned char *data,
                                   const size_t length, mvm_value_t *result) {
  if (!mvm || !result || (!data && length))
    return MVM_RESULT_VM_ERROR;
  size_t offset = 0;
  while (offset < length) {
    uint32_t code_point;
    if (!mvm_utf8_decode(data, length, &offset, &code_point) || !code_point)
      return MVM_RESULT_RUNTIME_ERROR;
  }
  if (length == SIZE_MAX)
    return MVM_RESULT_VM_ERROR;
  mvm_object_t *object =
      mvm_gc_allocate(mvm, MVM_OBJECT_STRING, length + 1);
  if (!object)
    return MVM_RESULT_VM_ERROR;
  if (length)
    memcpy(object->data, data, length);
  object->data[length] = 0;
  *result = (mvm_value_t){.kind = MVM_KIND_STRING, .as.s = (char *)object->data};
  return MVM_RESULT_OK;
}
