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
#include "sync_platform.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
typedef HANDLE mvm_platform_thread_t;
#if defined(_MSC_VER)
#define MVM_THREAD_LOCAL __declspec(thread)
#else
#define MVM_THREAD_LOCAL _Thread_local
#endif
#else
#include <pthread.h>
typedef pthread_t mvm_platform_thread_t;
#define MVM_THREAD_LOCAL _Thread_local
#endif

typedef enum mvm_thread_state {
  MVM_THREAD_STARTING,
  MVM_THREAD_RUNNING,
  MVM_THREAD_FINISHED,
} mvm_thread_state_t;

typedef struct mvm_thread_control {
  mvm_platform_thread_t os_thread;
  mvm_platform_mutex_t mutex;
  mvm_platform_cond_t finished_cond;
  mvm_thread_state_t state;
  mvm_result_t status;
  mvm_value_t result;
  mvm_gc_root_t result_root;
  mvm_t *runtime;
  mvm_execution_t *execution;
  uint32_t entry;
  struct mvm_thread_control *next;
  bool created;
  bool joined;
  bool detached;
  atomic_bool interrupt_requested;
  atomic_size_t references;
} mvm_thread_control_t;

typedef struct mvm_runtime {
  mvm_platform_mutex_t mutex;
  mvm_platform_cond_t cond;
  mvm_thread_control_t *controls;
  size_t active_workers;
  atomic_bool gc_requested;
} mvm_runtime_t;

static MVM_THREAD_LOCAL mvm_execution_t *mvm_current;

mvm_execution_t *mvm_current_execution(const mvm_t *mvm) {
  return mvm_current && mvm_current->runtime == mvm ? mvm_current : NULL;
}

void mvm_runtime_lock(mvm_t *mvm) {
  if (mvm && mvm->runtime)
    mvm_platform_lock(&((mvm_runtime_t *)mvm->runtime)->mutex);
}

void mvm_runtime_unlock(mvm_t *mvm) {
  if (mvm && mvm->runtime)
    mvm_platform_unlock(&((mvm_runtime_t *)mvm->runtime)->mutex);
}

bool mvm_runtime_init(mvm_t *mvm) {
  if (!mvm || mvm->runtime)
    return false;
  mvm_runtime_t *runtime = mvm_memory_calloc(mvm->memory, 1, sizeof(*runtime));
  if (!runtime)
    return false;
  if (!mvm_platform_mutex_init(&runtime->mutex)) {
    mvm_memory_free(runtime);
    return false;
  }
  if (!mvm_platform_cond_init(&runtime->cond)) {
    mvm_platform_mutex_destroy(&runtime->mutex);
    mvm_memory_free(runtime);
    return false;
  }
  atomic_init(&runtime->gc_requested, false);
  mvm->runtime = runtime;
  return true;
}

static atomic_bool *mvm_execution_flag(mvm_t *mvm,
                                       mvm_execution_t *execution) {
  return execution ? &execution->executing : &mvm->executing;
}

static size_t *mvm_execution_depth(mvm_t *mvm, mvm_execution_t *execution) {
  return execution ? &execution->execution_depth : &mvm->execution_depth;
}

void mvm_execution_poll(mvm_t *mvm) {
  if (!mvm || !mvm->runtime)
    return;
  mvm_runtime_t *runtime = mvm->runtime;
  if (!atomic_load_explicit(&runtime->gc_requested, memory_order_acquire))
    return;
  mvm_execution_t *execution = mvm_current_execution(mvm);
  atomic_bool *executing = mvm_execution_flag(mvm, execution);
  const bool was_executing =
      atomic_load_explicit(executing, memory_order_acquire);
  atomic_store_explicit(executing, false, memory_order_release);
  mvm_platform_lock(&runtime->mutex);
  mvm_platform_broadcast(&runtime->cond);
  while (atomic_load_explicit(&runtime->gc_requested, memory_order_acquire))
    mvm_platform_wait(&runtime->cond, &runtime->mutex);
  atomic_store_explicit(executing, was_executing, memory_order_release);
  mvm_platform_unlock(&runtime->mutex);
}

void mvm_execution_enter(mvm_t *mvm) {
  if (!mvm || !mvm->runtime)
    return;
  mvm_execution_t *execution = mvm_current_execution(mvm);
  atomic_bool *executing = mvm_execution_flag(mvm, execution);
  ++*mvm_execution_depth(mvm, execution);
  atomic_store_explicit(executing, true, memory_order_release);
  mvm_execution_poll(mvm);
}

void mvm_execution_leave(mvm_t *mvm) {
  if (!mvm || !mvm->runtime)
    return;
  mvm_runtime_t *runtime = mvm->runtime;
  mvm_execution_t *execution = mvm_current_execution(mvm);
  size_t *depth = mvm_execution_depth(mvm, execution);
  if (*depth && --*depth == 0) {
    atomic_store_explicit(mvm_execution_flag(mvm, execution), false,
                          memory_order_release);
    if (atomic_load_explicit(&runtime->gc_requested, memory_order_acquire)) {
      mvm_platform_lock(&runtime->mutex);
      mvm_platform_broadcast(&runtime->cond);
      mvm_platform_unlock(&runtime->mutex);
    }
  }
}

bool mvm_runtime_gc_begin(mvm_t *mvm) {
  if (!mvm || !mvm->runtime)
    return true;
  mvm_runtime_t *runtime = mvm->runtime;
  mvm_execution_t *current = mvm_current_execution(mvm);
  mvm_platform_lock(&runtime->mutex);
  if (atomic_load_explicit(&runtime->gc_requested, memory_order_acquire)) {
    atomic_bool *executing = mvm_execution_flag(mvm, current);
    const bool was_executing = atomic_load_explicit(executing, memory_order_acquire);
    atomic_store_explicit(executing, false, memory_order_release);
    mvm_platform_broadcast(&runtime->cond);
    while (atomic_load_explicit(&runtime->gc_requested, memory_order_acquire))
      mvm_platform_wait(&runtime->cond, &runtime->mutex);
    atomic_store_explicit(executing, was_executing, memory_order_release);
    mvm_platform_unlock(&runtime->mutex);
    return false;
  }
  atomic_store_explicit(&runtime->gc_requested, true, memory_order_release);
  for (;;) {
    bool busy = current != NULL &&
                atomic_load_explicit(&mvm->executing, memory_order_acquire);
    for (mvm_execution_t *execution = mvm->executions; execution && !busy;
         execution = execution->next)
      busy = execution != current &&
             atomic_load_explicit(&execution->executing, memory_order_acquire);
    if (!busy)
      break;
    mvm_platform_wait(&runtime->cond, &runtime->mutex);
  }
  return true;
}

void mvm_runtime_gc_end(mvm_t *mvm) {
  if (!mvm || !mvm->runtime)
    return;
  mvm_runtime_t *runtime = mvm->runtime;
  atomic_store_explicit(&runtime->gc_requested, false, memory_order_release);
  mvm_platform_broadcast(&runtime->cond);
  mvm_platform_unlock(&runtime->mutex);
}

static void mvm_thread_run(mvm_thread_control_t *control);

#if defined(_WIN32)
static DWORD WINAPI mvm_platform_main(void *argument) {
  mvm_thread_control_t *control = argument;
  mvm_current = control->execution;
  mvm_thread_run(control);
  mvm_current = NULL;
  return 0;
}
#else
static void *mvm_platform_main(void *argument) {
  mvm_thread_control_t *control = argument;
  mvm_current = control->execution;
  mvm_thread_run(control);
  mvm_current = NULL;
  return NULL;
}
#endif

static bool mvm_platform_start(mvm_thread_control_t *control) {
#if defined(_WIN32)
  control->os_thread =
      CreateThread(NULL, 0, mvm_platform_main, control, 0, NULL);
  return control->os_thread != NULL;
#else
  return pthread_create(&control->os_thread, NULL, mvm_platform_main, control) ==
         0;
#endif
}

static bool mvm_platform_join(mvm_thread_control_t *control) {
#if defined(_WIN32)
  if (WaitForSingleObject(control->os_thread, INFINITE) != WAIT_OBJECT_0)
    return false;
  return CloseHandle(control->os_thread) != 0;
#else
  return pthread_join(control->os_thread, NULL) == 0;
#endif
}

static bool mvm_platform_detach(mvm_thread_control_t *control) {
#if defined(_WIN32)
  return CloseHandle(control->os_thread) != 0;
#else
  return pthread_detach(control->os_thread) == 0;
#endif
}

static void mvm_thread_control_release(mvm_thread_control_t *control) {
  if (atomic_fetch_sub_explicit(&control->references, 1,
                                memory_order_acq_rel) != 1)
    return;
  mvm_platform_cond_destroy(&control->finished_cond);
  mvm_platform_mutex_destroy(&control->mutex);
  mvm_memory_free(control);
}

static mvm_execution_t *mvm_execution_new(mvm_t *mvm) {
  mvm_execution_t *execution =
      mvm_memory_calloc(mvm->memory, 1, sizeof(*execution));
  if (!execution)
    return NULL;
  execution->stack =
      mvm_stack_create_managed(mvm->memory, MVM_STACK_MIN_CAPACITY);
  if (!execution->stack) {
    mvm_memory_free(execution);
    return NULL;
  }
  execution->runtime = mvm;
  execution->frame_limit = mvm->frame_limit;
  execution->working = true;
  mvm_runtime_lock(mvm);
  execution->next = mvm->executions;
  mvm->executions = execution;
  mvm_runtime_unlock(mvm);
  return execution;
}

static void mvm_execution_destroy(mvm_execution_t *execution) {
  if (!execution)
    return;
  mvm_t *mvm = execution->runtime;
  mvm_runtime_lock(mvm);
  for (mvm_execution_t **current = &mvm->executions; *current;
       current = &(*current)->next) {
    if (*current == execution) {
      *current = execution->next;
      break;
    }
  }
  mvm_runtime_unlock(mvm);
  mvm_memory_free(execution->frames);
  mvm_stack_destroy(execution->stack);
  mvm_memory_free(execution);
}

static mvm_thread_control_t *
mvm_thread_control_new(mvm_t *mvm, mvm_execution_t *execution) {
  mvm_thread_control_t *control =
      mvm_memory_calloc(mvm->memory, 1, sizeof(*control));
  if (!control)
    return NULL;
  if (!mvm_platform_mutex_init(&control->mutex)) {
    mvm_memory_free(control);
    return NULL;
  }
  if (!mvm_platform_cond_init(&control->finished_cond)) {
    mvm_platform_mutex_destroy(&control->mutex);
    mvm_memory_free(control);
    return NULL;
  }
  control->state = MVM_THREAD_STARTING;
  control->status = MVM_RESULT_OK;
  control->runtime = mvm;
  control->execution = execution;
  execution->control = control;
  atomic_init(&control->interrupt_requested, false);
  atomic_init(&control->references, 2);
  if (!mvm_gc_root_register(mvm, &control->result_root, &control->result, 1)) {
    mvm_platform_cond_destroy(&control->finished_cond);
    mvm_platform_mutex_destroy(&control->mutex);
    mvm_memory_free(control);
    return NULL;
  }
  mvm_runtime_lock(mvm);
  control->next = ((mvm_runtime_t *)mvm->runtime)->controls;
  ((mvm_runtime_t *)mvm->runtime)->controls = control;
  mvm_runtime_unlock(mvm);
  return control;
}

static void mvm_thread_control_unlink(mvm_thread_control_t *control) {
  mvm_t *mvm = control->runtime;
  mvm_runtime_lock(mvm);
  mvm_runtime_t *runtime = mvm->runtime;
  for (mvm_thread_control_t **current = &runtime->controls; *current;
       current = &(*current)->next) {
    if (*current == control) {
      *current = control->next;
      break;
    }
  }
  mvm_runtime_unlock(mvm);
  mvm_gc_root_unregister(mvm, &control->result_root);
}

static void mvm_thread_control_detach(mvm_thread_control_t *control) {
  mvm_platform_lock(&control->mutex);
  if (control->created && !control->joined && !control->detached) {
    if (mvm_platform_detach(control))
      control->detached = true;
  }
  mvm_platform_unlock(&control->mutex);
}

void mvm_thread_release_handle(mvm_thread_t *thread) {
  if (!thread || !thread->control)
    return;
  mvm_thread_control_t *control = thread->control;
  thread->control = NULL;
  mvm_thread_control_detach(control);
  mvm_thread_control_release(control);
}

mvm_thread_t *mvm_checked_thread(const mvm_t *mvm, const mvm_value_t value) {
  if (value.kind != MVM_KIND_THREAD)
    return NULL;
  const mvm_object_t *object = mvm_gc_find(mvm, value.as.thread);
  if (!object || object->kind != MVM_OBJECT_THREAD ||
      (const void *)object->data != (const void *)value.as.thread)
    return NULL;
  return value.as.thread;
}

static mvm_result_t mvm_thread_validate(const mvm_t *mvm,
                                        const mvm_value_t value,
                                        mvm_thread_control_t **control) {
  const mvm_thread_t *thread = mvm_checked_thread(mvm, value);
  if (!thread || !thread->control)
    return MVM_RESULT_RUNTIME_ERROR;
  *control = thread->control;
  return MVM_RESULT_OK;
}

static const mvm_type_t *mvm_thread_entry_type(const mvm_t *mvm,
                                                const mvm_value_t entry) {
  const mvm_function_type_t *function =
      mvm_function_at((mvm_t *)mvm, entry.as.func);
  return function ? mvm_type_by_id(mvm, function->type_id) : NULL;
}

static mvm_result_t mvm_thread_validate_arguments(
    const mvm_t *mvm, mvm_value_t entry, const mvm_value_t *arguments,
    size_t argument_count) {
  const mvm_type_t *type = mvm_thread_entry_type(mvm, entry);
  if (!type)
    return MVM_RESULT_OK;
  if (type->tag != MVM_TYPE_FUNCTION || type->argc != argument_count)
    return MVM_RESULT_RUNTIME_ERROR;
  for (size_t i = 0; i < argument_count; ++i)
    if (!mvm_value_matches_type(mvm, type->param_type_ids[i], arguments[i]))
      return MVM_RESULT_RUNTIME_ERROR;
  return MVM_RESULT_OK;
}

static void mvm_thread_run(mvm_thread_control_t *control) {
  mvm_t *mvm = control->runtime;
  mvm_execution_t *execution = control->execution;
  mvm_platform_lock(&control->mutex);
  control->state = MVM_THREAD_RUNNING;
  mvm_platform_unlock(&control->mutex);

  mvm_execution_enter(mvm);
  mvm_result_t status = MVM_RESULT_OK;
  while (execution->working) {
    if (atomic_load_explicit(&control->interrupt_requested,
                             memory_order_acquire)) {
      status = MVM_RESULT_INTERRUPT;
      break;
    }
    status = mvm_step(mvm, execution);
    if (status != MVM_RESULT_OK)
      break;
  }
  mvm_execution_leave(mvm);

  mvm_value_t result = {.kind = MVM_KIND_VOID};
  if (status == MVM_RESULT_OK) {
    if (!execution->stack || execution->stack->size == 0 ||
        execution->stack->data[execution->stack->size - 1].kind == MVM_KIND_PACKET) {
      status = MVM_RESULT_RUNTIME_ERROR;
    } else {
      result = execution->stack->data[execution->stack->size - 1];
      const mvm_type_t *type = mvm_thread_entry_type(
          mvm, (mvm_value_t){.kind = MVM_KIND_FUNC, .as.func = control->entry});
      const mvm_type_t *result_type =
          type ? mvm_type_by_id(mvm, type->result_type_id) : NULL;
      if (result_type &&
          (result_type->kind == MVM_KIND_VOID
               ? result.kind != MVM_KIND_VOID && result.kind != MVM_KIND_NULL
               : !mvm_value_matches_type(mvm, type->result_type_id, result)))
        status = MVM_RESULT_RUNTIME_ERROR;
    }
  }
  mvm_runtime_lock(mvm);
  control->result = result;
  control->execution = NULL;
  mvm_runtime_unlock(mvm);
  mvm_execution_destroy(execution);

  mvm_platform_lock(&control->mutex);
  control->status = status;
  control->state = MVM_THREAD_FINISHED;
  mvm_platform_broadcast(&control->finished_cond);
  mvm_platform_unlock(&control->mutex);
  mvm_thread_control_release(control);
  // A detached worker must finish using the pool before shutdown destroys it.
  mvm_runtime_lock(mvm);
  mvm_runtime_t *runtime = mvm->runtime;
  --runtime->active_workers;
  mvm_platform_broadcast(&runtime->cond);
  mvm_runtime_unlock(mvm);
}

static mvm_result_t mvm_thread_spawn_active(
    mvm_t *mvm, const mvm_value_t entry, const mvm_value_t *arguments,
    const size_t argument_count, mvm_value_t *result) {
  if (!mvm || !result || (argument_count && !arguments))
    return MVM_RESULT_VM_ERROR;
  if (!mvm->ram || (mvm->ram->size && !mvm->ram->data) ||
      entry.kind != MVM_KIND_FUNC || entry.as.func >= mvm->ram->size ||
      argument_count > UINT8_MAX)
    return MVM_RESULT_RUNTIME_ERROR;
  mvm_runtime_lock(mvm);
  const bool shutting_down = mvm->shutting_down;
  mvm_runtime_unlock(mvm);
  if (shutting_down)
    return MVM_RESULT_VM_ERROR;
  const mvm_result_t validation =
      mvm_thread_validate_arguments(mvm, entry, arguments, argument_count);
  if (validation != MVM_RESULT_OK)
    return validation;

  mvm_execution_t *execution = mvm_execution_new(mvm);
  if (!execution)
    return MVM_RESULT_VM_ERROR;
  mvm_function_type_t *function = mvm_function_at(mvm, entry.as.func);
  if (function && function->max_stack < argument_count) {
    mvm_execution_destroy(execution);
    return MVM_RESULT_RUNTIME_ERROR;
  }
  if (function && !mvm_stack_reserve(execution->stack, function->max_stack)) {
    mvm_execution_destroy(execution);
    return MVM_RESULT_VM_ERROR;
  }
  for (size_t i = 0; i < argument_count; ++i) {
    if (!mvm_stack_push(execution->stack, arguments[i])) {
      mvm_execution_destroy(execution);
      return MVM_RESULT_VM_ERROR;
    }
  }
  execution->pc = entry.as.func;
  execution->function = function;

  mvm_thread_control_t *control = mvm_thread_control_new(mvm, execution);
  if (!control) {
    mvm_execution_destroy(execution);
    return MVM_RESULT_VM_ERROR;
  }
  control->entry = entry.as.func;

  mvm_object_t *object =
      mvm_gc_allocate(mvm, MVM_OBJECT_THREAD, sizeof(mvm_thread_t));
  if (!object) {
    mvm_thread_control_unlink(control);
    mvm_execution_destroy(execution);
    mvm_thread_control_release(control);
    mvm_thread_control_release(control);
    return MVM_RESULT_VM_ERROR;
  }
  mvm_thread_t *thread = (mvm_thread_t *)object->data;
  thread->control = control;
  atomic_fetch_add_explicit(&control->references, 1, memory_order_relaxed);

  mvm_runtime_lock(mvm);
  ++((mvm_runtime_t *)mvm->runtime)->active_workers;
  mvm_runtime_unlock(mvm);
  mvm_platform_lock(&control->mutex);
  control->created = true;
  const bool started = mvm_platform_start(control);
  if (!started)
    control->created = false;
  mvm_platform_unlock(&control->mutex);
  if (!started) {
    mvm_runtime_lock(mvm);
    --((mvm_runtime_t *)mvm->runtime)->active_workers;
    mvm_platform_broadcast(&((mvm_runtime_t *)mvm->runtime)->cond);
    mvm_runtime_unlock(mvm);
    thread->control = NULL;
    mvm_thread_control_unlink(control);
    mvm_execution_destroy(execution);
    mvm_thread_control_release(control);
    mvm_thread_control_release(control);
    mvm_thread_control_release(control);
    return MVM_RESULT_VM_ERROR;
  }

  *result = (mvm_value_t){.kind = MVM_KIND_THREAD, .as.thread = thread};
  return MVM_RESULT_OK;
}

mvm_result_t mvm_thread_spawn(mvm_t *mvm, const mvm_value_t entry,
                              const mvm_value_t *arguments,
                              const size_t argument_count, mvm_value_t *result) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  mvm_execution_enter(mvm);
  const mvm_result_t status =
      mvm_thread_spawn_active(mvm, entry, arguments, argument_count, result);
  mvm_execution_leave(mvm);
  return status;
}

mvm_result_t mvm_thread_join(mvm_t *mvm, const mvm_value_t thread,
                             mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  mvm_thread_control_t *control;
  const mvm_result_t validation = mvm_thread_validate(mvm, thread, &control);
  if (validation != MVM_RESULT_OK)
    return validation;
  mvm_execution_t *execution = mvm_current_execution(mvm);
  const bool active = execution ? execution->execution_depth > 0
                                : mvm->execution_depth > 0;

  mvm_runtime_lock(mvm);
  const bool self = execution && control->execution == execution;
  mvm_runtime_unlock(mvm);
  mvm_platform_lock(&control->mutex);
  if (self || control->detached) {
    mvm_platform_unlock(&control->mutex);
    return MVM_RESULT_RUNTIME_ERROR;
  }
  if (active)
    mvm_execution_leave(mvm);
  mvm_result_t status = MVM_RESULT_OK;
  while (control->state != MVM_THREAD_FINISHED) {
    if (!mvm_platform_wait(&control->finished_cond, &control->mutex)) {
      status = MVM_RESULT_VM_ERROR;
      goto done;
    }
  }
  if (!control->joined) {
    if (!mvm_platform_join(control)) {
      status = MVM_RESULT_VM_ERROR;
      goto done;
    }
    control->joined = true;
  }
  status = control->status;

done: {
    const mvm_value_t value = control->result;
    mvm_platform_unlock(&control->mutex);
    if (active)
      mvm_execution_enter(mvm);
    if (status == MVM_RESULT_OK)
      *result = value;
  }
  return status;
}

mvm_result_t mvm_thread_detach(mvm_t *mvm, const mvm_value_t thread) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  mvm_thread_control_t *control;
  const mvm_result_t validation = mvm_thread_validate(mvm, thread, &control);
  if (validation != MVM_RESULT_OK)
    return validation;

  mvm_platform_lock(&control->mutex);
  mvm_result_t status = MVM_RESULT_OK;
  if (!control->joined && !control->detached) {
    if (mvm_platform_detach(control))
      control->detached = true;
    else
      status = MVM_RESULT_VM_ERROR;
  }
  mvm_platform_unlock(&control->mutex);
  return status;
}

mvm_result_t mvm_thread_interrupt(mvm_t *mvm, const mvm_value_t thread) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  mvm_thread_control_t *control;
  const mvm_result_t validation = mvm_thread_validate(mvm, thread, &control);
  if (validation != MVM_RESULT_OK)
    return validation;
  atomic_store_explicit(&control->interrupt_requested, true,
                        memory_order_release);
  mvm_runtime_lock(mvm);
  mvm_sync_interrupt_execution(control->execution);
  mvm_runtime_unlock(mvm);
  return MVM_RESULT_OK;
}

bool mvm_thread_execution_interrupted(const mvm_execution_t *execution) {
  return execution && execution->control &&
         atomic_load_explicit(&execution->control->interrupt_requested,
                              memory_order_acquire);
}

void mvm_thread_shutdown(mvm_t *mvm) {
  if (!mvm || !mvm->runtime)
    return;
  mvm_runtime_t *runtime = mvm->runtime;
  mvm_runtime_lock(mvm);
  mvm->shutting_down = true;
  for (mvm_thread_control_t *control = runtime->controls; control;
       control = control->next) {
    atomic_store_explicit(&control->interrupt_requested, true,
                          memory_order_release);
    mvm_sync_interrupt_execution(control->execution);
  }
  if (mvm->waiting)
    mvm_sync_interrupt_execution(&(mvm_execution_t){.waiting = mvm->waiting});
  mvm_runtime_unlock(mvm);

  for (mvm_thread_control_t *control = runtime->controls; control;
       control = control->next) {
    mvm_platform_lock(&control->mutex);
    while (control->state != MVM_THREAD_FINISHED)
      mvm_platform_wait(&control->finished_cond, &control->mutex);
    if (control->created && !control->joined && !control->detached) {
      if (mvm_platform_join(control))
        control->joined = true;
    }
    mvm_platform_unlock(&control->mutex);
  }

  mvm_platform_lock(&runtime->mutex);
  while (runtime->active_workers)
    mvm_platform_wait(&runtime->cond, &runtime->mutex);
  mvm_platform_unlock(&runtime->mutex);
  mvm_sync_shutdown(mvm);

  while (runtime->controls) {
    mvm_thread_control_t *control = runtime->controls;
    runtime->controls = control->next;
    mvm_gc_root_unregister(mvm, &control->result_root);
    mvm_thread_control_release(control);
  }
  mvm_platform_cond_destroy(&runtime->cond);
  mvm_platform_mutex_destroy(&runtime->mutex);
  mvm_memory_free(runtime);
  mvm->runtime = NULL;
}

static mvm_result_t mvm_builtin_spawn(mvm_t *mvm, const mvm_value_t *args,
                                      mvm_value_t *result) {
  if (args[0].kind != MVM_KIND_FUNC)
    return MVM_RESULT_RUNTIME_ERROR;
  if (args[1].kind == MVM_KIND_PACKET) {
    const mvm_packet_t *packet = mvm_checked_packet(mvm, args[1]);
    if (!packet)
      return MVM_RESULT_RUNTIME_ERROR;
    return mvm_thread_spawn(mvm, args[0], packet->values, packet->count, result);
  }
  return mvm_thread_spawn(mvm, args[0], args + 1, 1, result);
}

static mvm_result_t mvm_builtin_join(mvm_t *mvm, const mvm_value_t *args,
                                     mvm_value_t *result) {
  return mvm_thread_join(mvm, args[0], result);
}

static mvm_result_t mvm_builtin_detach(mvm_t *mvm, const mvm_value_t *args,
                                       mvm_value_t *result) {
  const mvm_result_t status = mvm_thread_detach(mvm, args[0]);
  if (status == MVM_RESULT_OK)
    *result = (mvm_value_t){.kind = MVM_KIND_VOID};
  return status;
}

static mvm_result_t mvm_builtin_interrupt(mvm_t *mvm, const mvm_value_t *args,
                                          mvm_value_t *result) {
  const mvm_result_t status = mvm_thread_interrupt(mvm, args[0]);
  if (status == MVM_RESULT_OK)
    *result = (mvm_value_t){.kind = MVM_KIND_VOID};
  return status;
}

mvm_result_t mvm_thread_register(mvm_t *mvm) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  static const char *const names[] = {
      "threadSpawn", "threadJoin", "threadDetach", "threadInterrupt",
  };
  size_t found = 0;
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    uint16_t id;
    if (mvm_find_native(mvm, names[i], &id))
      ++found;
  }
  if (found == sizeof(names) / sizeof(names[0]))
    return MVM_RESULT_OK;
  if (found)
    return MVM_RESULT_VM_ERROR;

  static const mvm_kind_t thread_arg[] = {MVM_KIND_THREAD};
  const size_t initial_count = mvm->native_count;
  mvm_result_t status = mvm_register_builtin_dynamic(mvm, names[0],
                                                      mvm_builtin_spawn, NULL);
  if (status == MVM_RESULT_OK)
    status = mvm_register_builtin_dynamic(mvm, names[1], mvm_builtin_join, NULL);
  if (status == MVM_RESULT_OK)
    status = mvm_register_builtin(mvm, names[2], mvm_builtin_detach,
                                  MVM_KIND_VOID, 1, thread_arg, NULL);
  if (status == MVM_RESULT_OK)
    status = mvm_register_builtin(mvm, names[3], mvm_builtin_interrupt,
                                  MVM_KIND_VOID, 1, thread_arg, NULL);
  if (status != MVM_RESULT_OK)
    mvm_native_rewind(mvm, initial_count);
  return status;
}
