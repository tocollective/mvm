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
#include "sync_platform.h"

#include <stdio.h>

typedef enum MvmFileBackend {
  MVM_FILE_BACKEND_OS,
  MVM_FILE_BACKEND_RESOURCE,
} MvmFileBackend;

typedef struct MvmFileSlot {
  mvm_platform_mutex_t operation;
  uint32_t generation;
  size_t users;
  bool active;
  bool closing;
  MvmFileBackend backend;
  FILE *stream;
  unsigned char *data;
  size_t size;
  size_t offset;
} MvmFileSlot;

struct mvm_file_state {
  mvm_platform_mutex_t mutex;
  mvm_platform_cond_t cond;
  MvmFileSlot **slots;
  size_t count;
  size_t capacity;
  char *image_path;
  struct zip_t *archive;
  bool archive_attempted;
  uint32_t archive_error;
};

enum {
  MVM_FILE_ERROR_NONE,
  MVM_FILE_ERROR_INVALID_ARGUMENT,
  MVM_FILE_ERROR_NOT_FOUND,
  MVM_FILE_ERROR_PERMISSION,
  MVM_FILE_ERROR_ALREADY_EXISTS,
  MVM_FILE_ERROR_IO,
  MVM_FILE_ERROR_INVALID_HANDLE,
  MVM_FILE_ERROR_READ_ONLY,
  MVM_FILE_ERROR_UNSUPPORTED,
  MVM_FILE_ERROR_INVALID_UTF8,
  MVM_FILE_ERROR_NO_IMAGE,
  MVM_FILE_ERROR_RESOURCE_LIMIT,
};

void mvm_file_clear_error(mvm_t *mvm);
void mvm_file_set_error(mvm_t *mvm, uint32_t error);
void mvm_file_set_errno(mvm_t *mvm, int error);
mvm_result_t mvm_resource_open(mvm_t *mvm, const char *path, MvmFileSlot *slot);
bool mvm_resource_exists(mvm_t *mvm, const char *path);
void mvm_resource_shutdown(mvm_t *mvm);
