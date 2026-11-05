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

#include <stdbool.h>

#if defined(_WIN32)
#include <windows.h>
typedef CRITICAL_SECTION mvm_platform_mutex_t;
typedef CONDITION_VARIABLE mvm_platform_cond_t;
typedef DWORD mvm_platform_thread_id_t;
#else
#include <pthread.h>
typedef pthread_mutex_t mvm_platform_mutex_t;
typedef pthread_cond_t mvm_platform_cond_t;
typedef pthread_t mvm_platform_thread_id_t;
#endif

bool mvm_platform_mutex_init(mvm_platform_mutex_t *mutex);
void mvm_platform_mutex_destroy(mvm_platform_mutex_t *mutex);
void mvm_platform_lock(mvm_platform_mutex_t *mutex);
void mvm_platform_unlock(mvm_platform_mutex_t *mutex);
bool mvm_platform_cond_init(mvm_platform_cond_t *cond);
void mvm_platform_cond_destroy(mvm_platform_cond_t *cond);
bool mvm_platform_wait(mvm_platform_cond_t *cond,
                       mvm_platform_mutex_t *mutex);
void mvm_platform_signal(mvm_platform_cond_t *cond);
void mvm_platform_broadcast(mvm_platform_cond_t *cond);
mvm_platform_thread_id_t mvm_platform_thread_id(void);
bool mvm_platform_thread_equal(mvm_platform_thread_id_t left,
                               mvm_platform_thread_id_t right);
