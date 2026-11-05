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

static void (*answer_hook)(void);

// Test-only control point, separate from the VM's native export table.
TEST_EXPORT void mvm_test_set_answer_hook(void (*hook)(void)) {
  answer_hook = hook;
}

static int64_t test_answer(void) {
  if (answer_hook)
    answer_hook();
  return 42;
}

static const mvm_native_export_t test_functions[] = {
    {"nativeTestAnswer", (void (*)(void))test_answer, MVM_KIND_SLONG, 0, NULL},
};

static const mvm_native_exports_t test_exports = {
    MVM_NATIVE_ABI_VERSION,
    sizeof(test_functions) / sizeof(test_functions[0]),
    test_functions,
};

TEST_EXPORT const mvm_native_exports_t *mvm_get_native_exports(void) {
  return &test_exports;
}
