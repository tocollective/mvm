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
#include "mvm.h"

// Raw bytecode tests choose an exact instruction-buffer boundary separately
// from the instance's memory budget. Production images size it during linking.
static inline mvm_t *test_vm_create(const char *path, size_t code_size,
                                    size_t frames) {
  mvm_t *vm = mvm_create(path, 0, MVM_MEMORY_DEFAULT_MAX, frames);
  if (vm && !path && !mvm_ram_resize(vm->ram, code_size)) {
    mvm_destroy(vm);
    return NULL;
  }
  return vm;
}
