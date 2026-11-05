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

#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "file.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#include <unistd.h>
#endif

#define MVM_FILE_SCHEME "mvm://"
#define MVM_FILE_CHUNK 8192

static uint32_t *mvm_file_error_slot(mvm_t *mvm) {
  mvm_execution_t *execution = mvm_current_execution(mvm);
  return execution ? &execution->file_error : &mvm->file_error;
}

void mvm_file_clear_error(mvm_t *mvm) {
  if (mvm)
    *mvm_file_error_slot(mvm) = MVM_FILE_ERROR_NONE;
}

void mvm_file_set_error(mvm_t *mvm, const uint32_t error) {
  if (mvm)
    *mvm_file_error_slot(mvm) = error;
}

void mvm_file_set_errno(mvm_t *mvm, const int error) {
  uint32_t mapped = MVM_FILE_ERROR_IO;
  if (error == ENOENT || error == ENOTDIR)
    mapped = MVM_FILE_ERROR_NOT_FOUND;
  else if (error == EACCES || error == EPERM)
    mapped = MVM_FILE_ERROR_PERMISSION;
  else if (error == EEXIST)
    mapped = MVM_FILE_ERROR_ALREADY_EXISTS;
  else if (error == EBADF)
    mapped = MVM_FILE_ERROR_INVALID_HANDLE;
  else if (error == EINVAL)
    mapped = MVM_FILE_ERROR_INVALID_ARGUMENT;
  else if (error == EFBIG || error == ENOSPC || error == ENOMEM)
    mapped = MVM_FILE_ERROR_RESOURCE_LIMIT;
  mvm_file_set_error(mvm, mapped);
}

#if defined(_WIN32)
static void mvm_file_set_win32_error(mvm_t *mvm, const DWORD error) {
  uint32_t mapped = MVM_FILE_ERROR_IO;
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
    mapped = MVM_FILE_ERROR_NOT_FOUND;
  else if (error == ERROR_ACCESS_DENIED)
    mapped = MVM_FILE_ERROR_PERMISSION;
  else if (error == ERROR_ALREADY_EXISTS)
    mapped = MVM_FILE_ERROR_ALREADY_EXISTS;
  else if (error == ERROR_INVALID_NAME || error == ERROR_INVALID_PARAMETER)
    mapped = MVM_FILE_ERROR_INVALID_ARGUMENT;
  else if (error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY ||
           error == ERROR_DISK_FULL)
    mapped = MVM_FILE_ERROR_RESOURCE_LIMIT;
  mvm_file_set_error(mvm, mapped);
}
#endif

static bool mvm_file_is_resource(const char *path) {
  return path && strncmp(path, MVM_FILE_SCHEME, sizeof(MVM_FILE_SCHEME) - 1) == 0;
}

static char *mvm_file_image_path(mvm_t *mvm) {
  if (!mvm->binary_path)
    return NULL;
#if defined(_WIN32)
  char *resolved = _fullpath(NULL, mvm->binary_path, 0);
#else
  char *resolved = realpath(mvm->binary_path, NULL);
#endif
  if (!resolved)
    return NULL;
  const size_t length = strlen(resolved);
  char *copy = mvm_memory_alloc(mvm->memory, length + 1);
  if (copy)
    memcpy(copy, resolved, length + 1);
  free(resolved);
  return copy;
}

static uint64_t mvm_file_handle(size_t index, uint32_t generation) {
  return ((uint64_t)generation << 32) | (uint64_t)(index + 1);
}

static MvmFileSlot *mvm_file_lookup(mvm_t *mvm, uint64_t handle) {
  mvm_file_state_t *state = mvm->files;
  const size_t index = (size_t)(handle & UINT32_MAX);
  const uint32_t generation = (uint32_t)(handle >> 32);
  if (!state || !index || !generation || index > state->count)
    return NULL;
  MvmFileSlot *slot = state->slots[index - 1];
  return slot && slot->generation == generation ? slot : NULL;
}

static MvmFileSlot *mvm_file_acquire(mvm_t *mvm, uint64_t handle) {
  mvm_file_state_t *state = mvm->files;
  if (!state) {
    mvm_file_set_error(mvm, MVM_FILE_ERROR_INVALID_HANDLE);
    return NULL;
  }
  mvm_platform_lock(&state->mutex);
  MvmFileSlot *slot = mvm_file_lookup(mvm, handle);
  if (!slot || !slot->active || slot->closing) {
    mvm_platform_unlock(&state->mutex);
    mvm_file_set_error(mvm, MVM_FILE_ERROR_INVALID_HANDLE);
    return NULL;
  }
  ++slot->users;
  mvm_platform_unlock(&state->mutex);
  mvm_platform_lock(&slot->operation);
  return slot;
}

static void mvm_file_release(mvm_t *mvm, MvmFileSlot *slot) {
  mvm_platform_unlock(&slot->operation);
  mvm_file_state_t *state = mvm->files;
  mvm_platform_lock(&state->mutex);
  --slot->users;
  if (!slot->users)
    mvm_platform_broadcast(&state->cond);
  mvm_platform_unlock(&state->mutex);
}

static bool mvm_file_discard(MvmFileSlot *slot, int *error) {
  bool ok = true;
  if (slot->stream) {
    ok = fclose(slot->stream) == 0;
    if (!ok && error)
      *error = errno;
    slot->stream = NULL;
  }
  if (slot->data) {
    mvm_memory_free(slot->data);
    slot->data = NULL;
  }
  slot->size = slot->offset = 0;
  return ok;
}

static mvm_result_t mvm_file_open_impl(mvm_t *mvm, const char *path,
                                       uint32_t mode, uint64_t *result) {
  if (!mvm || !path || !result)
    return MVM_RESULT_VM_ERROR;
  if (!*path || mode > 4) {
    mvm_file_set_error(mvm, MVM_FILE_ERROR_INVALID_ARGUMENT);
    return MVM_RESULT_OK;
  }
  MvmFileSlot opened = {0};
  FILE *stream = NULL;
  const bool resource = mvm_file_is_resource(path);
  if (resource) {
    if (mode != 0) {
      mvm_file_set_error(mvm, MVM_FILE_ERROR_READ_ONLY);
      return MVM_RESULT_OK;
    }
    const mvm_result_t status =
        mvm_resource_open(mvm, path + sizeof(MVM_FILE_SCHEME) - 1, &opened);
    if (status != MVM_RESULT_OK || !opened.data)
      return status;
    opened.backend = MVM_FILE_BACKEND_RESOURCE;
  } else {
    static const char *const modes[] = {"rb", "wb", "ab", "r+b", "w+b"};
    stream = fopen(path, modes[mode]);
    if (!stream) {
      mvm_file_set_errno(mvm, errno);
      return MVM_RESULT_OK;
    }
    opened.stream = stream;
    opened.backend = MVM_FILE_BACKEND_OS;
  }

  mvm_file_state_t *state = mvm->files;
  mvm_platform_lock(&state->mutex);
  size_t index = 0;
  while (index < state->count && state->slots[index]->active)
    ++index;
  if (index == state->count) {
    if (state->count == UINT32_MAX) {
      mvm_platform_unlock(&state->mutex);
      mvm_file_discard(&opened, NULL);
      return MVM_RESULT_VM_ERROR;
    }
    if (state->count == state->capacity) {
      const size_t capacity = state->capacity ? state->capacity * 2 : 16;
      if (capacity < state->capacity ||
          capacity > SIZE_MAX / sizeof(*state->slots)) {
        mvm_platform_unlock(&state->mutex);
        mvm_file_discard(&opened, NULL);
        return MVM_RESULT_VM_ERROR;
      }
      MvmFileSlot **slots = mvm_memory_realloc(
          mvm->memory, state->slots, capacity * sizeof(*state->slots));
      if (!slots) {
        mvm_platform_unlock(&state->mutex);
        mvm_file_discard(&opened, NULL);
        return MVM_RESULT_VM_ERROR;
      }
      state->slots = slots;
      state->capacity = capacity;
    }
    MvmFileSlot *slot = mvm_memory_calloc(mvm->memory, 1, sizeof(*slot));
    if (!slot) {
      mvm_platform_unlock(&state->mutex);
      mvm_file_discard(&opened, NULL);
      return MVM_RESULT_VM_ERROR;
    }
    if (!mvm_platform_mutex_init(&slot->operation)) {
      mvm_memory_free(slot);
      mvm_platform_unlock(&state->mutex);
      mvm_file_discard(&opened, NULL);
      return MVM_RESULT_VM_ERROR;
    }
    slot->generation = 1;
    state->slots[state->count++] = slot;
  }
  MvmFileSlot *slot = state->slots[index];
  slot->backend = opened.backend;
  slot->stream = opened.stream;
  slot->data = opened.data;
  slot->size = opened.size;
  slot->offset = 0;
  slot->users = 0;
  slot->closing = false;
  slot->active = true;
  *result = mvm_file_handle(index, slot->generation);
  mvm_platform_unlock(&state->mutex);
  return MVM_RESULT_OK;
}

static bool mvm_file_close_impl(mvm_t *mvm, uint64_t handle) {
  mvm_file_state_t *state = mvm->files;
  if (!state) {
    mvm_file_set_error(mvm, MVM_FILE_ERROR_INVALID_HANDLE);
    return false;
  }
  mvm_platform_lock(&state->mutex);
  MvmFileSlot *slot = mvm_file_lookup(mvm, handle);
  if (!slot || !slot->active || slot->closing) {
    mvm_platform_unlock(&state->mutex);
    mvm_file_set_error(mvm, MVM_FILE_ERROR_INVALID_HANDLE);
    return false;
  }
  slot->closing = true;
  while (slot->users) {
    if (!mvm_platform_wait(&state->cond, &state->mutex)) {
      slot->closing = false;
      mvm_platform_unlock(&state->mutex);
      mvm_file_set_error(mvm, MVM_FILE_ERROR_IO);
      return false;
    }
  }
  int error = 0;
  const bool ok = mvm_file_discard(slot, &error);
  slot->active = false;
  slot->closing = false;
  ++slot->generation;
  if (!slot->generation)
    slot->generation = 1;
  mvm_platform_unlock(&state->mutex);
  if (!ok) {
    mvm_file_set_errno(mvm, error);
    return false;
  }
  return true;
}

static mvm_result_t mvm_file_read_bytes(mvm_t *mvm, uint64_t handle,
                                        unsigned char *data, size_t capacity,
                                        size_t *read) {
  MvmFileSlot *slot = mvm_file_acquire(mvm, handle);
  if (!slot)
    return MVM_RESULT_OK;
  size_t count = 0;
  if (slot->backend == MVM_FILE_BACKEND_RESOURCE) {
    count = slot->size - slot->offset;
    if (count > capacity)
      count = capacity;
    if (count)
      memcpy(data, slot->data + slot->offset, count);
    slot->offset += count;
  } else {
    count = fread(data, 1, capacity, slot->stream);
    if (ferror(slot->stream)) {
      mvm_file_set_errno(mvm, errno);
      count = 0;
    }
  }
  mvm_file_release(mvm, slot);
  *read = count;
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_file_read_impl(mvm_t *mvm, uint64_t handle,
                                       mvm_value_t buffer, int64_t *result) {
  mvm_array_t *array = mvm_checked_array(mvm, buffer);
  if (!array || array->kind != MVM_KIND_UBYTE)
    return MVM_RESULT_RUNTIME_ERROR;
  if (array->length > INT64_MAX) {
    mvm_file_set_error(mvm, MVM_FILE_ERROR_INVALID_ARGUMENT);
    return MVM_RESULT_OK;
  }
  size_t total = 0;
  while (total < array->length) {
    unsigned char data[MVM_FILE_CHUNK];
    const size_t remaining = array->length - total;
    size_t count = 0;
    const mvm_result_t status = mvm_file_read_bytes(
        mvm, handle, data, remaining < sizeof(data) ? remaining : sizeof(data),
        &count);
    if (status != MVM_RESULT_OK)
      return status;
    if (*mvm_file_error_slot(mvm)) {
      *result = -1;
      return MVM_RESULT_OK;
    }
    for (size_t i = 0; i < count; ++i)
      if (mvm_array_set(mvm, buffer, total + i,
                        (mvm_value_t){.kind = MVM_KIND_UBYTE,
                                      .as.ub = data[i]}) != MVM_RESULT_OK)
        return MVM_RESULT_RUNTIME_ERROR;
    total += count;
    if (count < sizeof(data) && (count == 0 || count == remaining))
      break;
    if (count == 0)
      break;
  }
  *result = (int64_t)total;
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_file_write_impl(mvm_t *mvm, uint64_t handle,
                                        mvm_value_t data, int64_t *result) {
  mvm_array_t *array = mvm_checked_array(mvm, data);
  if (!array || array->kind != MVM_KIND_UBYTE)
    return MVM_RESULT_RUNTIME_ERROR;
  if (array->length > INT64_MAX) {
    mvm_file_set_error(mvm, MVM_FILE_ERROR_INVALID_ARGUMENT);
    return MVM_RESULT_OK;
  }
  MvmFileSlot *slot = mvm_file_acquire(mvm, handle);
  if (!slot)
    return MVM_RESULT_OK;
  if (slot->backend != MVM_FILE_BACKEND_OS) {
    mvm_file_release(mvm, slot);
    mvm_file_set_error(mvm, MVM_FILE_ERROR_READ_ONLY);
    return MVM_RESULT_OK;
  }
  size_t total = 0;
  bool failed = false;
  while (total < array->length) {
    unsigned char buffer[MVM_FILE_CHUNK];
    size_t count = array->length - total;
    if (count > sizeof(buffer))
      count = sizeof(buffer);
    for (size_t i = 0; i < count; ++i) {
      if (array->elements[total + i].kind != MVM_KIND_UBYTE) {
        mvm_file_release(mvm, slot);
        return MVM_RESULT_RUNTIME_ERROR;
      }
      buffer[i] = array->elements[total + i].as.ub;
    }
    const size_t written = fwrite(buffer, 1, count, slot->stream);
    total += written;
    if (written != count) {
      if (ferror(slot->stream)) {
        mvm_file_set_errno(mvm, errno);
        failed = true;
      }
      break;
    }
  }
  mvm_file_release(mvm, slot);
  *result = failed ? -1 : (int64_t)total;
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_file_read_line_impl(mvm_t *mvm, uint64_t handle,
                                            mvm_value_t *result) {
  MvmFileSlot *slot = mvm_file_acquire(mvm, handle);
  if (!slot)
    return MVM_RESULT_OK;
  unsigned char *buffer = NULL;
  size_t length = 0;
  size_t capacity = 0;
  bool eof = false;
  if (slot->backend == MVM_FILE_BACKEND_RESOURCE) {
    const size_t start = slot->offset;
    while (slot->offset < slot->size && slot->data[slot->offset] != '\n')
      ++slot->offset;
    length = slot->offset - start;
    if (slot->offset < slot->size)
      ++slot->offset;
    else
      eof = true;
    if (length) {
      buffer = mvm_memory_alloc(mvm->memory, length);
      if (!buffer) {
        mvm_file_release(mvm, slot);
        return MVM_RESULT_VM_ERROR;
      }
      memcpy(buffer, slot->data + start, length);
    }
  } else {
    int c;
    while ((c = fgetc(slot->stream)) != EOF) {
      if (c == '\n')
        break;
      if (length == capacity) {
        const size_t grown = capacity ? capacity * 2 : 128;
        if (grown < capacity) {
          mvm_memory_free(buffer);
          mvm_file_release(mvm, slot);
          return MVM_RESULT_VM_ERROR;
        }
        unsigned char *next =
            mvm_memory_realloc(mvm->memory, buffer, grown);
        if (!next) {
          mvm_memory_free(buffer);
          mvm_file_release(mvm, slot);
          return MVM_RESULT_VM_ERROR;
        }
        buffer = next;
        capacity = grown;
      }
      buffer[length++] = (unsigned char)c;
    }
    eof = c == EOF;
    if (ferror(slot->stream))
      mvm_file_set_errno(mvm, errno);
  }
  mvm_file_release(mvm, slot);
  if (mvm->file_error || (mvm_current_execution(mvm) &&
                         mvm_current_execution(mvm)->file_error)) {
    mvm_memory_free(buffer);
    return MVM_RESULT_OK;
  }
  if (eof && !length) {
    mvm_memory_free(buffer);
    *result = (mvm_value_t){.kind = MVM_KIND_NULL};
    return MVM_RESULT_OK;
  }
  if (length && buffer[length - 1] == '\r')
    --length;
  const mvm_result_t status =
      mvm_string_from_bytes(mvm, buffer, length, result);
  mvm_memory_free(buffer);
  if (status == MVM_RESULT_RUNTIME_ERROR) {
    mvm_file_set_error(mvm, MVM_FILE_ERROR_INVALID_UTF8);
    *result = (mvm_value_t){.kind = MVM_KIND_NULL};
    return MVM_RESULT_OK;
  }
  return status;
}

static mvm_result_t mvm_file_seek_impl(mvm_t *mvm, uint64_t handle,
                                       int64_t offset, uint32_t origin,
                                       bool *result) {
  if (origin > 2) {
    mvm_file_set_error(mvm, MVM_FILE_ERROR_INVALID_ARGUMENT);
    return MVM_RESULT_OK;
  }
  MvmFileSlot *slot = mvm_file_acquire(mvm, handle);
  if (!slot)
    return MVM_RESULT_OK;
  bool ok = true;
  if (slot->backend == MVM_FILE_BACKEND_RESOURCE) {
    int64_t base = origin == 0 ? 0 : origin == 1 ? (int64_t)slot->offset
                                                 : (int64_t)slot->size;
    if ((offset > 0 && base > INT64_MAX - offset) ||
        (offset < 0 && base < INT64_MIN - offset)) {
      ok = false;
    } else {
      const int64_t position = base + offset;
      if (position < 0 || (uint64_t)position > slot->size)
        ok = false;
      else
        slot->offset = (size_t)position;
    }
  } else {
    const int whence = origin == 0 ? SEEK_SET : origin == 1 ? SEEK_CUR : SEEK_END;
#if defined(_WIN32)
    ok = _fseeki64(slot->stream, offset, whence) == 0;
#else
    const off_t converted = (off_t)offset;
    ok = (int64_t)converted == offset &&
         fseeko(slot->stream, converted, whence) == 0;
#endif
  }
  if (!ok)
    mvm_file_set_error(mvm, MVM_FILE_ERROR_INVALID_ARGUMENT);
  mvm_file_release(mvm, slot);
  *result = ok;
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_file_tell_impl(mvm_t *mvm, uint64_t handle,
                                       int64_t *result) {
  MvmFileSlot *slot = mvm_file_acquire(mvm, handle);
  if (!slot)
    return MVM_RESULT_OK;
  int64_t position = -1;
  if (slot->backend == MVM_FILE_BACKEND_RESOURCE) {
    if (slot->offset <= INT64_MAX)
      position = (int64_t)slot->offset;
  } else {
#if defined(_WIN32)
    position = _ftelli64(slot->stream);
#else
    const off_t value = ftello(slot->stream);
    if (value >= 0 && (uint64_t)value <= INT64_MAX)
      position = (int64_t)value;
#endif
  }
  if (position < 0)
    mvm_file_set_errno(mvm, errno);
  mvm_file_release(mvm, slot);
  *result = position;
  return MVM_RESULT_OK;
}

static bool mvm_file_flush_impl(mvm_t *mvm, uint64_t handle) {
  MvmFileSlot *slot = mvm_file_acquire(mvm, handle);
  if (!slot)
    return false;
  bool ok = slot->backend == MVM_FILE_BACKEND_OS && fflush(slot->stream) == 0;
  if (slot->backend != MVM_FILE_BACKEND_OS)
    mvm_file_set_error(mvm, MVM_FILE_ERROR_READ_ONLY);
  else if (!ok)
    mvm_file_set_errno(mvm, errno);
  mvm_file_release(mvm, slot);
  return ok;
}

static mvm_result_t mvm_file_read_all_impl(mvm_t *mvm, const char *path,
                                           mvm_value_t *result) {
  uint64_t handle = 0;
  mvm_result_t status = mvm_file_open_impl(mvm, path, 0, &handle);
  if (status != MVM_RESULT_OK || !handle)
    return status;
  mvm_value_t array;
  status = mvm_array_new_primitive(mvm, MVM_KIND_UBYTE, 0, &array);
  if (status != MVM_RESULT_OK) {
    mvm_file_close_impl(mvm, handle);
    return status;
  }
  for (;;) {
    unsigned char data[MVM_FILE_CHUNK];
    size_t count = 0;
    status = mvm_file_read_bytes(mvm, handle, data, sizeof(data), &count);
    if (status != MVM_RESULT_OK || *mvm_file_error_slot(mvm))
      break;
    if (!count)
      break;
    const size_t old_length = array.as.array->length;
    if (count > UINT32_MAX - old_length) {
      mvm_file_set_error(mvm, MVM_FILE_ERROR_RESOURCE_LIMIT);
      break;
    }
    status = mvm_array_resize(mvm, array, old_length + count);
    if (status != MVM_RESULT_OK)
      break;
    for (size_t i = 0; i < count; ++i)
      if (mvm_array_set(mvm, array, old_length + i,
                        (mvm_value_t){.kind = MVM_KIND_UBYTE,
                                      .as.ub = data[i]}) != MVM_RESULT_OK) {
        status = MVM_RESULT_RUNTIME_ERROR;
        break;
      }
    if (status != MVM_RESULT_OK)
      break;
  }
  const bool closed = mvm_file_close_impl(mvm, handle);
  if (status != MVM_RESULT_OK)
    return status;
  if (!closed || *mvm_file_error_slot(mvm)) {
    *result = (mvm_value_t){.kind = MVM_KIND_NULL};
    return MVM_RESULT_OK;
  }
  *result = array;
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_file_write_all_impl(mvm_t *mvm, const char *path,
                                            mvm_value_t data, bool *result) {
  uint64_t handle = 0;
  mvm_result_t status = mvm_file_open_impl(mvm, path, 1, &handle);
  if (status != MVM_RESULT_OK || !handle)
    return status;
  int64_t written = -1;
  status = mvm_file_write_impl(mvm, handle, data, &written);
  const bool closed = mvm_file_close_impl(mvm, handle);
  *result = status == MVM_RESULT_OK && written >= 0 &&
            (uint64_t)written == data.as.array->length && closed;
  return status;
}

static mvm_result_t mvm_file_string_from_array(mvm_t *mvm, mvm_value_t array,
                                               mvm_value_t *result) {
  if (!array.as.array || array.kind != MVM_KIND_ARRAY)
    return MVM_RESULT_RUNTIME_ERROR;
  const size_t length = array.as.array->length;
  unsigned char *data = mvm_memory_alloc(mvm->memory, length);
  if (length && !data)
    return MVM_RESULT_VM_ERROR;
  for (size_t i = 0; i < length; ++i) {
    if (array.as.array->elements[i].kind != MVM_KIND_UBYTE) {
      mvm_memory_free(data);
      return MVM_RESULT_RUNTIME_ERROR;
    }
    data[i] = array.as.array->elements[i].as.ub;
  }
  const mvm_result_t status = mvm_string_from_bytes(mvm, data, length, result);
  mvm_memory_free(data);
  if (status == MVM_RESULT_RUNTIME_ERROR) {
    mvm_file_set_error(mvm, MVM_FILE_ERROR_INVALID_UTF8);
    *result = (mvm_value_t){.kind = MVM_KIND_NULL};
    return MVM_RESULT_OK;
  }
  return status;
}

static mvm_result_t mvm_file_read_text_impl(mvm_t *mvm, const char *path,
                                            mvm_value_t *result) {
  mvm_value_t bytes;
  const mvm_result_t status = mvm_file_read_all_impl(mvm, path, &bytes);
  if (status != MVM_RESULT_OK || bytes.kind == MVM_KIND_NULL)
    return status;
  return mvm_file_string_from_array(mvm, bytes, result);
}

static mvm_result_t mvm_file_write_text_impl(mvm_t *mvm, const char *path,
                                             const char *text, bool *result) {
  size_t length;
  size_t characters;
  mvm_result_t status = mvm_measure_string(
      mvm, (mvm_value_t){.kind = MVM_KIND_STRING, .as.s = (char *)text},
      &length, &characters);
  if (status != MVM_RESULT_OK)
    return status;
  mvm_value_t bytes;
  status = mvm_array_new_primitive(mvm, MVM_KIND_UBYTE, length, &bytes);
  if (status != MVM_RESULT_OK)
    return status;
  for (size_t i = 0; i < length; ++i)
    if (mvm_array_set(mvm, bytes, i,
                      (mvm_value_t){.kind = MVM_KIND_UBYTE,
                                    .as.ub = (uint8_t)text[i]}) != MVM_RESULT_OK)
      return MVM_RESULT_RUNTIME_ERROR;
  return mvm_file_write_all_impl(mvm, path, bytes, result);
}

static bool mvm_file_remove_dir_impl(mvm_t *mvm, const char *path) {
#if defined(_WIN32)
  const bool ok = RemoveDirectoryA(path) != 0;
  if (!ok)
    mvm_file_set_win32_error(mvm, GetLastError());
  return ok;
#else
  const bool ok = rmdir(path) == 0;
  if (!ok)
    mvm_file_set_errno(mvm, errno);
  return ok;
#endif
}

static mvm_result_t mvm_file_temp_dir_result(mvm_t *mvm, const char *path,
                                             mvm_value_t *result) {
  const mvm_result_t status = mvm_string_from_bytes(
      mvm, (const unsigned char *)path, strlen(path), result);
  if (status != MVM_RESULT_RUNTIME_ERROR)
    return status;
  mvm_file_set_error(mvm, MVM_FILE_ERROR_INVALID_UTF8);
  result->kind = MVM_KIND_NULL;
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_file_make_temp_dir_impl(mvm_t *mvm,
                                                mvm_value_t *result) {
  result->kind = MVM_KIND_NULL;
#if defined(_WIN32)
  const DWORD required = GetTempPathA(0, NULL);
  if (!required) {
    mvm_file_set_win32_error(mvm, GetLastError());
    return MVM_RESULT_OK;
  }
  char *root = mvm_memory_alloc(mvm->memory, required);
  if (!root)
    return MVM_RESULT_VM_ERROR;
  const DWORD root_length = GetTempPathA(required, root);
  if (!root_length || root_length >= required) {
    mvm_file_set_win32_error(mvm, root_length ? ERROR_BUFFER_OVERFLOW
                                              : GetLastError());
    mvm_memory_free(root);
    return MVM_RESULT_OK;
  }
  const bool separator = root[root_length - 1] != '\\' &&
                         root[root_length - 1] != '/';
  if ((size_t)root_length > SIZE_MAX - 38) {
    mvm_memory_free(root);
    mvm_file_set_error(mvm, MVM_FILE_ERROR_RESOURCE_LIMIT);
    return MVM_RESULT_OK;
  }
  const size_t path_size = root_length + (separator ? 1 : 0) + 37;
  char *path = mvm_memory_alloc(mvm->memory, path_size);
  if (!path) {
    mvm_memory_free(root);
    return MVM_RESULT_VM_ERROR;
  }
  static const char digits[] = "0123456789abcdef";
  bool created = false;
  for (size_t attempt = 0; attempt < 128 && !created; ++attempt) {
    unsigned char random[16];
    if (BCryptGenRandom(NULL, random, sizeof(random),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
      mvm_file_set_error(mvm, MVM_FILE_ERROR_IO);
      break;
    }
    char *cursor = path;
    memcpy(cursor, root, root_length);
    cursor += root_length;
    if (separator)
      *cursor++ = '\\';
    memcpy(cursor, "mvm-", 4);
    cursor += 4;
    for (size_t i = 0; i < sizeof(random); ++i) {
      *cursor++ = digits[random[i] >> 4];
      *cursor++ = digits[random[i] & 15];
    }
    *cursor = '\0';
    created = CreateDirectoryA(path, NULL) != 0;
    if (!created) {
      const DWORD error = GetLastError();
      if (error != ERROR_ALREADY_EXISTS) {
        mvm_file_set_win32_error(mvm, error);
        break;
      }
    }
  }
  mvm_memory_free(root);
  if (!created) {
    if (*mvm_file_error_slot(mvm) == MVM_FILE_ERROR_NONE)
      mvm_file_set_error(mvm, MVM_FILE_ERROR_ALREADY_EXISTS);
    mvm_memory_free(path);
    return MVM_RESULT_OK;
  }
  const mvm_result_t status = mvm_file_temp_dir_result(mvm, path, result);
  if (status != MVM_RESULT_OK || result->kind == MVM_KIND_NULL)
    RemoveDirectoryA(path);
  mvm_memory_free(path);
  return status;
#else
  const char *root = getenv("TMPDIR");
  if (!root || !*root)
    root = "/tmp";
  const size_t root_length = strlen(root);
  const bool separator = root[root_length - 1] != '/';
  if (root_length > SIZE_MAX - sizeof("mvm-XXXXXX") - 1) {
    mvm_file_set_error(mvm, MVM_FILE_ERROR_RESOURCE_LIMIT);
    return MVM_RESULT_OK;
  }
  const size_t path_size =
      root_length + (separator ? 1 : 0) + sizeof("mvm-XXXXXX");
  char *path = mvm_memory_alloc(mvm->memory, path_size);
  if (!path)
    return MVM_RESULT_VM_ERROR;
  snprintf(path, path_size, "%s%smvm-XXXXXX", root, separator ? "/" : "");
  if (!mkdtemp(path)) {
    mvm_file_set_errno(mvm, errno);
    mvm_memory_free(path);
    return MVM_RESULT_OK;
  }
  char *resolved = realpath(path, NULL);
  if (!resolved) {
    const int error = errno;
    rmdir(path);
    mvm_memory_free(path);
    mvm_file_set_errno(mvm, error);
    return MVM_RESULT_OK;
  }
  mvm_memory_free(path);
  const mvm_result_t status = mvm_file_temp_dir_result(mvm, resolved, result);
  if (status != MVM_RESULT_OK || result->kind == MVM_KIND_NULL)
    rmdir(resolved);
  free(resolved);
  return status;
#endif
}

static mvm_result_t mvm_builtin_file_open(mvm_t *mvm, const mvm_value_t *args,
                                          mvm_value_t *result) {
  mvm_file_clear_error(mvm);
  uint64_t handle = 0;
  const mvm_result_t status =
      mvm_file_open_impl(mvm, args[0].as.s, args[1].as.uw, &handle);
  *result = (mvm_value_t){.kind = MVM_KIND_ULONG, .as.ul = handle};
  return status;
}

static mvm_result_t mvm_builtin_file_close(mvm_t *mvm, const mvm_value_t *args,
                                           mvm_value_t *result) {
  mvm_file_clear_error(mvm);
  *result = (mvm_value_t){.kind = MVM_KIND_BOOL,
                          .as.b = mvm_file_close_impl(mvm, args[0].as.ul)};
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_builtin_file_read(mvm_t *mvm, const mvm_value_t *args,
                                          mvm_value_t *result) {
  mvm_file_clear_error(mvm);
  int64_t count = -1;
  const mvm_result_t status =
      mvm_file_read_impl(mvm, args[0].as.ul, args[1], &count);
  *result = (mvm_value_t){.kind = MVM_KIND_SLONG, .as.sl = count};
  return status;
}

static mvm_result_t mvm_builtin_file_read_line(mvm_t *mvm,
                                               const mvm_value_t *args,
                                               mvm_value_t *result) {
  mvm_file_clear_error(mvm);
  result->kind = MVM_KIND_NULL;
  return mvm_file_read_line_impl(mvm, args[0].as.ul, result);
}

static mvm_result_t mvm_builtin_file_write(mvm_t *mvm, const mvm_value_t *args,
                                           mvm_value_t *result) {
  mvm_file_clear_error(mvm);
  int64_t count = -1;
  const mvm_result_t status =
      mvm_file_write_impl(mvm, args[0].as.ul, args[1], &count);
  *result = (mvm_value_t){.kind = MVM_KIND_SLONG, .as.sl = count};
  return status;
}

static mvm_result_t mvm_builtin_file_seek(mvm_t *mvm, const mvm_value_t *args,
                                          mvm_value_t *result) {
  mvm_file_clear_error(mvm);
  bool ok = false;
  const mvm_result_t status = mvm_file_seek_impl(
      mvm, args[0].as.ul, args[1].as.sl, args[2].as.uw, &ok);
  *result = (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = ok};
  return status;
}

static mvm_result_t mvm_builtin_file_tell(mvm_t *mvm, const mvm_value_t *args,
                                          mvm_value_t *result) {
  mvm_file_clear_error(mvm);
  int64_t position = -1;
  const mvm_result_t status =
      mvm_file_tell_impl(mvm, args[0].as.ul, &position);
  *result = (mvm_value_t){.kind = MVM_KIND_SLONG, .as.sl = position};
  return status;
}

static mvm_result_t mvm_builtin_file_flush(mvm_t *mvm, const mvm_value_t *args,
                                           mvm_value_t *result) {
  mvm_file_clear_error(mvm);
  *result = (mvm_value_t){.kind = MVM_KIND_BOOL,
                          .as.b = mvm_file_flush_impl(mvm, args[0].as.ul)};
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_builtin_file_read_all(mvm_t *mvm,
                                              const mvm_value_t *args,
                                              mvm_value_t *result) {
  mvm_file_clear_error(mvm);
  result->kind = MVM_KIND_NULL;
  return mvm_file_read_all_impl(mvm, args[0].as.s, result);
}

static mvm_result_t mvm_builtin_file_write_all(mvm_t *mvm,
                                               const mvm_value_t *args,
                                               mvm_value_t *result) {
  mvm_file_clear_error(mvm);
  bool ok = false;
  const mvm_result_t status =
      mvm_file_write_all_impl(mvm, args[0].as.s, args[1], &ok);
  *result = (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = ok};
  return status;
}

static mvm_result_t mvm_builtin_file_read_text(mvm_t *mvm,
                                               const mvm_value_t *args,
                                               mvm_value_t *result) {
  mvm_file_clear_error(mvm);
  result->kind = MVM_KIND_NULL;
  return mvm_file_read_text_impl(mvm, args[0].as.s, result);
}

static mvm_result_t mvm_builtin_file_write_text(mvm_t *mvm,
                                                const mvm_value_t *args,
                                                mvm_value_t *result) {
  mvm_file_clear_error(mvm);
  bool ok = false;
  const mvm_result_t status = mvm_file_write_text_impl(
      mvm, args[0].as.s, args[1].as.s, &ok);
  *result = (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = ok};
  return status;
}

static mvm_result_t mvm_builtin_file_make_temp_dir(mvm_t *mvm,
                                                   const mvm_value_t *args,
                                                   mvm_value_t *result) {
  (void)args;
  mvm_file_clear_error(mvm);
  result->kind = MVM_KIND_NULL;
  return mvm_file_make_temp_dir_impl(mvm, result);
}

static mvm_result_t mvm_builtin_file_remove_dir(mvm_t *mvm,
                                                const mvm_value_t *args,
                                                mvm_value_t *result) {
  mvm_file_clear_error(mvm);
  const char *path = args[0].as.s;
  bool ok = false;
  if (mvm_file_is_resource(path))
    mvm_file_set_error(mvm, MVM_FILE_ERROR_READ_ONLY);
  else
    ok = mvm_file_remove_dir_impl(mvm, path);
  *result = (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = ok};
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_builtin_file_exists(mvm_t *mvm,
                                            const mvm_value_t *args,
                                            mvm_value_t *result) {
  mvm_file_clear_error(mvm);
  const char *path = args[0].as.s;
  bool exists = false;
  if (mvm_file_is_resource(path)) {
    exists = mvm_resource_exists(mvm, path + sizeof(MVM_FILE_SCHEME) - 1);
  } else {
    struct stat info;
    exists = stat(path, &info) == 0;
  }
  *result = (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = exists};
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_builtin_file_remove(mvm_t *mvm,
                                            const mvm_value_t *args,
                                            mvm_value_t *result) {
  mvm_file_clear_error(mvm);
  const char *path = args[0].as.s;
  bool ok = false;
  if (mvm_file_is_resource(path)) {
    mvm_file_set_error(mvm, MVM_FILE_ERROR_READ_ONLY);
  } else {
    ok = remove(path) == 0;
    if (!ok)
      mvm_file_set_errno(mvm, errno);
  }
  *result = (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = ok};
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_builtin_file_rename(mvm_t *mvm,
                                            const mvm_value_t *args,
                                            mvm_value_t *result) {
  mvm_file_clear_error(mvm);
  const char *old_path = args[0].as.s;
  const char *new_path = args[1].as.s;
  bool ok = false;
  if (mvm_file_is_resource(old_path) || mvm_file_is_resource(new_path)) {
    mvm_file_set_error(mvm, MVM_FILE_ERROR_READ_ONLY);
  } else {
    ok = rename(old_path, new_path) == 0;
    if (!ok)
      mvm_file_set_errno(mvm, errno);
  }
  *result = (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = ok};
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_builtin_file_last_error(mvm_t *mvm,
                                                const mvm_value_t *args,
                                                mvm_value_t *result) {
  (void)args;
  *result = (mvm_value_t){.kind = MVM_KIND_UWORD,
                          .as.uw = *mvm_file_error_slot(mvm)};
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_builtin_file_error_message(mvm_t *mvm,
                                                   const mvm_value_t *args,
                                                   mvm_value_t *result) {
  static const char *const messages[] = {
      "no error", "invalid argument", "not found", "permission denied",
      "already exists", "I/O error", "invalid file handle", "read-only resource",
      "unsupported operation", "invalid UTF-8", "no current MVM image",
      "resource limit exceeded",
  };
  const uint32_t error = args[0].as.uw;
  const char *message = error < sizeof(messages) / sizeof(messages[0])
                            ? messages[error]
                            : "unknown file error";
  return mvm_string_from_bytes(mvm, (const unsigned char *)message,
                               strlen(message), result);
}

mvm_result_t mvm_file_register(mvm_t *mvm) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  if (mvm->files)
    return MVM_RESULT_VM_ERROR;
  mvm_file_state_t *state =
      mvm_memory_calloc(mvm->memory, 1, sizeof(*state));
  if (!state)
    return MVM_RESULT_VM_ERROR;
  if (!mvm_platform_mutex_init(&state->mutex)) {
    mvm_memory_free(state);
    return MVM_RESULT_VM_ERROR;
  }
  if (!mvm_platform_cond_init(&state->cond)) {
    mvm_platform_mutex_destroy(&state->mutex);
    mvm_memory_free(state);
    return MVM_RESULT_VM_ERROR;
  }
  state->image_path = mvm_file_image_path(mvm);
  if (!state->image_path) {
    mvm_platform_cond_destroy(&state->cond);
    mvm_platform_mutex_destroy(&state->mutex);
    mvm_memory_free(state);
    return MVM_RESULT_VM_ERROR;
  }
  mvm->files = state;

  static const mvm_kind_t open_args[] = {MVM_KIND_STRING, MVM_KIND_UWORD};
  static const mvm_kind_t file_arg[] = {MVM_KIND_ULONG};
  static const mvm_kind_t read_args[] = {MVM_KIND_ULONG, MVM_KIND_ARRAY};
  static const mvm_kind_t seek_args[] = {MVM_KIND_ULONG, MVM_KIND_SLONG,
                                         MVM_KIND_UWORD};
  static const mvm_kind_t write_all_args[] = {MVM_KIND_STRING, MVM_KIND_ARRAY};
  static const mvm_kind_t write_text_args[] = {MVM_KIND_STRING, MVM_KIND_STRING};
  static const mvm_kind_t path_args[] = {MVM_KIND_STRING};
  static const mvm_kind_t rename_args[] = {MVM_KIND_STRING, MVM_KIND_STRING};
  static const mvm_kind_t error_args[] = {MVM_KIND_UWORD};
  struct {
    const char *name;
    mvm_builtin_fn function;
    mvm_kind_t result;
    uint8_t argc;
    const mvm_kind_t *args;
  } builtins[] = {
      {"fileOpen", mvm_builtin_file_open, MVM_KIND_ULONG, 2, open_args},
      {"fileClose", mvm_builtin_file_close, MVM_KIND_BOOL, 1, file_arg},
      {"fileRead", mvm_builtin_file_read, MVM_KIND_SLONG, 2, read_args},
      {"fileReadLine", mvm_builtin_file_read_line, MVM_KIND_STRING, 1, file_arg},
      {"fileWrite", mvm_builtin_file_write, MVM_KIND_SLONG, 2, read_args},
      {"fileSeek", mvm_builtin_file_seek, MVM_KIND_BOOL, 3, seek_args},
      {"fileTell", mvm_builtin_file_tell, MVM_KIND_SLONG, 1, file_arg},
      {"fileFlush", mvm_builtin_file_flush, MVM_KIND_BOOL, 1, file_arg},
      {"fileReadAll", mvm_builtin_file_read_all, MVM_KIND_ARRAY, 1, path_args},
      {"fileWriteAll", mvm_builtin_file_write_all, MVM_KIND_BOOL, 2,
       write_all_args},
      {"fileReadText", mvm_builtin_file_read_text, MVM_KIND_STRING, 1, path_args},
      {"fileWriteText", mvm_builtin_file_write_text, MVM_KIND_BOOL, 2,
       write_text_args},
      {"fileMakeTempDir", mvm_builtin_file_make_temp_dir, MVM_KIND_STRING, 0,
       NULL},
      {"fileRemoveDir", mvm_builtin_file_remove_dir, MVM_KIND_BOOL, 1, path_args},
      {"fileExists", mvm_builtin_file_exists, MVM_KIND_BOOL, 1, path_args},
      {"fileRemove", mvm_builtin_file_remove, MVM_KIND_BOOL, 1, path_args},
      {"fileRename", mvm_builtin_file_rename, MVM_KIND_BOOL, 2, rename_args},
      {"fileLastError", mvm_builtin_file_last_error, MVM_KIND_UWORD, 0, NULL},
      {"fileErrorMessage", mvm_builtin_file_error_message, MVM_KIND_STRING, 1,
       error_args},
  };
  const size_t initial_count = mvm->native_count;
  for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); ++i) {
    if (mvm_register_builtin(mvm, builtins[i].name, builtins[i].function,
                             builtins[i].result, builtins[i].argc,
                             builtins[i].args, NULL) != MVM_RESULT_OK) {
      mvm_native_rewind(mvm, initial_count);
      return MVM_RESULT_VM_ERROR;
    }
  }
  return MVM_RESULT_OK;
}

void mvm_file_shutdown(mvm_t *mvm) {
  if (!mvm || !mvm->files)
    return;
  mvm_file_state_t *state = mvm->files;
  mvm_resource_shutdown(mvm);
  for (size_t i = 0; i < state->count; ++i) {
    MvmFileSlot *slot = state->slots[i];
    if (!slot)
      continue;
    mvm_file_discard(slot, NULL);
    mvm_platform_mutex_destroy(&slot->operation);
    mvm_memory_free(slot);
  }
  mvm_memory_free(state->slots);
  mvm_memory_free(state->image_path);
  mvm_platform_cond_destroy(&state->cond);
  mvm_platform_mutex_destroy(&state->mutex);
  mvm_memory_free(state);
  mvm->files = NULL;
}
