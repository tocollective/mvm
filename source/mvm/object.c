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

static mvm_value_t mvm_array_default(mvm_kind_t kind) {
  mvm_value_t initial = {.kind =
                             mvm_nullable_kind(kind) ? MVM_KIND_NULL : kind};
  // Initialize the active union member, including floating point and TYPE.
  switch (kind) {
  case MVM_KIND_BOOL:
    initial.as.b = false;
    break;
  case MVM_KIND_UBYTE:
    initial.as.ub = 0;
    break;
  case MVM_KIND_UHALF:
    initial.as.uh = 0;
    break;
  case MVM_KIND_UWORD:
    initial.as.uw = 0;
    break;
  case MVM_KIND_ULONG:
    initial.as.ul = 0;
    break;
  case MVM_KIND_SBYTE:
    initial.as.sb = 0;
    break;
  case MVM_KIND_SHALF:
    initial.as.sh = 0;
    break;
  case MVM_KIND_SWORD:
    initial.as.sw = 0;
    break;
  case MVM_KIND_SLONG:
    initial.as.sl = 0;
    break;
  case MVM_KIND_FLOAT:
    initial.as.f = 0.0f;
    break;
  case MVM_KIND_DOUBLE:
    initial.as.d = 0.0;
    break;
  case MVM_KIND_TYPE:
    initial.as.type = MVM_KIND_VOID;
    break;
  default:
    break;
  }
  return initial;
}

mvm_result_t mvm_array_new(mvm_t *mvm, mvm_kind_t kind, size_t length,
                           mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  if (kind <= MVM_KIND_VOID || kind >= MVM_KIND_COUNT ||
      kind == MVM_KIND_PACKET || kind == MVM_KIND_UNION || length > UINT32_MAX)
    return MVM_RESULT_RUNTIME_ERROR;
  if (length > SIZE_MAX / sizeof(mvm_value_t))
    return MVM_RESULT_VM_ERROR;
  mvm_object_t *object =
      mvm_gc_allocate(mvm, MVM_OBJECT_ARRAY, sizeof(mvm_array_t));
  if (!object)
    return MVM_RESULT_VM_ERROR;
  mvm_array_t *array = (mvm_array_t *)object->data;
  array->kind = kind;
  array->element_type_id = MVM_TYPE_ID_NONE;
  const mvm_value_t value = {.kind = MVM_KIND_ARRAY, .as.array = array};
  const mvm_result_t status = mvm_array_resize(mvm, value, length);
  if (status != MVM_RESULT_OK)
    return status;
  *result = value;
  return MVM_RESULT_OK;
}

mvm_result_t mvm_array_new_primitive(mvm_t *mvm, const mvm_kind_t kind,
                                     const size_t length, mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  uint32_t element_id = MVM_TYPE_ID_NONE;
  for (size_t i = 0; i < mvm->type_count; ++i) {
    const mvm_type_t *type = &mvm->types[i];
    if (type->tag == MVM_TYPE_PRIMITIVE && type->kind == kind &&
        !(type->flags & MVM_TYPE_FLAG_NULLABLE)) {
      element_id = (uint32_t)i;
      break;
    }
  }
  if (element_id == MVM_TYPE_ID_NONE)
    return MVM_RESULT_RUNTIME_ERROR;
  for (size_t i = 0; i < mvm->type_count; ++i) {
    const mvm_type_t *type = &mvm->types[i];
    if (type->tag == MVM_TYPE_ARRAY && type->inner_type_id == element_id)
      return mvm_array_new_typed(mvm, (uint32_t)i, length, result);
  }
  return MVM_RESULT_RUNTIME_ERROR;
}

mvm_array_t *mvm_checked_array(const mvm_t *mvm, mvm_value_t value) {
  if (value.kind != MVM_KIND_ARRAY)
    return NULL;
  const mvm_object_t *object = mvm_gc_find(mvm, value.as.array);
  if (!object || object->kind != MVM_OBJECT_ARRAY ||
      (const void *)object->data != (const void *)value.as.array)
    return NULL;
  return value.as.array;
}

mvm_result_t mvm_array_get(const mvm_t *mvm, mvm_value_t value, size_t index,
                           mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  const mvm_array_t *array = mvm_checked_array(mvm, value);
  if (!array || index >= array->length)
    return MVM_RESULT_RUNTIME_ERROR;
  if (array->element_type_id != MVM_TYPE_ID_NONE &&
      !mvm_value_matches_type(mvm, array->element_type_id, array->elements[index]))
    return MVM_RESULT_RUNTIME_ERROR;
  *result = array->elements[index];
  return MVM_RESULT_OK;
}

static bool mvm_array_accepts(const mvm_t *mvm, const mvm_array_t *array,
                              mvm_value_t value) {
  if (array->element_type_id != MVM_TYPE_ID_NONE)
    return mvm_value_matches_type(mvm, array->element_type_id, value);
  return (value.kind == array->kind ||
          (value.kind == MVM_KIND_NULL && mvm_nullable_kind(array->kind))) &&
         (value.kind != MVM_KIND_ARRAY || mvm_checked_array(mvm, value)) &&
         (value.kind != MVM_KIND_THREAD || mvm_checked_thread(mvm, value)) &&
         mvm_sync_value_valid(mvm, value);
}

mvm_result_t mvm_array_set(mvm_t *mvm, mvm_value_t target, size_t index,
                           mvm_value_t value) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  mvm_array_t *array = mvm_checked_array(mvm, target);
  if (!array || index >= array->length || !mvm_array_accepts(mvm, array, value))
    return MVM_RESULT_RUNTIME_ERROR;
  array->elements[index] = value;
  return MVM_RESULT_OK;
}

mvm_result_t mvm_array_length(const mvm_t *mvm, mvm_value_t value,
                              size_t *length) {
  if (!mvm || !length)
    return MVM_RESULT_VM_ERROR;
  const mvm_array_t *array = mvm_checked_array(mvm, value);
  if (!array)
    return MVM_RESULT_RUNTIME_ERROR;
  *length = array->length;
  return MVM_RESULT_OK;
}

mvm_result_t mvm_array_capacity(const mvm_t *mvm, mvm_value_t value,
                                size_t *capacity) {
  if (!mvm || !capacity)
    return MVM_RESULT_VM_ERROR;
  const mvm_array_t *array = mvm_checked_array(mvm, value);
  if (!array)
    return MVM_RESULT_RUNTIME_ERROR;
  *capacity = array->capacity;
  return MVM_RESULT_OK;
}

mvm_result_t mvm_array_reserve(mvm_t *mvm, mvm_value_t value, size_t capacity) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  mvm_array_t *array = mvm_checked_array(mvm, value);
  if (!array || capacity > UINT32_MAX)
    return MVM_RESULT_RUNTIME_ERROR;
  mvm_gc_root_t root;
  if (!mvm_gc_root_register(mvm, &root, &value, 1))
    return MVM_RESULT_VM_ERROR;
  const bool success = mvm_gc_array_reserve(mvm, array, capacity);
  mvm_gc_root_unregister(mvm, &root);
  return success ? MVM_RESULT_OK : MVM_RESULT_VM_ERROR;
}

// Automatic growth is geometric, bounded by both the byte size and VM limit.
static mvm_result_t mvm_array_grow(mvm_t *mvm, mvm_value_t value,
                                   size_t required) {
  const mvm_array_t *array = value.as.array;
  if (required <= array->capacity)
    return MVM_RESULT_OK;
  const size_t byte_limit = SIZE_MAX / sizeof(*array->elements);
  const size_t limit = byte_limit < UINT32_MAX ? byte_limit : UINT32_MAX;
  if (required > limit)
    return MVM_RESULT_VM_ERROR;
  size_t capacity = array->capacity;
  if (!capacity)
    capacity = limit < 8 ? limit : 8;
  while (capacity < required)
    capacity = capacity > limit / 2 ? limit : capacity * 2;
  return mvm_array_reserve(mvm, value, capacity);
}

mvm_result_t mvm_array_resize(mvm_t *mvm, mvm_value_t value, size_t length) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  mvm_array_t *array = mvm_checked_array(mvm, value);
  if (!array || length > UINT32_MAX)
    return MVM_RESULT_RUNTIME_ERROR;
  if (length > array->length && array->element_type_id != MVM_TYPE_ID_NONE &&
      !mvm_type_defaultable(mvm, array->element_type_id))
    return MVM_RESULT_RUNTIME_ERROR;
  const mvm_result_t status = mvm_array_grow(mvm, value, length);
  if (status != MVM_RESULT_OK)
    return status;
  const mvm_type_t *element = array->element_type_id == MVM_TYPE_ID_NONE ? NULL :
      mvm_type_by_id(mvm, array->element_type_id);
  if (length > array->length && element && element->tag == MVM_TYPE_AGGREGATE) {
    const size_t old_length = array->length;
    mvm_gc_root_t root;
    if (!mvm_gc_root_register(mvm, &root, &value, 1))
      return MVM_RESULT_VM_ERROR;
    for (size_t i = old_length; i < length; ++i) {
      mvm_value_t item;
      const mvm_result_t initialized = mvm_typed_alloc(mvm, array->element_type_id, &item);
      if (initialized != MVM_RESULT_OK) {
        for (size_t j = old_length; j < array->length; ++j)
          array->elements[j] = (mvm_value_t){.kind = MVM_KIND_NULL};
        array->length = old_length;
        mvm_gc_root_unregister(mvm, &root);
        return initialized;
      }
      array->elements[i] = item;
      array->length = i + 1;
    }
    mvm_gc_root_unregister(mvm, &root);
    return MVM_RESULT_OK;
  }
  mvm_value_t initial = mvm_array_default(array->kind);
  if (element) {
    initial.type_id = array->element_type_id;
    initial.has_full_type = true;
    if (element->kind == MVM_KIND_TYPE) {
      for (size_t i = 0; i < mvm->type_count; ++i)
        if (mvm->types[i].tag == MVM_TYPE_PRIMITIVE && mvm->types[i].kind == MVM_KIND_VOID) {
          initial.as.type = (uint32_t)i;
          break;
        }
    }
  }
  // Clear truncated slots as well, so removed references cannot be retained.
  const size_t first = length < array->length ? length : array->length;
  const size_t last = length > array->length ? length : array->length;
  for (size_t i = first; i < last; ++i)
    array->elements[i] = initial;
  array->length = length;
  return MVM_RESULT_OK;
}

mvm_result_t mvm_array_insert(mvm_t *mvm, mvm_value_t target, size_t index,
                              mvm_value_t value) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  mvm_array_t *array = mvm_checked_array(mvm, target);
  if (!array || index > array->length || array->length == UINT32_MAX ||
      !mvm_array_accepts(mvm, array, value))
    return MVM_RESULT_RUNTIME_ERROR;
  // The inserted value may only exist in this C argument when growth collects.
  mvm_gc_root_t root;
  if (!mvm_gc_root_register(mvm, &root, &value, 1))
    return MVM_RESULT_VM_ERROR;
  const mvm_result_t status = mvm_array_grow(mvm, target, array->length + 1);
  mvm_gc_root_unregister(mvm, &root);
  if (status != MVM_RESULT_OK)
    return status;
  memmove(array->elements + index + 1, array->elements + index,
          (array->length - index) * sizeof(*array->elements));
  array->elements[index] = value;
  ++array->length;
  return MVM_RESULT_OK;
}

mvm_result_t mvm_array_push(mvm_t *mvm, mvm_value_t target, mvm_value_t value) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  const mvm_array_t *array = mvm_checked_array(mvm, target);
  if (!array)
    return MVM_RESULT_RUNTIME_ERROR;
  return mvm_array_insert(mvm, target, array->length, value);
}

mvm_result_t mvm_array_remove(mvm_t *mvm, mvm_value_t target, size_t index,
                              mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  mvm_array_t *array = mvm_checked_array(mvm, target);
  if (!array || index >= array->length)
    return MVM_RESULT_RUNTIME_ERROR;
  const mvm_value_t removed = array->elements[index];
  memmove(array->elements + index, array->elements + index + 1,
          (array->length - index - 1) * sizeof(*array->elements));
  --array->length;
  array->elements[array->length] = mvm_array_default(array->kind);
  *result = removed;
  return MVM_RESULT_OK;
}

mvm_result_t mvm_array_pop(mvm_t *mvm, mvm_value_t target,
                           mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  const mvm_array_t *array = mvm_checked_array(mvm, target);
  if (!array || !array->length)
    return MVM_RESULT_RUNTIME_ERROR;
  return mvm_array_remove(mvm, target, array->length - 1, result);
}

mvm_result_t mvm_record_new(mvm_t *mvm, size_t length, mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  if (length > (SIZE_MAX - sizeof(struct mvm_record)) / sizeof(mvm_value_t))
    return MVM_RESULT_VM_ERROR;
  mvm_object_t *object =
      mvm_gc_allocate(mvm, MVM_OBJECT_RECORD,
                      sizeof(struct mvm_record) + length * sizeof(mvm_value_t));
  if (!object)
    return MVM_RESULT_VM_ERROR;
  struct mvm_record *record = (struct mvm_record *)object->data;
  record->length = length;
  for (size_t i = 0; i < length; ++i)
    record->fields[i] = (mvm_value_t){.kind = MVM_KIND_NULL};
  *result = (mvm_value_t){.kind = MVM_KIND_REFERENCE, .as.ptr = record};
  return MVM_RESULT_OK;
}

static struct mvm_record *mvm_checked_record(const mvm_t *mvm,
                                             mvm_value_t value) {
  if (value.kind != MVM_KIND_REFERENCE)
    return NULL;
  const mvm_object_t *object = mvm_gc_find(mvm, value.as.ptr);
  if (!object || object->kind != MVM_OBJECT_RECORD ||
      (const void *)object->data != value.as.ptr)
    return NULL;
  return value.as.ptr;
}

mvm_result_t mvm_record_get(const mvm_t *mvm, mvm_value_t value, size_t index,
                            mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  const struct mvm_record *record = mvm_checked_record(mvm, value);
  if (!record || index >= record->length)
    return MVM_RESULT_RUNTIME_ERROR;
  *result = record->fields[index];
  return MVM_RESULT_OK;
}

mvm_result_t mvm_record_set(mvm_t *mvm, mvm_value_t target, size_t index,
                            mvm_value_t value) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  struct mvm_record *record = mvm_checked_record(mvm, target);
  if (!record || index >= record->length || value.kind == MVM_KIND_PACKET)
    return MVM_RESULT_RUNTIME_ERROR;
  record->fields[index] = value;
  return MVM_RESULT_OK;
}

bool mvm_type_va_allowed(const mvm_t *mvm, const uint32_t type_id) {
  const mvm_type_t *type = mvm_type_by_id(mvm, type_id);
  if (!type)
    return false;
  if (type->tag == MVM_TYPE_ENUM || type->tag == MVM_TYPE_REFERENCE ||
      type->tag == MVM_TYPE_FUNCTION || type->tag == MVM_TYPE_ARRAY ||
      type->tag == MVM_TYPE_AGGREGATE || type->tag == MVM_TYPE_UNION)
    return true;
  if (type->tag != MVM_TYPE_PRIMITIVE)
    return false;
  return (type->kind >= MVM_KIND_UBYTE && type->kind <= MVM_KIND_SLONG) ||
         type->kind == MVM_KIND_BOOL || type->kind == MVM_KIND_FLOAT ||
         type->kind == MVM_KIND_DOUBLE || type->kind == MVM_KIND_STRING ||
         type->kind == MVM_KIND_TYPE || type->kind == MVM_KIND_THREAD ||
         type->kind == MVM_KIND_MUTEX || type->kind == MVM_KIND_CONDITION ||
         type->kind == MVM_KIND_ATOMIC;
}

static bool mvm_reference_compatible(const mvm_t *mvm, const mvm_value_t value,
                                     const mvm_type_t *expected) {
  if (!value.has_full_type)
    return false;
  const mvm_type_t *actual = mvm_type_by_id(mvm, value.type_id);
  return actual && actual->tag == MVM_TYPE_REFERENCE &&
         actual->inner_type_id == expected->inner_type_id &&
         (!(expected->flags & MVM_TYPE_FLAG_MUTABLE) ||
          (actual->flags & MVM_TYPE_FLAG_MUTABLE));
}

static bool mvm_packet_value_matches(const mvm_t *mvm, const mvm_value_t value,
                                     const mvm_type_t *type) {
  if (value.kind == MVM_KIND_NULL)
    return mvm_nullable_kind(type->kind);
  if (type->tag == MVM_TYPE_AGGREGATE || type->tag == MVM_TYPE_ARRAY ||
      type->tag == MVM_TYPE_UNION) {
    const uint32_t id = (uint32_t)(type - mvm->types);
    return mvm_value_matches_type(mvm, id, value);
  }
  if (value.kind != type->kind)
    return false;
  if (type->tag == MVM_TYPE_REFERENCE)
    return mvm_reference_compatible(mvm, value, type);
  if (type->tag == MVM_TYPE_FUNCTION)
    return !value.has_full_type || mvm_type_by_id(mvm, value.type_id) == type;
  return mvm_sync_value_valid(mvm, value);
}

mvm_packet_t *mvm_checked_packet(const mvm_t *mvm, const mvm_value_t value) {
  if (value.kind != MVM_KIND_PACKET)
    return NULL;
  const mvm_object_t *object = mvm_gc_find(mvm, value.as.packet);
  if (!object || object->kind != MVM_OBJECT_PACKET ||
      (const void *)object->data != (const void *)value.as.packet)
    return NULL;
  return value.as.packet;
}

mvm_result_t mvm_variadic_count(const mvm_t *mvm, const mvm_value_t value,
                                size_t *count) {
  if (!mvm || !count)
    return MVM_RESULT_VM_ERROR;
  const mvm_packet_t *packet = mvm_checked_packet(mvm, value);
  if (!packet)
    return MVM_RESULT_RUNTIME_ERROR;
  *count = packet->count;
  return MVM_RESULT_OK;
}

mvm_result_t mvm_variadic_arg(mvm_t *mvm, const mvm_value_t value,
                              const size_t index, const uint32_t type_id,
                              mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  const mvm_packet_t *packet = mvm_checked_packet(mvm, value);
  if (!packet)
    return MVM_RESULT_RUNTIME_ERROR;
  return mvm_va_arg(mvm, packet, index, type_id, result);
}

mvm_result_t mvm_packet_new(mvm_t *mvm, const size_t count,
                            const mvm_value_t *values, const size_t types_pc,
                            mvm_value_t *result) {
  if (!mvm || !result || (count && !values))
    return MVM_RESULT_VM_ERROR;
  if (count > (SIZE_MAX - sizeof(mvm_packet_t)) / sizeof(mvm_value_t))
    return MVM_RESULT_VM_ERROR;
  for (size_t i = 0; i < count; ++i) {
    const uint32_t type_id = mvm_ram_peek_uword(mvm->ram, types_pc + i * 4);
    const mvm_type_t *type = mvm_type_by_id(mvm, type_id);
    if (!mvm_type_va_allowed(mvm, type_id) ||
        !mvm_packet_value_matches(mvm, values[i], type))
      return MVM_RESULT_RUNTIME_ERROR;
  }

  const size_t size = sizeof(mvm_packet_t) + count * sizeof(mvm_value_t);
  mvm_object_t *object = mvm_gc_allocate(mvm, MVM_OBJECT_PACKET, size);
  if (!object)
    return MVM_RESULT_VM_ERROR;
  mvm_packet_t *packet = (mvm_packet_t *)object->data;
  packet->count = count;
  for (size_t i = 0; i < count; ++i) {
    packet->values[i] = values[i];
    packet->values[i].type_id = mvm_ram_peek_uword(mvm->ram, types_pc + i * 4);
    packet->values[i].has_full_type = true;
  }
  *result = (mvm_value_t){.kind = MVM_KIND_PACKET, .as.packet = packet};
  return MVM_RESULT_OK;
}

static size_t mvm_va_integer_width(const mvm_kind_t kind) {
  switch (kind) {
  case MVM_KIND_UBYTE:
  case MVM_KIND_SBYTE: return 1;
  case MVM_KIND_UHALF:
  case MVM_KIND_SHALF: return 2;
  case MVM_KIND_UWORD:
  case MVM_KIND_SWORD: return 4;
  case MVM_KIND_ULONG:
  case MVM_KIND_SLONG: return 8;
  default: return 0;
  }
}

static uint64_t mvm_va_integer_bits(const mvm_value_t value) {
  switch (value.kind) {
  case MVM_KIND_UBYTE: return value.as.ub;
  case MVM_KIND_UHALF: return value.as.uh;
  case MVM_KIND_UWORD: return value.as.uw;
  case MVM_KIND_ULONG: return value.as.ul;
  case MVM_KIND_SBYTE: return (uint64_t)value.as.sb;
  case MVM_KIND_SHALF: return (uint64_t)value.as.sh;
  case MVM_KIND_SWORD: return (uint64_t)value.as.sw;
  case MVM_KIND_SLONG: return (uint64_t)value.as.sl;
  default: return 0;
  }
}

static void mvm_va_set_integer(mvm_value_t *value, const mvm_kind_t kind,
                               const uint64_t bits) {
  value->kind = kind;
  value->as.ul = 0;
  switch (kind) {
  case MVM_KIND_UBYTE: value->as.ub = (uint8_t)bits; break;
  case MVM_KIND_UHALF: value->as.uh = (uint16_t)bits; break;
  case MVM_KIND_UWORD: value->as.uw = (uint32_t)bits; break;
  case MVM_KIND_ULONG: value->as.ul = bits; break;
  case MVM_KIND_SBYTE: value->as.sb = (int8_t)bits; break;
  case MVM_KIND_SHALF: value->as.sh = (int16_t)bits; break;
  case MVM_KIND_SWORD: value->as.sw = (int32_t)bits; break;
  case MVM_KIND_SLONG: value->as.sl = (int64_t)bits; break;
  default: break;
  }
}

mvm_result_t mvm_va_arg(mvm_t *mvm, const mvm_packet_t *packet,
                        const size_t index, const uint32_t type_id,
                        mvm_value_t *result) {
  if (!mvm || !packet || !result)
    return MVM_RESULT_VM_ERROR;
  const mvm_type_t *expected = mvm_type_by_id(mvm, type_id);
  if (!mvm_type_va_allowed(mvm, type_id) || index >= packet->count)
    return MVM_RESULT_RUNTIME_ERROR;
  const mvm_value_t actual = packet->values[index];
  const mvm_type_t *actual_type = mvm_type_by_id(mvm, actual.type_id);
  if (!actual.has_full_type || !actual_type)
    return MVM_RESULT_RUNTIME_ERROR;

  if (actual.type_id == type_id) {
    *result = actual;
    return MVM_RESULT_OK;
  }
  if (expected->tag == MVM_TYPE_PRIMITIVE &&
      actual_type->tag == MVM_TYPE_PRIMITIVE &&
      mvm_va_integer_width(expected->kind) &&
      mvm_va_integer_width(expected->kind) <= mvm_va_integer_width(actual_type->kind)) {
    *result = actual;
    mvm_va_set_integer(result, expected->kind, mvm_va_integer_bits(actual));
    result->type_id = type_id;
    result->has_full_type = true;
    return MVM_RESULT_OK;
  }
  if (expected->tag == MVM_TYPE_REFERENCE &&
      mvm_reference_compatible(mvm, actual, expected)) {
    *result = actual;
    result->type_id = type_id;
    result->has_full_type = true;
    return MVM_RESULT_OK;
  }
  return MVM_RESULT_RUNTIME_ERROR;
}


mvm_result_t mvm_array_new_typed(mvm_t *mvm, uint32_t id, size_t length,
                                  mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  const mvm_type_t *type = mvm_type_by_id(mvm, id);
  if (!type || type->tag != MVM_TYPE_ARRAY || length > UINT32_MAX)
    return MVM_RESULT_RUNTIME_ERROR;
  const uint32_t element_id = type->inner_type_id;
  const mvm_type_t *element = mvm_type_by_id(mvm, element_id);
  if (!element || !element->size ||
      (length && !mvm_type_defaultable(mvm, element_id)))
    return MVM_RESULT_RUNTIME_ERROR;
  const mvm_kind_t kind = element->tag == MVM_TYPE_AGGREGATE
                              ? MVM_KIND_REFERENCE : element->kind;
  mvm_value_t value;
  // Start empty; no non-defaultable element is ever made observable.
  mvm_result_t status = mvm_array_new(mvm, kind == MVM_KIND_UNION
                                              ? MVM_KIND_REFERENCE : kind,
                                      0, &value);
  if (status != MVM_RESULT_OK)
    return status;
  value.type_id = id;
  value.has_full_type = true;
  mvm_array_t *array = value.as.array;
  array->kind = kind;
  array->element_type_id = element_id;
  mvm_gc_root_t root;
  if (!mvm_gc_root_register(mvm, &root, &value, 1))
    return MVM_RESULT_VM_ERROR;
  status = mvm_array_resize(mvm, value, length);
  mvm_gc_root_unregister(mvm, &root);
  if (status == MVM_RESULT_OK)
    *result = value;
  return status;
}
