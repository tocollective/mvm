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

#include "sync_platform.h"

bool mvm_platform_mutex_init(mvm_platform_mutex_t *mutex) {
#if defined(_WIN32)
  InitializeCriticalSection(mutex);
  return true;
#else
  return pthread_mutex_init(mutex, NULL) == 0;
#endif
}

void mvm_platform_mutex_destroy(mvm_platform_mutex_t *mutex) {
#if defined(_WIN32)
  DeleteCriticalSection(mutex);
#else
  pthread_mutex_destroy(mutex);
#endif
}

void mvm_platform_lock(mvm_platform_mutex_t *mutex) {
#if defined(_WIN32)
  EnterCriticalSection(mutex);
#else
  pthread_mutex_lock(mutex);
#endif
}

void mvm_platform_unlock(mvm_platform_mutex_t *mutex) {
#if defined(_WIN32)
  LeaveCriticalSection(mutex);
#else
  pthread_mutex_unlock(mutex);
#endif
}

bool mvm_platform_cond_init(mvm_platform_cond_t *cond) {
#if defined(_WIN32)
  InitializeConditionVariable(cond);
  return true;
#else
  return pthread_cond_init(cond, NULL) == 0;
#endif
}

void mvm_platform_cond_destroy(mvm_platform_cond_t *cond) {
#if !defined(_WIN32)
  pthread_cond_destroy(cond);
#else
  (void)cond;
#endif
}

bool mvm_platform_wait(mvm_platform_cond_t *cond,
                       mvm_platform_mutex_t *mutex) {
#if defined(_WIN32)
  return SleepConditionVariableCS(cond, mutex, INFINITE) != 0;
#else
  return pthread_cond_wait(cond, mutex) == 0;
#endif
}

void mvm_platform_signal(mvm_platform_cond_t *cond) {
#if defined(_WIN32)
  WakeConditionVariable(cond);
#else
  pthread_cond_signal(cond);
#endif
}

void mvm_platform_broadcast(mvm_platform_cond_t *cond) {
#if defined(_WIN32)
  WakeAllConditionVariable(cond);
#else
  pthread_cond_broadcast(cond);
#endif
}

mvm_platform_thread_id_t mvm_platform_thread_id(void) {
#if defined(_WIN32)
  return GetCurrentThreadId();
#else
  return pthread_self();
#endif
}

bool mvm_platform_thread_equal(const mvm_platform_thread_id_t left,
                               const mvm_platform_thread_id_t right) {
#if defined(_WIN32)
  return left == right;
#else
  return pthread_equal(left, right) != 0;
#endif
}
