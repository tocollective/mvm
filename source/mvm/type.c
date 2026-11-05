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

#include <stdlib.h>
#include <string.h>

static mvm_result_t mvm_full_load(const mvm_t *mvm, uint32_t type_id,
                                  const unsigned char *data, mvm_value_t *result);
static mvm_result_t mvm_full_store(const mvm_t *mvm, uint32_t type_id,
                                   unsigned char *data, mvm_value_t value);

static size_t mvm_typed_kind_width(const mvm_kind_t kind) {
  const size_t scalar_width = mvm_ram_kind_width(kind);
  if (scalar_width)
    return scalar_width;
  switch (kind) {
  case MVM_KIND_STRING:
  case MVM_KIND_ARRAY:
  case MVM_KIND_TYPE:
  case MVM_KIND_REFERENCE:
  case MVM_KIND_FUNC:
  case MVM_KIND_THREAD:
  case MVM_KIND_UNION:
  case MVM_KIND_MUTEX:
  case MVM_KIND_CONDITION:
  case MVM_KIND_ATOMIC:
    return sizeof(uint64_t);
  default:
    return 0;
  }
}

static bool mvm_type_reference_kind(const mvm_kind_t kind) {
  return kind == MVM_KIND_STRING || kind == MVM_KIND_ARRAY ||
         kind == MVM_KIND_REFERENCE || kind == MVM_KIND_THREAD ||
         kind == MVM_KIND_UNION || kind == MVM_KIND_MUTEX ||
         kind == MVM_KIND_CONDITION || kind == MVM_KIND_ATOMIC;
}

static const mvm_type_reference_t *
mvm_type_find_reference(const mvm_type_desc_t *desc, const size_t offset) {
  size_t first = 0;
  size_t count = desc->reference_count;
  while (count) {
    const size_t step = count / 2;
    const size_t index = first + step;
    const mvm_type_reference_t *reference = &desc->references[index];
    if (reference->offset < offset) {
      first = index + 1;
      count -= step + 1;
    } else {
      count = step;
    }
  }
  if (first < desc->reference_count &&
      desc->references[first].offset == offset)
    return &desc->references[first];
  return NULL;
}

static bool mvm_integer_kind(const mvm_kind_t kind) {
  return kind >= MVM_KIND_UBYTE && kind <= MVM_KIND_SLONG;
}

static mvm_kind_t mvm_type_intrinsic_reference(const mvm_type_desc_t *desc) {
  if (desc->tag == MVM_TYPE_REFERENCE || desc->tag == MVM_TYPE_ARRAY ||
      desc->tag == MVM_TYPE_UNION)
    return desc->kind;
  if (desc->tag == MVM_TYPE_PRIMITIVE && mvm_type_reference_kind(desc->kind))
    return desc->kind;
  return MVM_KIND_VOID;
}

static bool mvm_type_valid(const mvm_t *mvm, const mvm_type_desc_t *desc) {
  if (!desc->alignment || desc->alignment > _Alignof(max_align_t) ||
      (desc->alignment & (desc->alignment - 1)) != 0 ||
      (desc->field_count && !desc->fields) ||
      (desc->reference_count && !desc->references) ||
      desc->field_count > (size_t)UINT16_MAX + 1 || desc->tag > MVM_TYPE_UNION)
    return false;

  const mvm_kind_t intrinsic = mvm_type_intrinsic_reference(desc);
  if (desc->tag != MVM_TYPE_AGGREGATE && desc->reference_count &&
      (desc->reference_count != 1 || !intrinsic ||
       desc->references[0].offset != 0 || desc->references[0].kind != intrinsic))
    return false;

  for (size_t i = 0; i < desc->reference_count; ++i) {
    const mvm_type_reference_t *reference = &desc->references[i];
    if (!mvm_type_reference_kind(reference->kind) ||
        reference->offset > desc->size ||
        sizeof(uint64_t) > desc->size - reference->offset ||
        (i && reference->offset <= desc->references[i - 1].offset))
      return false;
  }

  for (size_t i = 0; i < desc->field_count; ++i) {
    const mvm_type_field_t *field = &desc->fields[i];
    const size_t width = field->kind == MVM_KIND_VOID
                             ? field->size
                             : mvm_typed_kind_width(field->kind);
    if (!width || field->size != width || field->offset > desc->size ||
        width > desc->size - field->offset ||
        (i && field->offset < desc->fields[i - 1].offset +
                                  desc->fields[i - 1].size) ||
        (field->has_type_id && (size_t)field->type_id >= mvm->type_count))
      return false;
    if (mvm_type_reference_kind(field->kind)) {
      const mvm_type_reference_t *reference =
          mvm_type_find_reference(desc, field->offset);
      if (!reference || reference->kind != field->kind)
        return false;
    }
  }

  if (desc->tag != MVM_TYPE_UNION &&
      (desc->variant_count || desc->variant_type_ids))
    return false;
  if (desc->tag == MVM_TYPE_UNION) {
    if (desc->kind != MVM_KIND_UNION || desc->size != 8 || desc->alignment != 8 ||
        desc->field_count || desc->flags || desc->argc || desc->param_type_ids ||
        desc->variant_count < 2 || desc->variant_count > 4096 ||
        !desc->variant_type_ids)
      return false;
    for (size_t i = 0; i < desc->variant_count; ++i) {
      const uint32_t id = desc->variant_type_ids[i];
      if (id >= mvm->type_count || !mvm->types[id].size ||
          mvm->types[id].tag == MVM_TYPE_UNION)
        return false;
      for (size_t j = 0; j < i; ++j)
        if (desc->variant_type_ids[j] == id)
          return false;
    }
    return true;
  }
  if (desc->tag == MVM_TYPE_AGGREGATE)
    return desc->size && desc->kind == MVM_KIND_VOID && !desc->flags &&
           !desc->argc && !desc->param_type_ids;
  if (desc->field_count ||
      (desc->param_type_ids && desc->tag != MVM_TYPE_FUNCTION))
    return false;

  if (desc->tag == MVM_TYPE_PRIMITIVE) {
    if (desc->kind == MVM_KIND_VOID)
      return !desc->size && desc->alignment == 1 && !desc->flags && !desc->argc;
    const size_t width = mvm_typed_kind_width(desc->kind);
    const bool nullable = (desc->flags & MVM_TYPE_FLAG_NULLABLE) != 0;
    return desc->kind > MVM_KIND_NULL && desc->kind < MVM_KIND_COUNT &&
           desc->kind != MVM_KIND_PACKET && desc->kind != MVM_KIND_UNION &&
           width && desc->size == width &&
           !(desc->flags & ~MVM_TYPE_FLAG_NULLABLE) &&
           (!nullable || mvm_nullable_kind(desc->kind)) && !desc->argc;
  }
  if (desc->tag == MVM_TYPE_ENUM)
    return mvm_integer_kind(desc->kind) &&
           desc->size == mvm_typed_kind_width(desc->kind) && !desc->flags &&
           !desc->argc;
  if (desc->size != sizeof(uint64_t) || desc->alignment != sizeof(uint64_t))
    return false;
  if (desc->tag == MVM_TYPE_REFERENCE)
    return desc->kind == MVM_KIND_REFERENCE &&
           (size_t)desc->inner_type_id < mvm->type_count &&
           mvm->types[desc->inner_type_id].size &&
           !(desc->flags & ~(MVM_TYPE_FLAG_MUTABLE | MVM_TYPE_FLAG_NULLABLE)) &&
           !desc->argc;
  if (desc->tag == MVM_TYPE_ARRAY)
    return desc->kind == MVM_KIND_ARRAY &&
           (size_t)desc->inner_type_id < mvm->type_count &&
           mvm->types[desc->inner_type_id].size &&
           !(desc->flags & ~(MVM_TYPE_FLAG_MUTABLE | MVM_TYPE_FLAG_NULLABLE)) &&
           !desc->argc;
  if (desc->tag == MVM_TYPE_FUNCTION) {
    if (desc->kind != MVM_KIND_FUNC ||
        (desc->flags & ~(MVM_TYPE_FLAG_VARIADIC | MVM_TYPE_FLAG_NULLABLE)) ||
        desc->argc > UINT8_MAX ||
        ((desc->flags & MVM_TYPE_FLAG_VARIADIC) && desc->argc >= UINT8_MAX) ||
        (desc->argc && !desc->param_type_ids) ||
        (size_t)desc->result_type_id >= mvm->type_count)
      return false;
    for (size_t i = 0; i < desc->argc; ++i) {
      if ((size_t)desc->param_type_ids[i] >= mvm->type_count ||
          !mvm->types[desc->param_type_ids[i]].size)
        return false;
    }
    return true;
  }
  return false;
}

static void mvm_type_clear(mvm_type_t *type) {
  mvm_memory_free(type->fields);
  mvm_memory_free(type->references);
  mvm_memory_free(type->param_type_ids);
  mvm_memory_free(type->variant_type_ids);
}

mvm_result_t mvm_type_register(mvm_t *mvm, const mvm_type_desc_t *desc,
                               uint32_t *id) {
  if (!mvm || !desc || mvm->type_count > UINT32_MAX)
    return MVM_RESULT_VM_ERROR;
  if (!mvm_type_valid(mvm, desc))
    return MVM_RESULT_VM_ERROR;

  const mvm_kind_t intrinsic = mvm_type_intrinsic_reference(desc);
  mvm_type_t type = {
      .size = desc->size,
      .alignment = desc->alignment,
      .field_count = desc->field_count,
      .reference_count = desc->reference_count || intrinsic ? 1 : 0,
      .tag = desc->tag,
      .kind = desc->kind,
      .inner_type_id = desc->inner_type_id,
      .flags = desc->flags,
      .argc = desc->argc,
      .result_type_id = desc->result_type_id,
      .variant_count = desc->variant_count,
  };
  if (desc->field_count) {
    if (desc->field_count > SIZE_MAX / sizeof(*type.fields))
      return MVM_RESULT_VM_ERROR;
    type.fields =
        mvm_memory_alloc(mvm->memory, desc->field_count * sizeof(*type.fields));
    if (!type.fields)
      return MVM_RESULT_VM_ERROR;
    memcpy(type.fields, desc->fields,
           desc->field_count * sizeof(*type.fields));
  }
  if (type.reference_count) {
    type.references = mvm_memory_alloc(mvm->memory, sizeof(*type.references));
    if (!type.references) {
      mvm_type_clear(&type);
      return MVM_RESULT_VM_ERROR;
    }
    type.references[0] = desc->reference_count
                             ? desc->references[0]
                             : (mvm_type_reference_t){.offset = 0, .kind = intrinsic};
  }
  if (desc->reference_count > 1) {
    mvm_type_reference_t *references = mvm_memory_alloc(
        mvm->memory, desc->reference_count * sizeof(*type.references));
    if (!references) {
      mvm_type_clear(&type);
      return MVM_RESULT_VM_ERROR;
    }
    memcpy(references, desc->references,
           desc->reference_count * sizeof(*type.references));
    mvm_memory_free(type.references);
    type.references = references;
    type.reference_count = desc->reference_count;
  }
  if (desc->argc) {
    type.param_type_ids = mvm_memory_alloc(
        mvm->memory, desc->argc * sizeof(*type.param_type_ids));
    if (!type.param_type_ids) {
      mvm_type_clear(&type);
      return MVM_RESULT_VM_ERROR;
    }
    memcpy(type.param_type_ids, desc->param_type_ids,
           desc->argc * sizeof(*type.param_type_ids));
  }

  if (desc->variant_count) {
    type.variant_type_ids =
        mvm_memory_alloc(mvm->memory, desc->variant_count * sizeof(uint32_t));
    if (!type.variant_type_ids) {
      mvm_type_clear(&type);
      return MVM_RESULT_VM_ERROR;
    }
    memcpy(type.variant_type_ids, desc->variant_type_ids,
           desc->variant_count * sizeof(uint32_t));
  }

  if (mvm->type_count == mvm->type_capacity) {
    if (mvm->type_capacity > SIZE_MAX / 2 ||
        (mvm->type_capacity ? mvm->type_capacity * 2 : 8) >
            SIZE_MAX / sizeof(*mvm->types)) {
      mvm_type_clear(&type);
      return MVM_RESULT_VM_ERROR;
    }
    const size_t capacity = mvm->type_capacity ? mvm->type_capacity * 2 : 8;
    mvm_type_t *types = mvm_memory_realloc(mvm->memory, mvm->types,
                                           capacity * sizeof(*mvm->types));
    if (!types) {
      mvm_type_clear(&type);
      return MVM_RESULT_VM_ERROR;
    }
    mvm->types = types;
    mvm->type_capacity = capacity;
  }

  const uint32_t type_id = (uint32_t)mvm->type_count;
  mvm->types[mvm->type_count++] = type;
  if (id)
    *id = type_id;
  return MVM_RESULT_OK;
}

mvm_result_t mvm_type_clone(mvm_t *destination, const mvm_t *source) {
  if (!destination || !source || destination == source ||
      destination->type_count)
    return MVM_RESULT_VM_ERROR;
  if (source->type_count > SIZE_MAX / sizeof(mvm_type_desc_t) ||
      source->function_type_count > SIZE_MAX / sizeof(mvm_function_type_t))
    return MVM_RESULT_VM_ERROR;
  mvm_type_desc_t *descs = mvm_memory_calloc(
      destination->memory, source->type_count, sizeof(*descs));
  mvm_function_type_t *functions =
      source->function_type_count
          ? mvm_memory_alloc(destination->memory,
                             source->function_type_count * sizeof(*functions))
          : NULL;
  if ((source->type_count && !descs) ||
      (source->function_type_count && !functions)) {
    mvm_memory_free(descs);
    mvm_memory_free(functions);
    return MVM_RESULT_VM_ERROR;
  }
  for (size_t i = 0; i < source->type_count; ++i) {
    const mvm_type_t *type = &source->types[i];
    descs[i] = (mvm_type_desc_t) {
        .size = type->size,
        .alignment = type->alignment,
        .field_count = type->field_count,
        .fields = type->fields,
        .reference_count = type->reference_count,
        .references = type->references,
        .tag = type->tag,
        .kind = type->kind,
        .inner_type_id = type->inner_type_id,
        .flags = type->flags,
        .argc = type->argc,
        .param_type_ids = type->param_type_ids,
        .result_type_id = type->result_type_id,
        .variant_count = type->variant_count,
        .variant_type_ids = type->variant_type_ids,
    };
  }
  const mvm_result_t status =
      mvm_type_load(destination, descs, source->type_count);
  mvm_memory_free(descs);
  if (status != MVM_RESULT_OK) {
    mvm_memory_free(functions);
    return status;
  }
  if (source->function_type_count)
    memcpy(functions, source->function_types, source->function_type_count * sizeof(*functions));
  destination->function_types = functions;
  destination->function_type_count = source->function_type_count;
  return MVM_RESULT_OK;
}

const mvm_type_t *mvm_type_by_id(const mvm_t *mvm, const uint32_t type_id) {
  if (!mvm || (size_t)type_id >= mvm->type_count)
    return NULL;
  return &mvm->types[type_id];
}

static const mvm_type_t *mvm_find_type(const mvm_t *mvm,
                                       const uint32_t type_id) {
  return mvm_type_by_id(mvm, type_id);
}

mvm_object_t *mvm_typed_object(const mvm_t *mvm,
                                      const mvm_value_t value) {
  if (value.kind != MVM_KIND_REFERENCE || !value.as.ptr)
    return NULL;
  mvm_object_t *object = mvm_gc_find(mvm, value.as.ptr);
  if (!object || object->kind != MVM_OBJECT_TYPED ||
      object->data != value.as.ptr)
    return NULL;
  return object;
}

static mvm_result_t mvm_typed_memory(const mvm_t *mvm, const uint32_t type_id,
                                     const mvm_value_t value,
                                     unsigned char **data) {
  const mvm_type_t *type = mvm_find_type(mvm, type_id);
  if (!type || value.kind != MVM_KIND_REFERENCE || !value.as.ptr)
    return MVM_RESULT_RUNTIME_ERROR;
  const mvm_object_t *object = mvm_gc_find(mvm, value.as.ptr);
  if (!object || object->kind != MVM_OBJECT_TYPED)
    return MVM_RESULT_RUNTIME_ERROR;
  const size_t offset = (uintptr_t)value.as.ptr - (uintptr_t)object->data;
  if (type->size > object->size - offset)
    return MVM_RESULT_RUNTIME_ERROR;
  *data = (unsigned char *)value.as.ptr;
  return MVM_RESULT_OK;
}

static void mvm_typed_default(const mvm_t *mvm, const uint32_t type_id,
                              unsigned char *data) {
  const mvm_type_t *type = mvm_find_type(mvm, type_id);
  memset(data, 0, type->size);
  if (type->tag == MVM_TYPE_FUNCTION) {
    mvm_write_u64(data, UINT64_MAX);
    return;
  }
  if (type->tag != MVM_TYPE_AGGREGATE)
    return;
  for (size_t i = 0; i < type->field_count; ++i) {
    const mvm_type_field_t *field = &type->fields[i];
    if (field->kind == MVM_KIND_FUNC) {
      mvm_write_u64(data + field->offset, UINT64_MAX);
    } else if (field->kind == MVM_KIND_VOID && field->has_type_id) {
      mvm_typed_default(mvm, field->type_id, data + field->offset);
    }
  }
}

mvm_result_t mvm_typed_construct(mvm_t *mvm, const uint32_t type_id,
                             mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  const mvm_type_t *type = mvm_find_type(mvm, type_id);
  if (!type || !type->size)
    return MVM_RESULT_RUNTIME_ERROR;
  mvm_object_t *object =
      mvm_gc_allocate(mvm, MVM_OBJECT_TYPED, type->size);
  if (!object)
    return MVM_RESULT_VM_ERROR;
  object->type_id = type_id;
  mvm_typed_default(mvm, type_id, object->data);
  *result =
      (mvm_value_t){.kind = MVM_KIND_REFERENCE, .as.ptr = object->data,
                    .type_id = type_id, .has_full_type = true};
  return MVM_RESULT_OK;
}

mvm_result_t mvm_typed_alloc(mvm_t *mvm, const uint32_t type_id,
                             mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  if (!mvm_type_defaultable(mvm, type_id))
    return MVM_RESULT_RUNTIME_ERROR;
  return mvm_typed_construct(mvm, type_id, result);
}

mvm_result_t mvm_typed_zero(mvm_t *mvm, const uint32_t type_id,
                            const mvm_value_t destination) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  if (!mvm_type_defaultable(mvm, type_id))
    return MVM_RESULT_RUNTIME_ERROR;
  unsigned char *data;
  const mvm_result_t status = mvm_typed_memory(mvm, type_id, destination, &data);
  if (status != MVM_RESULT_OK)
    return status;
  mvm_typed_default(mvm, type_id, data);
  return MVM_RESULT_OK;
}

mvm_result_t mvm_typed_copy(mvm_t *mvm, const uint32_t type_id,
                            const mvm_value_t destination,
                            const mvm_value_t source) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  const mvm_type_t *type = mvm_find_type(mvm, type_id);
  unsigned char *target_data;
  unsigned char *source_data;
  mvm_result_t status = mvm_typed_memory(mvm, type_id, destination, &target_data);
  if (status != MVM_RESULT_OK)
    return status;
  status = mvm_typed_memory(mvm, type_id, source, &source_data);
  if (status != MVM_RESULT_OK)
    return status;
  if (type->tag == MVM_TYPE_AGGREGATE && !mvm_value_matches_type(mvm, type_id, source))
    return MVM_RESULT_RUNTIME_ERROR;
  memmove(target_data, source_data, type->size);
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_typed_load(const unsigned char *data,
                                   const mvm_kind_t kind,
                                   mvm_value_t *result) {
  const size_t scalar_width = mvm_ram_kind_width(kind);
  if (scalar_width) {
    mvm_ram_t view = {.data = (uint8_t *)data, .size = scalar_width};
    return mvm_load_ram(&view, 0, kind, result);
  }

  const uint64_t bits = mvm_read_u64(data);
  if ((!bits && kind != MVM_KIND_FUNC && kind != MVM_KIND_TYPE) ||
      (bits == UINT64_MAX && kind == MVM_KIND_FUNC)) {
    *result = (mvm_value_t){.kind = MVM_KIND_NULL};
    return MVM_RESULT_OK;
  }
  if ((uint64_t)(uintptr_t)bits != bits)
    return MVM_RESULT_RUNTIME_ERROR;

  *result = (mvm_value_t){.kind = kind};
  switch (kind) {
  case MVM_KIND_STRING:
    result->as.s = (char *)(uintptr_t)bits;
    break;
  case MVM_KIND_ARRAY:
    result->as.array = (mvm_array_t *)(uintptr_t)bits;
    break;
  case MVM_KIND_REFERENCE:
    result->as.ptr = (void *)(uintptr_t)bits;
    break;
  case MVM_KIND_TYPE:
    result->as.type = (uint32_t)bits;
    result->has_full_type = true;
    break;
  case MVM_KIND_FUNC:
    if (bits > UINT32_MAX)
      return MVM_RESULT_RUNTIME_ERROR;
    result->as.func = (uint32_t)bits;
    break;
  case MVM_KIND_THREAD:
    result->as.thread = (mvm_thread_t *)(uintptr_t)bits;
    break;
  case MVM_KIND_UNION:
    result->as.union_value = (mvm_union_t *)(uintptr_t)bits;
    break;
  case MVM_KIND_MUTEX:
    result->as.mutex = (mvm_mutex_t *)(uintptr_t)bits;
    break;
  case MVM_KIND_CONDITION:
    result->as.condition = (mvm_condition_t *)(uintptr_t)bits;
    break;
  case MVM_KIND_ATOMIC:
    result->as.atomic = (mvm_atomic_t *)(uintptr_t)bits;
    break;
  default:
    return MVM_RESULT_RUNTIME_ERROR;
  }
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_typed_store(const mvm_t *mvm, unsigned char *data,
                                    const mvm_type_field_t *field,
                                    const mvm_value_t value) {
  if (value.kind == MVM_KIND_NULL) {
    if (field->has_type_id) {
      const mvm_type_t *type = mvm_type_by_id(mvm, field->type_id);
      if (!type || !(type->flags & MVM_TYPE_FLAG_NULLABLE))
        return MVM_RESULT_RUNTIME_ERROR;
    } else if (!mvm_nullable_kind(field->kind)) {
      return MVM_RESULT_RUNTIME_ERROR;
    }
  }
  if (value.kind != field->kind &&
      !(value.kind == MVM_KIND_NULL && mvm_nullable_kind(field->kind)))
    return MVM_RESULT_RUNTIME_ERROR;

  const size_t scalar_width = mvm_ram_kind_width(field->kind);
  if (scalar_width) {
    mvm_ram_t view = {.data = data, .size = scalar_width};
    return mvm_store_ram(&view, 0, value) ? MVM_RESULT_OK
                                          : MVM_RESULT_RUNTIME_ERROR;
  }

  uint64_t bits = value.kind == MVM_KIND_NULL && field->kind == MVM_KIND_FUNC
                      ? UINT64_MAX
                      : 0;
  if (value.kind != MVM_KIND_NULL) {
    switch (field->kind) {
    case MVM_KIND_UNION:
      if (!field->has_type_id || !mvm_value_matches_type(mvm, field->type_id, value))
        return MVM_RESULT_RUNTIME_ERROR;
      bits = (uint64_t)(uintptr_t)value.as.union_value;
      break;
    case MVM_KIND_STRING:
      bits = (uint64_t)(uintptr_t)value.as.s;
      break;
    case MVM_KIND_ARRAY:
      if (!mvm_checked_array(mvm, value))
        return MVM_RESULT_RUNTIME_ERROR;
      bits = (uint64_t)(uintptr_t)value.as.array;
      break;
    case MVM_KIND_REFERENCE:
      bits = (uint64_t)(uintptr_t)value.as.ptr;
      break;
    case MVM_KIND_TYPE:
      bits = value.as.type;
      break;
    case MVM_KIND_FUNC:
      bits = value.as.func;
      break;
    case MVM_KIND_THREAD:
      if (!mvm_checked_thread(mvm, value))
        return MVM_RESULT_RUNTIME_ERROR;
      bits = (uint64_t)(uintptr_t)value.as.thread;
      break;
    case MVM_KIND_MUTEX:
      if (!mvm_checked_mutex(mvm, value))
        return MVM_RESULT_RUNTIME_ERROR;
      bits = (uint64_t)(uintptr_t)value.as.mutex;
      break;
    case MVM_KIND_CONDITION:
      if (!mvm_checked_condition(mvm, value))
        return MVM_RESULT_RUNTIME_ERROR;
      bits = (uint64_t)(uintptr_t)value.as.condition;
      break;
    case MVM_KIND_ATOMIC:
      if (!mvm_checked_atomic(mvm, value))
        return MVM_RESULT_RUNTIME_ERROR;
      bits = (uint64_t)(uintptr_t)value.as.atomic;
      break;
    default:
      return MVM_RESULT_RUNTIME_ERROR;
    }
  }
  mvm_write_u64(data, bits);
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_aggregate_memory(const mvm_t *mvm, const mvm_value_t value,
                                         const mvm_type_t **type,
                                         unsigned char **data) {
  const mvm_object_t *object = mvm_typed_object(mvm, value);
  const mvm_type_t *value_type = value.has_full_type
                                     ? mvm_find_type(mvm, value.type_id)
                                     : NULL;
  const bool whole_object =
      object && (!value.has_full_type || value.type_id == object->type_id ||
                 (value_type && value_type->tag == MVM_TYPE_REFERENCE &&
                  value_type->inner_type_id == object->type_id));
  if (whole_object) {
    *type = mvm_find_type(mvm, object->type_id);
    if (!*type || (*type)->tag != MVM_TYPE_AGGREGATE)
      return MVM_RESULT_RUNTIME_ERROR;
    *data = (unsigned char *)value.as.ptr;
    return MVM_RESULT_OK;
  }

  if (!value.has_full_type)
    return MVM_RESULT_RUNTIME_ERROR;
  uint32_t aggregate_id = value.type_id;
  if (value_type && value_type->tag == MVM_TYPE_REFERENCE)
    aggregate_id = value_type->inner_type_id;
  *type = mvm_find_type(mvm, aggregate_id);
  if (!*type || (*type)->tag != MVM_TYPE_AGGREGATE ||
      !mvm_value_matches_type(mvm, value.type_id, value))
    return MVM_RESULT_RUNTIME_ERROR;
  return mvm_typed_memory(mvm, aggregate_id, value, data);
}

mvm_result_t mvm_typed_field_get(const mvm_t *mvm, const mvm_value_t value,
                                 const size_t index, mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  const mvm_type_t *type;
  unsigned char *data;
  const mvm_result_t status = mvm_aggregate_memory(mvm, value, &type, &data);
  if (status != MVM_RESULT_OK)
    return status;
  if (index >= type->field_count)
    return MVM_RESULT_RUNTIME_ERROR;
  const mvm_type_field_t *field = &type->fields[index];
  if (field->has_type_id)
    return mvm_full_load(mvm, field->type_id, data + field->offset, result);
  if (field->kind == MVM_KIND_VOID)
    return MVM_RESULT_RUNTIME_ERROR;
  mvm_value_t loaded;
  const mvm_result_t load_status =
      mvm_typed_load(data + field->offset, field->kind, &loaded);
  if (load_status != MVM_RESULT_OK)
    return load_status;
  *result = loaded;
  return MVM_RESULT_OK;
}

mvm_result_t mvm_typed_field_set(mvm_t *mvm, const mvm_value_t value,
                                 const size_t index, const mvm_value_t field) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  const mvm_type_t *type;
  unsigned char *data;
  const mvm_result_t status = mvm_aggregate_memory(mvm, value, &type, &data);
  if (status != MVM_RESULT_OK)
    return status;
  if (index >= type->field_count)
    return MVM_RESULT_RUNTIME_ERROR;
  const mvm_type_field_t *descriptor = &type->fields[index];
  if (descriptor->has_type_id)
    return mvm_full_store(mvm, descriptor->type_id,
                          data + descriptor->offset, field);
  if (descriptor->kind == MVM_KIND_VOID)
    return MVM_RESULT_RUNTIME_ERROR;
  return mvm_typed_store(mvm, data + descriptor->offset, descriptor, field);
}

mvm_result_t mvm_typed_field_address(mvm_t *mvm, const mvm_value_t value,
                                     const size_t index, mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  const mvm_type_t *type;
  unsigned char *data;
  const mvm_result_t status = mvm_aggregate_memory(mvm, value, &type, &data);
  if (status != MVM_RESULT_OK)
    return status;
  if (index >= type->field_count)
    return MVM_RESULT_RUNTIME_ERROR;
  *result = (mvm_value_t){
      .kind = MVM_KIND_REFERENCE,
      .as.ptr = data + type->fields[index].offset,
  };
  const mvm_type_field_t *field = &type->fields[index];
  if (field->has_type_id) {
    const mvm_type_t *reference_type = value.has_full_type
                                           ? mvm_find_type(mvm, value.type_id)
                                           : NULL;
    const bool mutable = reference_type &&
                         reference_type->tag == MVM_TYPE_REFERENCE &&
                         (reference_type->flags & MVM_TYPE_FLAG_MUTABLE);
    uint32_t reference_type_id;
    const mvm_result_t status =
        mvm_reference_type(mvm, field->type_id, mutable, &reference_type_id);
    if (status != MVM_RESULT_OK)
      return status;
    result->type_id = reference_type_id;
    result->has_full_type = true;
  }
  return MVM_RESULT_OK;
}

mvm_result_t mvm_reference_type(mvm_t *mvm, const uint32_t pointee_type_id,
                                const bool mutable, uint32_t *type_id) {
  if (!mvm || !type_id)
    return MVM_RESULT_VM_ERROR;
  const mvm_type_t *pointee = mvm_find_type(mvm, pointee_type_id);
  if (!pointee || !pointee->size)
    return MVM_RESULT_RUNTIME_ERROR;
  const uint32_t flags = mutable ? MVM_TYPE_FLAG_MUTABLE : 0;
  for (size_t i = 0; i < mvm->type_count; ++i) {
    const mvm_type_t *type = &mvm->types[i];
    if (type->tag == MVM_TYPE_REFERENCE && type->inner_type_id == pointee_type_id &&
        type->flags == flags) {
      *type_id = (uint32_t)i;
      return MVM_RESULT_OK;
    }
  }
  const mvm_type_desc_t desc = {
      .size = sizeof(uint64_t),
      .alignment = sizeof(uint64_t),
      .tag = MVM_TYPE_REFERENCE,
      .kind = MVM_KIND_REFERENCE,
      .inner_type_id = pointee_type_id,
      .flags = flags,
  };
  return mvm_type_register(mvm, &desc, type_id);
}

static mvm_result_t mvm_reference_memory(const mvm_t *mvm,
                                         const mvm_type_t *reference_type,
                                         const mvm_value_t value,
                                         unsigned char **data) {
  if (value.kind != MVM_KIND_REFERENCE || !value.as.ptr || !value.has_full_type)
    return MVM_RESULT_RUNTIME_ERROR;
  const mvm_type_t *actual = mvm_find_type(mvm, value.type_id);
  if (!actual || actual->tag != MVM_TYPE_REFERENCE ||
      actual->inner_type_id != reference_type->inner_type_id ||
      ((reference_type->flags & MVM_TYPE_FLAG_MUTABLE) &&
       !(actual->flags & MVM_TYPE_FLAG_MUTABLE)))
    return MVM_RESULT_RUNTIME_ERROR;

  const mvm_object_t *object = mvm_gc_find(mvm, value.as.ptr);
  if (!object || object->kind != MVM_OBJECT_TYPED)
    return MVM_RESULT_RUNTIME_ERROR;
  const size_t offset = (uintptr_t)value.as.ptr - (uintptr_t)object->data;
  const mvm_type_t *pointee = mvm_find_type(mvm, reference_type->inner_type_id);
  if (!pointee || offset > object->size || pointee->size > object->size - offset)
    return MVM_RESULT_RUNTIME_ERROR;
  if (offset == 0 && object->type_id == reference_type->inner_type_id) {
    *data = (unsigned char *)value.as.ptr;
    return MVM_RESULT_OK;
  }

  const mvm_type_t *owner = mvm_find_type(mvm, object->type_id);
  if (!owner)
    return MVM_RESULT_RUNTIME_ERROR;
  for (size_t i = 0; i < owner->field_count; ++i) {
    const mvm_type_field_t *field = &owner->fields[i];
    if (field->offset == offset && field->has_type_id &&
        field->type_id == reference_type->inner_type_id) {
      *data = (unsigned char *)value.as.ptr;
      return MVM_RESULT_OK;
    }
  }
  return MVM_RESULT_RUNTIME_ERROR;
}

static mvm_result_t mvm_full_load(const mvm_t *mvm, const uint32_t type_id,
                                  const unsigned char *data, mvm_value_t *result) {
  const mvm_type_t *type = mvm_find_type(mvm, type_id);
  if (!type || !type->size)
    return MVM_RESULT_RUNTIME_ERROR;
  if (type->tag == MVM_TYPE_AGGREGATE) {
    *result = (mvm_value_t){.kind = MVM_KIND_REFERENCE,
                            .as.ptr = (void *)(uintptr_t)data,
                            .type_id = type_id,
                            .has_full_type = true};
    return MVM_RESULT_OK;
  }
  const mvm_result_t status = mvm_typed_load(data, type->kind, result);
  if (status != MVM_RESULT_OK)
    return status;
  result->type_id = type_id;
  result->has_full_type = true;
  if (type->tag == MVM_TYPE_UNION &&
      !mvm_value_matches_type(mvm, type_id, *result))
    return MVM_RESULT_RUNTIME_ERROR;
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_full_store(const mvm_t *mvm, const uint32_t type_id,
                                   unsigned char *data, const mvm_value_t value) {
  const mvm_type_t *type = mvm_find_type(mvm, type_id);
  if (!type || !type->size || value.kind == MVM_KIND_PACKET)
    return MVM_RESULT_RUNTIME_ERROR;
  if (type->tag == MVM_TYPE_AGGREGATE) {
    if (value.kind != MVM_KIND_REFERENCE || !value.has_full_type)
      return MVM_RESULT_RUNTIME_ERROR;
    if (value.type_id != type_id) {
      const mvm_type_t *actual = mvm_find_type(mvm, value.type_id);
      if (!actual || actual->tag != MVM_TYPE_REFERENCE ||
          actual->inner_type_id != type_id)
        return MVM_RESULT_RUNTIME_ERROR;
    }
    unsigned char *source;
    const mvm_result_t status = mvm_typed_memory(mvm, type_id, value, &source);
    if (status != MVM_RESULT_OK)
      return status;
    if (!mvm_value_matches_type(mvm, type_id, value))
      return MVM_RESULT_RUNTIME_ERROR;
    memmove(data, source, type->size);
    return MVM_RESULT_OK;
  }
  if (value.kind != MVM_KIND_NULL &&
      (type->tag == MVM_TYPE_REFERENCE || type->tag == MVM_TYPE_ARRAY ||
       type->tag == MVM_TYPE_FUNCTION)) {
    if (!value.has_full_type) {
      if (type->tag == MVM_TYPE_REFERENCE)
        return MVM_RESULT_RUNTIME_ERROR;
    } else {
      const mvm_type_t *actual = mvm_find_type(mvm, value.type_id);
      if (!actual || actual->tag != type->tag)
        return MVM_RESULT_RUNTIME_ERROR;
      if (type->tag == MVM_TYPE_REFERENCE) {
        if (actual->inner_type_id != type->inner_type_id ||
            ((type->flags & MVM_TYPE_FLAG_MUTABLE) &&
             !(actual->flags & MVM_TYPE_FLAG_MUTABLE)))
          return MVM_RESULT_RUNTIME_ERROR;
      } else if (value.type_id != type_id) {
        return MVM_RESULT_RUNTIME_ERROR;
      }
    }
  }
  const mvm_type_field_t field = {
      .size = type->size,
      .kind = type->kind,
      .type_id = type_id,
      .has_type_id = true,
  };
  return mvm_typed_store(mvm, data, &field, value);
}

mvm_result_t mvm_ref_read(mvm_t *mvm, const uint32_t reference_type_id,
                          const mvm_value_t reference, mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  const mvm_type_t *reference_type = mvm_find_type(mvm, reference_type_id);
  if (!reference_type || reference_type->tag != MVM_TYPE_REFERENCE)
    return MVM_RESULT_RUNTIME_ERROR;
  unsigned char *data;
  const mvm_result_t status =
      mvm_reference_memory(mvm, reference_type, reference, &data);
  if (status != MVM_RESULT_OK)
    return status;
  return mvm_full_load(mvm, reference_type->inner_type_id, data, result);
}

mvm_result_t mvm_ref_write(mvm_t *mvm, const uint32_t reference_type_id,
                           const mvm_value_t reference, const mvm_value_t value) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  const mvm_type_t *reference_type = mvm_find_type(mvm, reference_type_id);
  if (!reference_type || reference_type->tag != MVM_TYPE_REFERENCE ||
      !(reference_type->flags & MVM_TYPE_FLAG_MUTABLE))
    return MVM_RESULT_RUNTIME_ERROR;
  unsigned char *data;
  const mvm_result_t status =
      mvm_reference_memory(mvm, reference_type, reference, &data);
  if (status != MVM_RESULT_OK)
    return status;
  return mvm_full_store(mvm, reference_type->inner_type_id, data, value);
}

mvm_result_t mvm_typed_init(mvm_t *mvm, const uint32_t pointee_type_id,
                            const mvm_value_t value, mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  const mvm_type_t *pointee = mvm_find_type(mvm, pointee_type_id);
  if (!pointee || !pointee->size || value.kind == MVM_KIND_PACKET)
    return MVM_RESULT_RUNTIME_ERROR;
  uint32_t reference_type_id;
  mvm_result_t status =
      mvm_reference_type(mvm, pointee_type_id, true, &reference_type_id);
  if (status != MVM_RESULT_OK)
    return status;
  mvm_value_t reference;
  status = mvm_typed_construct(mvm, pointee_type_id, &reference);
  if (status != MVM_RESULT_OK)
    return status;
  status = mvm_full_store(mvm, pointee_type_id, reference.as.ptr, value);
  if (status != MVM_RESULT_OK)
    return status;
  reference.type_id = reference_type_id;
  reference.has_full_type = true;
  *result = reference;
  return MVM_RESULT_OK;
}

static bool mvm_layout_visit(const mvm_t *mvm, uint32_t id, uint8_t *states,
                              size_t depth) {
  if (id >= mvm->type_count || depth > 256 || states[id] == 1) return false;
  if (states[id] == 2) return true;
  states[id] = 1;
  const mvm_type_t *type = &mvm->types[id];
  if (type->tag == MVM_TYPE_AGGREGATE) {
    for (size_t i = 0; i < type->field_count; ++i)
      if (type->fields[i].has_type_id &&
          !mvm_layout_visit(mvm, type->fields[i].type_id, states, depth + 1))
        return false;
  } else if (type->tag == MVM_TYPE_UNION) {
    for (size_t i = 0; i < type->variant_count; ++i)
      if (!mvm_layout_visit(mvm, type->variant_type_ids[i], states, depth + 1)) return false;
  }
  states[id] = 2;
  return true;
}

mvm_result_t mvm_type_load(mvm_t *mvm, const mvm_type_desc_t *descs,
                           size_t count) {
  if (!mvm || mvm->type_count || (count && !descs) || count > UINT32_MAX)
    return MVM_RESULT_VM_ERROR;
  if (count > SIZE_MAX / sizeof(*mvm->types))
    return MVM_RESULT_VM_ERROR;
  mvm->types = mvm_memory_calloc(mvm->memory, count, sizeof(*mvm->types));
  if (count && !mvm->types)
    return MVM_RESULT_VM_ERROR;
  mvm->type_count = count;
  mvm->type_capacity = count;
  for (size_t i = 0; i < count; ++i) {
    const mvm_type_desc_t *desc = &descs[i];
    mvm_type_t *type = &mvm->types[i];
    type->size = desc->size;
    type->alignment = desc->alignment;
    type->field_count = desc->field_count;
    type->reference_count = desc->reference_count;
    type->tag = desc->tag;
    type->kind = desc->kind;
    type->inner_type_id = desc->inner_type_id;
    type->flags = desc->flags;
    type->argc = desc->argc;
    type->result_type_id = desc->result_type_id;
    type->variant_count = desc->variant_count;
    if (desc->variant_count) {
      if (desc->variant_count > 4096 || !desc->variant_type_ids)
        goto invalid;
      type->variant_type_ids =
          mvm_memory_alloc(mvm->memory, desc->variant_count * sizeof(uint32_t));
      if (!type->variant_type_ids)
        goto invalid;
      memcpy(type->variant_type_ids, desc->variant_type_ids,
             desc->variant_count * sizeof(uint32_t));
    }
    const mvm_kind_t intrinsic = mvm_type_intrinsic_reference(desc);
    if (intrinsic && !type->reference_count)
      type->reference_count = 1;
    if (desc->field_count) {
      if (desc->field_count > SIZE_MAX / sizeof(*type->fields) || !desc->fields)
        goto invalid;
      type->fields = mvm_memory_alloc(mvm->memory, desc->field_count *
                                                       sizeof(*type->fields));
      if (!type->fields)
        goto invalid;
      memcpy(type->fields, desc->fields,
             desc->field_count * sizeof(*type->fields));
    }
    if (type->reference_count) {
      if (type->reference_count > SIZE_MAX / sizeof(*type->references))
        goto invalid;
      type->references = mvm_memory_alloc(
          mvm->memory, type->reference_count * sizeof(*type->references));
      if (!type->references)
        goto invalid;
      if (desc->reference_count) {
        if (!desc->references)
          goto invalid;
        memcpy(type->references, desc->references,
               desc->reference_count * sizeof(*type->references));
      } else {
        type->references[0] =
            (mvm_type_reference_t){.offset = 0, .kind = intrinsic};
      }
    }
    if (desc->argc) {
      if (!desc->param_type_ids)
        goto invalid;
      type->param_type_ids = mvm_memory_alloc(
          mvm->memory, desc->argc * sizeof(*type->param_type_ids));
      if (!type->param_type_ids)
        goto invalid;
      memcpy(type->param_type_ids, desc->param_type_ids,
             desc->argc * sizeof(*type->param_type_ids));
    }
  }
  for (size_t i = 0; i < count; ++i) {
    const mvm_type_t *type = &mvm->types[i];
    const mvm_type_desc_t desc = {
        .size = type->size,
        .alignment = type->alignment,
        .field_count = type->field_count,
        .fields = type->fields,
        .reference_count = type->reference_count,
        .references = type->references,
        .tag = type->tag,
        .kind = type->kind,
        .inner_type_id = type->inner_type_id,
        .flags = type->flags,
        .argc = type->argc,
        .param_type_ids = type->param_type_ids,
        .result_type_id = type->result_type_id,
        .variant_count = type->variant_count,
        .variant_type_ids = type->variant_type_ids,
    };
    if (!mvm_type_valid(mvm, &desc))
      goto invalid;
  }
  uint8_t *states = count ? mvm_memory_calloc(mvm->memory, count, 1) : NULL;
  if (count && !states)
    goto invalid;
  bool valid_layout = true;
  for (size_t i = 0; i < count && valid_layout; ++i)
    valid_layout = mvm_layout_visit(mvm, (uint32_t)i, states, 0);
  mvm_memory_free(states);
  if (!valid_layout)
    goto invalid;
  return MVM_RESULT_OK;

invalid:
  for (size_t i = 0; i < count; ++i)
    mvm_type_clear(&mvm->types[i]);
  mvm_memory_free(mvm->types);
  mvm->types = NULL;
  mvm->type_count = 0;
  mvm->type_capacity = 0;
  return MVM_RESULT_VM_ERROR;
}

static bool mvm_defaultable_depth(const mvm_t *mvm, uint32_t id, size_t depth) {
  const mvm_type_t *type = mvm_type_by_id(mvm, id);
  if (!type || depth > 256 || type->tag == MVM_TYPE_UNION)
    return false;
  if (mvm_nullable_kind(type->kind) &&
      !(type->flags & MVM_TYPE_FLAG_NULLABLE))
    return false;
  if (type->tag == MVM_TYPE_AGGREGATE) {
    for (size_t i = 0; i < type->field_count; ++i) {
      const mvm_type_field_t *field = &type->fields[i];
      if (field->has_type_id &&
          !mvm_defaultable_depth(mvm, field->type_id, depth + 1))
        return false;
    }
  }
  return true;
}

bool mvm_type_defaultable(const mvm_t *mvm, uint32_t type_id) {
  return mvm && mvm_defaultable_depth(mvm, type_id, 0);
}

static bool mvm_union_contains(const mvm_type_t *type, uint32_t id) {
  if (!type || type->tag != MVM_TYPE_UNION)
    return false;
  for (size_t i = 0; i < type->variant_count; ++i)
    if (type->variant_type_ids[i] == id)
      return true;
  return false;
}

static const mvm_union_t *mvm_checked_union(const mvm_t *mvm, mvm_value_t value) {
  if (value.kind != MVM_KIND_UNION || !value.has_full_type)
    return NULL;
  const mvm_object_t *object = mvm_gc_find(mvm, value.as.union_value);
  if (!object || object->kind != MVM_OBJECT_UNION ||
      object->data != (const unsigned char *)value.as.union_value ||
      object->size != sizeof(mvm_union_t) || object->type_id != value.type_id)
    return NULL;
  const mvm_union_t *box = value.as.union_value;
  return mvm_union_contains(mvm_type_by_id(mvm, value.type_id), box->active_type_id)
             ? box : NULL;
}

static bool mvm_storage_path(const mvm_t *mvm, uint32_t owner_id, size_t offset,
                              uint32_t wanted_id, size_t depth) {
  const mvm_type_t *owner = mvm_type_by_id(mvm, owner_id);
  if (!owner || depth > 256) return false;
  if (!offset && owner_id == wanted_id) return true;
  if (owner->tag != MVM_TYPE_AGGREGATE) return false;
  for (size_t i = 0; i < owner->field_count; ++i) {
    const mvm_type_field_t *field = &owner->fields[i];
    if (field->has_type_id && offset >= field->offset && offset - field->offset < field->size &&
        mvm_storage_path(mvm, field->type_id, offset - field->offset, wanted_id, depth + 1))
      return true;
  }
  return false;
}

static bool mvm_matches_depth(const mvm_t *mvm, uint32_t id,
                              mvm_value_t value, size_t depth) {
  const mvm_type_t *type = mvm_type_by_id(mvm, id);
  if (!type || !type->size || depth > 256)
    return false;
  if (value.kind == MVM_KIND_NULL)
    return mvm_nullable_kind(type->kind) &&
           (type->flags & MVM_TYPE_FLAG_NULLABLE);
  if (type->tag == MVM_TYPE_UNION) {
    const mvm_union_t *box = mvm_checked_union(mvm, value);
    return value.type_id == id && box &&
           mvm_matches_depth(mvm, box->active_type_id, box->payload, depth + 1);
  }
  if (type->tag == MVM_TYPE_AGGREGATE) {
    if (value.kind != MVM_KIND_REFERENCE || !value.has_full_type)
      return false;
    unsigned char *data;
    if (value.type_id != id) {
      const mvm_type_t *actual = mvm_type_by_id(mvm, value.type_id);
      if (!actual || actual->tag != MVM_TYPE_REFERENCE || actual->inner_type_id != id ||
          mvm_reference_memory(mvm, actual, value, &data) != MVM_RESULT_OK)
        return false;
    } else if (mvm_typed_memory(mvm, id, value, &data) != MVM_RESULT_OK) {
      return false;
    }
    const mvm_object_t *owner = mvm_gc_find(mvm, value.as.ptr);
    if (!owner || !mvm_storage_path(mvm, owner->type_id,
            (uintptr_t)value.as.ptr - (uintptr_t)owner->data, id, 0))
      return false;
    for (size_t i = 0; i < type->field_count; ++i) {
      const mvm_type_field_t *field = &type->fields[i];
      if (!field->has_type_id)
        continue;
      const mvm_type_t *field_type = mvm_type_by_id(mvm, field->type_id);
      if (!field_type) return false;
      mvm_value_t part;
      if (field_type->tag == MVM_TYPE_AGGREGATE) {
        part = (mvm_value_t){.kind = MVM_KIND_REFERENCE,
                             .as.ptr = data + field->offset};
      } else if (mvm_typed_load(data + field->offset, field_type->kind, &part) != MVM_RESULT_OK) {
        return false;
      }
      part.type_id = field->type_id;
      part.has_full_type = true;
      // Validate the subtree once; full_load would recursively validate it again.
      if (!mvm_matches_depth(mvm, field->type_id, part, depth + 1)) return false;
    }
    return true;
  }
  if (value.kind != type->kind)
    return false;
  if (type->tag == MVM_TYPE_REFERENCE) {
    unsigned char *data;
    return mvm_reference_memory(mvm, type, value, &data) == MVM_RESULT_OK;
  }
  if (type->tag == MVM_TYPE_ARRAY) {
    const mvm_array_t *array = mvm_checked_array(mvm, value);
    return array && array->element_type_id == type->inner_type_id;
  }
  if (type->tag == MVM_TYPE_FUNCTION) {
    for (size_t i = 0; i < mvm->function_type_count; ++i)
      if (mvm->function_types[i].address == value.as.func)
        return mvm->function_types[i].type_id == id;
    return false;
  }
  if (type->kind == MVM_KIND_STRING) {
    size_t bytes, chars;
    return mvm_measure_string(mvm, value, &bytes, &chars) == MVM_RESULT_OK;
  }
  if (type->kind == MVM_KIND_THREAD)
    return mvm_checked_thread(mvm, value) != NULL;
  if (type->kind == MVM_KIND_MUTEX || type->kind == MVM_KIND_CONDITION ||
      type->kind == MVM_KIND_ATOMIC)
    return mvm_sync_value_valid(mvm, value);
  if (type->kind == MVM_KIND_TYPE)
    return value.has_full_type && mvm_type_by_id(mvm, value.as.type);
  return !value.has_full_type || value.type_id == id;
}

bool mvm_value_matches_type(const mvm_t *mvm, uint32_t id, mvm_value_t value) {
  return mvm && mvm_matches_depth(mvm, id, value, 0);
}

static mvm_result_t mvm_union_box(mvm_t *mvm, uint32_t id, uint32_t active,
                                  mvm_value_t payload, mvm_value_t *result) {
  mvm_gc_root_t root;
  if (!mvm_gc_root_register(mvm, &root, &payload, 1))
    return MVM_RESULT_VM_ERROR;
  mvm_object_t *object = mvm_gc_allocate(mvm, MVM_OBJECT_UNION, sizeof(mvm_union_t));
  if (object) {
    object->type_id = id;
    mvm_union_t *box = (mvm_union_t *)object->data;
    box->active_type_id = active;
    box->payload = payload;
    *result = (mvm_value_t){.kind = MVM_KIND_UNION, .type_id = id,
                            .has_full_type = true, .as.union_value = box};
  }
  mvm_gc_root_unregister(mvm, &root);
  return object ? MVM_RESULT_OK : MVM_RESULT_VM_ERROR;
}

mvm_result_t mvm_union_new(mvm_t *mvm, uint32_t id, uint32_t active,
                            mvm_value_t payload, mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  if (!mvm_union_contains(mvm_type_by_id(mvm, id), active) ||
      !mvm_value_matches_type(mvm, active, payload))
    return MVM_RESULT_RUNTIME_ERROR;
  const mvm_type_t *type = mvm_type_by_id(mvm, active);
  if (type->tag == MVM_TYPE_AGGREGATE) {
    mvm_gc_root_t root;
    if (!mvm_gc_root_register(mvm, &root, &payload, 1))
      return MVM_RESULT_VM_ERROR;
    mvm_value_t copy;
    mvm_result_t status = mvm_typed_construct(mvm, active, &copy);
    if (status == MVM_RESULT_OK)
      status = mvm_typed_copy(mvm, active, copy, payload);
    mvm_gc_root_unregister(mvm, &root);
    if (status != MVM_RESULT_OK)
      return status;
    payload = copy;
  }
  // A variant's declared type also records typed null and weakened references.
  payload.type_id = active;
  payload.has_full_type = true;
  return mvm_union_box(mvm, id, active, payload, result);
}

mvm_result_t mvm_union_variantof(const mvm_t *mvm, mvm_value_t value,
                                  mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  const mvm_union_t *box = mvm_checked_union(mvm, value);
  if (!box || !mvm_value_matches_type(mvm, value.type_id, value))
    return MVM_RESULT_RUNTIME_ERROR;
  *result = (mvm_value_t){.kind = MVM_KIND_TYPE, .as.type = box->active_type_id,
                          .type_id = box->active_type_id, .has_full_type = true};
  return MVM_RESULT_OK;
}

mvm_result_t mvm_union_get(mvm_t *mvm, mvm_value_t value, uint32_t id,
                            mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  const mvm_union_t *box = mvm_checked_union(mvm, value);
  if (!box || box->active_type_id != id ||
      !mvm_value_matches_type(mvm, value.type_id, value))
    return MVM_RESULT_RUNTIME_ERROR;
  if (mvm_type_by_id(mvm, id)->tag != MVM_TYPE_AGGREGATE) {
    *result = box->payload;
    return MVM_RESULT_OK;
  }
  mvm_gc_root_t root;
  if (!mvm_gc_root_register(mvm, &root, &value, 1))
    return MVM_RESULT_VM_ERROR;
  mvm_value_t copy;
  mvm_result_t status = mvm_typed_construct(mvm, id, &copy);
  if (status == MVM_RESULT_OK)
    status = mvm_typed_copy(mvm, id, copy, box->payload);
  mvm_gc_root_unregister(mvm, &root);
  if (status == MVM_RESULT_OK)
    *result = copy;
  return status;
}

mvm_result_t mvm_union_widen(mvm_t *mvm, mvm_value_t value, uint32_t id,
                              mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  const mvm_union_t *box = mvm_checked_union(mvm, value);
  const mvm_type_t *source = mvm_type_by_id(mvm, value.type_id);
  const mvm_type_t *target = mvm_type_by_id(mvm, id);
  if (!box || !target || !mvm_value_matches_type(mvm, value.type_id, value))
    return MVM_RESULT_RUNTIME_ERROR;
  for (size_t i = 0; i < source->variant_count; ++i)
    if (!mvm_union_contains(target, source->variant_type_ids[i]))
      return MVM_RESULT_RUNTIME_ERROR;
  if (id == value.type_id) {
    *result = value;
    return MVM_RESULT_OK;
  }
  // payload is rooted inside mvm_union_box before its first allocation.
  return mvm_union_box(mvm, id, box->active_type_id, box->payload, result);
}
