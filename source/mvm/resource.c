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

#include "file.h"
#include "zip.h"

#include <string.h>

#define MVM_RESOURCE_MAX_ENTRY (256ULL * 1024 * 1024)
#define MVM_RESOURCE_MAX_TOTAL (1024ULL * 1024 * 1024)
#define MVM_RESOURCE_MAX_ENTRIES 4096

static bool mvm_resource_safe_path(const char *path) {
  if (!path || !*path || *path == '/' || strchr(path, '\\'))
    return false;
  const size_t length = strlen(path);
  if (length >= 2 && path[1] == ':')
    return false;
  size_t start = 0;
  for (size_t i = 0; i <= length; ++i) {
    if (i != length && path[i] != '/')
      continue;
    const size_t size = i - start;
    if (!size || (size == 1 && path[start] == '.') ||
        (size == 2 && path[start] == '.' && path[start + 1] == '.'))
      return false;
    start = i + 1;
  }
  return true;
}

static bool mvm_resource_entry_name(const char *name) {
  const size_t length = name ? strlen(name) : 0;
  return length && strcmp(name, "manifest.toml") != 0 &&
         (length < 3 || strcmp(name + length - 3, ".mo") != 0);
}

static mvm_result_t mvm_resource_archive_error(mvm_t *mvm,
                                                mvm_file_state_t *state,
                                                uint32_t error) {
  mvm_platform_lock(&state->mutex);
  state->archive_error = error;
  mvm_platform_unlock(&state->mutex);
  mvm_file_set_error(mvm, error);
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_resource_archive(mvm_t *mvm) {
  mvm_file_state_t *state = mvm->files;
  if (!state)
    return MVM_RESULT_VM_ERROR;
  mvm_platform_lock(&state->mutex);
  if (state->archive) {
    mvm_platform_unlock(&state->mutex);
    return MVM_RESULT_OK;
  }
  if (state->archive_attempted) {
    mvm_file_set_error(mvm, state->archive_error);
    mvm_platform_unlock(&state->mutex);
    return MVM_RESULT_OK;
  }
  state->archive_attempted = true;
  if (!mvm->binary_path) {
    mvm_platform_unlock(&state->mutex);
    return mvm_resource_archive_error(mvm, state, MVM_FILE_ERROR_NO_IMAGE);
  }
  state->archive = zip_open(state->image_path, 0, 'r');
  if (!state->archive) {
    mvm_platform_unlock(&state->mutex);
    return mvm_resource_archive_error(mvm, state, MVM_FILE_ERROR_IO);
  }
  const ssize_t entries = zip_entries_total(state->archive);
  if (entries < 0) {
    mvm_platform_unlock(&state->mutex);
    return mvm_resource_archive_error(mvm, state, MVM_FILE_ERROR_IO);
  }
  if (entries > MVM_RESOURCE_MAX_ENTRIES) {
    mvm_platform_unlock(&state->mutex);
    return mvm_resource_archive_error(mvm, state, MVM_FILE_ERROR_RESOURCE_LIMIT);
  }
  uint64_t total = 0;
  for (ssize_t i = 0; i < entries; ++i) {
    if (zip_entry_openbyindex(state->archive, (size_t)i) < 0) {
      mvm_platform_unlock(&state->mutex);
      return mvm_resource_archive_error(mvm, state, MVM_FILE_ERROR_IO);
    }
    const char *name = zip_entry_name(state->archive);
    const bool resource = zip_entry_isdir(state->archive) != 1 &&
                          mvm_resource_entry_name(name);
    const unsigned long long size = zip_entry_size(state->archive);
    uint32_t error = MVM_FILE_ERROR_NONE;
    if (resource) {
      if (!mvm_resource_safe_path(name)) {
        error = MVM_FILE_ERROR_INVALID_ARGUMENT;
      } else if (size > MVM_RESOURCE_MAX_ENTRY || size > SIZE_MAX ||
                 total > MVM_RESOURCE_MAX_TOTAL - size) {
        error = MVM_FILE_ERROR_RESOURCE_LIMIT;
      } else {
        total += size;
      }
    }
    if (zip_entry_close(state->archive) < 0)
      error = MVM_FILE_ERROR_IO;
    if (error) {
      mvm_platform_unlock(&state->mutex);
      return mvm_resource_archive_error(mvm, state, error);
    }
  }
  mvm_platform_unlock(&state->mutex);
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_resource_find(mvm_t *mvm, const char *path,
                                      ssize_t *index, size_t *size) {
  *index = -1;
  if (!mvm_resource_safe_path(path) || !mvm_resource_entry_name(path)) {
    mvm_file_set_error(mvm, MVM_FILE_ERROR_INVALID_ARGUMENT);
    return MVM_RESULT_OK;
  }
  const mvm_result_t status = mvm_resource_archive(mvm);
  if (status != MVM_RESULT_OK)
    return status;
  mvm_file_state_t *state = mvm->files;
  if (!state->archive)
    return MVM_RESULT_OK;
  mvm_platform_lock(&state->mutex);
  const ssize_t entries = zip_entries_total(state->archive);
  uint32_t error = entries < 0 ? MVM_FILE_ERROR_IO : MVM_FILE_ERROR_NOT_FOUND;
  for (ssize_t i = 0; i < entries; ++i) {
    if (zip_entry_openbyindex(state->archive, (size_t)i) < 0) {
      error = MVM_FILE_ERROR_IO;
      break;
    }
    const char *name = zip_entry_name(state->archive);
    const bool found = name && zip_entry_isdir(state->archive) != 1 &&
                       mvm_resource_entry_name(name) && strcmp(name, path) == 0;
    if (found) {
      const unsigned long long declared = zip_entry_size(state->archive);
      if (declared > SIZE_MAX) {
        error = MVM_FILE_ERROR_RESOURCE_LIMIT;
        zip_entry_close(state->archive);
        break;
      }
      *size = (size_t)declared;
      *index = i;
      error = MVM_FILE_ERROR_NONE;
    }
    if (zip_entry_close(state->archive) < 0) {
      *index = -1;
      error = MVM_FILE_ERROR_IO;
      break;
    }
    if (found)
      break;
  }
  mvm_platform_unlock(&state->mutex);
  if (*index < 0)
    mvm_file_set_error(mvm, error);
  return MVM_RESULT_OK;
}

mvm_result_t mvm_resource_open(mvm_t *mvm, const char *path, MvmFileSlot *slot) {
  if (!mvm || !slot)
    return MVM_RESULT_VM_ERROR;
  ssize_t index = -1;
  size_t size = 0;
  mvm_result_t status = mvm_resource_find(mvm, path, &index, &size);
  if (status != MVM_RESULT_OK || index < 0)
    return status;
  unsigned char *data = mvm_memory_alloc(mvm->memory, size ? size : 1);
  if (!data)
    return MVM_RESULT_VM_ERROR;

  mvm_file_state_t *state = mvm->files;
  mvm_platform_lock(&state->mutex);
  if (zip_entry_openbyindex(state->archive, (size_t)index) < 0) {
    mvm_platform_unlock(&state->mutex);
    mvm_memory_free(data);
    mvm_file_set_error(mvm, MVM_FILE_ERROR_IO);
    return MVM_RESULT_OK;
  }
  const ssize_t read = zip_entry_noallocread(state->archive, data, size);
  const int close_status = zip_entry_close(state->archive);
  mvm_platform_unlock(&state->mutex);
  if (read < 0 || (size_t)read != size || close_status < 0) {
    mvm_memory_free(data);
    mvm_file_set_error(mvm, MVM_FILE_ERROR_IO);
    return MVM_RESULT_OK;
  }
  slot->data = data;
  slot->size = size;
  slot->offset = 0;
  return MVM_RESULT_OK;
}

bool mvm_resource_exists(mvm_t *mvm, const char *path) {
  ssize_t index;
  size_t size;
  return mvm_resource_find(mvm, path, &index, &size) == MVM_RESULT_OK &&
         index >= 0;
}

void mvm_resource_shutdown(mvm_t *mvm) {
  if (!mvm || !mvm->files || !mvm->files->archive)
    return;
  zip_close(mvm->files->archive);
  mvm->files->archive = NULL;
}
