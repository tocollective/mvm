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

#include "mvm.h"

#if defined(_WIN32)
#define TEST_EXPORT __declspec(dllexport)
#else
#define TEST_EXPORT __attribute__((visibility("default")))
#endif

// If rejection leaks this export, the later call to the valid library will
// return 99 instead of 42 because deferred binding finds this candidate first.
static int64_t wrong_answer(void) { return 99; }
static const mvm_kind_t invalid_arguments[] = {MVM_KIND_VOID};
static const mvm_native_export_t functions[] = {
    {"nativeTestAnswer", (void (*)(void))wrong_answer, MVM_KIND_SLONG, 0, NULL},
    {"invalidArgument", (void (*)(void))wrong_answer, MVM_KIND_SLONG, 1,
     invalid_arguments},
};

static const mvm_native_exports_t exports = {
#if defined(TEST_INVALID_NATIVE_ABI)
    MVM_NATIVE_ABI_VERSION + 1,
#else
    MVM_NATIVE_ABI_VERSION,
#endif
    sizeof(functions) / sizeof(functions[0]),
    functions,
};

TEST_EXPORT const mvm_native_exports_t *mvm_get_native_exports(void) {
  return &exports;
}
