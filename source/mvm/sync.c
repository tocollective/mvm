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

typedef enum mvm_sync_control_kind {
  MVM_SYNC_MUTEX,
  MVM_SYNC_CONDITION,
  MVM_SYNC_ATOMIC,
} mvm_sync_control_kind_t;

typedef struct mvm_sync_waiter {
  mvm_platform_mutex_t mutex;
  mvm_platform_cond_t cond;
  bool notified;
  struct mvm_sync_waiter *next;
} mvm_sync_waiter_t;

typedef struct mvm_sync_control {
  mvm_t *runtime;
  mvm_sync_control_kind_t kind;
  mvm_platform_mutex_t mutex;
  atomic_size_t references;
  bool handle_alive;
  bool linked;
  mvm_sync_waiter_t *waiters;
  struct mvm_sync_control *next;
  union {
    struct {
      bool locked;
      bool owner_held;
      mvm_execution_t *owner;
      mvm_platform_thread_id_t owner_id;
    } mutex;
    struct {
      atomic_uint_least64_t bits;
      mvm_kind_t kind;
    } atomic;
  } as;
} mvm_sync_control_t;

static void mvm_sync_control_release_raw(mvm_sync_control_t *control) {
  if (atomic_fetch_sub_explicit(&control->references, 1,
                                memory_order_acq_rel) != 1)
    return;
  mvm_platform_mutex_destroy(&control->mutex);
  mvm_memory_free(control);
}

static void mvm_sync_control_maybe_reclaim(mvm_sync_control_t *control) {
  mvm_t *mvm = control->runtime;
  if (!mvm || !mvm->runtime)
    return;
  bool reclaimed = false;
  mvm_runtime_lock(mvm);
  if (control->linked && !control->handle_alive &&
      atomic_load_explicit(&control->references, memory_order_acquire) == 1) {
    for (mvm_sync_control_t **current = &mvm->sync_controls; *current;
         current = &(*current)->next) {
      if (*current == control) {
        *current = control->next;
        control->linked = false;
        reclaimed = true;
        break;
      }
    }
  }
  mvm_runtime_unlock(mvm);
  if (reclaimed)
    mvm_sync_control_release_raw(control);
}

static void mvm_sync_control_retain(mvm_sync_control_t *control) {
  atomic_fetch_add_explicit(&control->references, 1, memory_order_relaxed);
}

static void mvm_sync_control_release(mvm_sync_control_t *control) {
  mvm_sync_control_release_raw(control);
  mvm_sync_control_maybe_reclaim(control);
}

static mvm_sync_control_t *mvm_sync_control_new(mvm_t *mvm,
                                                mvm_sync_control_kind_t kind) {
  if (!mvm || !mvm->runtime)
    return NULL;
  mvm_sync_control_t *control =
      mvm_memory_calloc(mvm->memory, 1, sizeof(*control));
  if (!control)
    return NULL;
  if (!mvm_platform_mutex_init(&control->mutex)) {
    mvm_memory_free(control);
    return NULL;
  }
  control->runtime = mvm;
  control->kind = kind;
  control->handle_alive = true;
  control->linked = true;
  atomic_init(&control->references, 2);
  mvm_runtime_lock(mvm);
  control->next = mvm->sync_controls;
  mvm->sync_controls = control;
  mvm_runtime_unlock(mvm);
  return control;
}

static void mvm_sync_control_abandon(mvm_sync_control_t *control) {
  mvm_t *mvm = control->runtime;
  mvm_runtime_lock(mvm);
  for (mvm_sync_control_t **current = &mvm->sync_controls; *current;
       current = &(*current)->next) {
    if (*current == control) {
      *current = control->next;
      control->linked = false;
      break;
    }
  }
  control->handle_alive = false;
  mvm_runtime_unlock(mvm);
  mvm_sync_control_release_raw(control);
  mvm_sync_control_release_raw(control);
}

static void mvm_sync_release_handle(mvm_sync_control_t *control) {
  if (!control)
    return;
  mvm_t *mvm = control->runtime;
  const bool linked = control->linked;
  control->handle_alive = false;
  mvm_sync_control_release_raw(control);
  if (!linked ||
      atomic_load_explicit(&control->references, memory_order_acquire) != 1)
    return;
  for (mvm_sync_control_t **current = &mvm->sync_controls; *current;
       current = &(*current)->next) {
    if (*current == control) {
      *current = control->next;
      control->linked = false;
      mvm_sync_control_release_raw(control);
      return;
    }
  }
}

static mvm_result_t mvm_sync_handle_new(mvm_t *mvm,
                                        mvm_sync_control_kind_t kind,
                                        mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  mvm_sync_control_t *control = mvm_sync_control_new(mvm, kind);
  if (!control)
    return MVM_RESULT_VM_ERROR;
  mvm_object_kind_t object_kind;
  mvm_kind_t value_kind;
  size_t size;
  switch (kind) {
  case MVM_SYNC_MUTEX:
    object_kind = MVM_OBJECT_MUTEX;
    value_kind = MVM_KIND_MUTEX;
    size = sizeof(mvm_mutex_t);
    break;
  case MVM_SYNC_CONDITION:
    object_kind = MVM_OBJECT_CONDITION;
    value_kind = MVM_KIND_CONDITION;
    size = sizeof(mvm_condition_t);
    break;
  case MVM_SYNC_ATOMIC:
    object_kind = MVM_OBJECT_ATOMIC;
    value_kind = MVM_KIND_ATOMIC;
    size = sizeof(mvm_atomic_t);
    break;
  default:
    mvm_sync_control_abandon(control);
    return MVM_RESULT_VM_ERROR;
  }
  mvm_object_t *object = mvm_gc_allocate(mvm, object_kind, size);
  if (!object) {
    mvm_sync_control_abandon(control);
    return MVM_RESULT_VM_ERROR;
  }
  mvm_value_t value = {.kind = value_kind};
  if (kind == MVM_SYNC_MUTEX) {
    ((mvm_mutex_t *)object->data)->control = control;
    value.as.mutex = (mvm_mutex_t *)object->data;
  } else if (kind == MVM_SYNC_CONDITION) {
    ((mvm_condition_t *)object->data)->control = control;
    value.as.condition = (mvm_condition_t *)object->data;
  } else {
    ((mvm_atomic_t *)object->data)->control = control;
    value.as.atomic = (mvm_atomic_t *)object->data;
  }
  *result = value;
  return MVM_RESULT_OK;
}

static void *mvm_sync_checked_handle(const mvm_t *mvm,
                                     mvm_object_kind_t object_kind,
                                     const void *pointer) {
  const mvm_object_t *object = mvm_gc_find(mvm, pointer);
  if (!object || object->kind != object_kind || object->data != pointer)
    return NULL;
  return (void *)pointer;
}

mvm_mutex_t *mvm_checked_mutex(const mvm_t *mvm, mvm_value_t value) {
  if (value.kind != MVM_KIND_MUTEX)
    return NULL;
  return mvm_sync_checked_handle(mvm, MVM_OBJECT_MUTEX, value.as.mutex);
}

mvm_condition_t *mvm_checked_condition(const mvm_t *mvm, mvm_value_t value) {
  if (value.kind != MVM_KIND_CONDITION)
    return NULL;
  return mvm_sync_checked_handle(mvm, MVM_OBJECT_CONDITION,
                                 value.as.condition);
}

mvm_atomic_t *mvm_checked_atomic(const mvm_t *mvm, mvm_value_t value) {
  if (value.kind != MVM_KIND_ATOMIC)
    return NULL;
  return mvm_sync_checked_handle(mvm, MVM_OBJECT_ATOMIC, value.as.atomic);
}

bool mvm_sync_value_valid(const mvm_t *mvm, const mvm_value_t value) {
  switch (value.kind) {
  case MVM_KIND_MUTEX:
    return mvm_checked_mutex(mvm, value) != NULL;
  case MVM_KIND_CONDITION:
    return mvm_checked_condition(mvm, value) != NULL;
  case MVM_KIND_ATOMIC:
    return mvm_checked_atomic(mvm, value) != NULL;
  default:
    return true;
  }
}

void mvm_mutex_release_handle(mvm_mutex_t *mutex) {
  if (!mutex || !mutex->control)
    return;
  mvm_sync_control_t *control = mutex->control;
  mutex->control = NULL;
  mvm_sync_release_handle(control);
}

void mvm_condition_release_handle(mvm_condition_t *condition) {
  if (!condition || !condition->control)
    return;
  mvm_sync_control_t *control = condition->control;
  condition->control = NULL;
  mvm_sync_release_handle(control);
}

void mvm_atomic_release_handle(mvm_atomic_t *atomic) {
  if (!atomic || !atomic->control)
    return;
  mvm_sync_control_t *control = atomic->control;
  atomic->control = NULL;
  mvm_sync_release_handle(control);
}

static mvm_result_t mvm_mutex_control(mvm_t *mvm, mvm_value_t value,
                                      mvm_sync_control_t **result) {
  mvm_mutex_t *mutex = mvm_checked_mutex(mvm, value);
  if (!mutex || !mutex->control ||
      mutex->control->kind != MVM_SYNC_MUTEX)
    return MVM_RESULT_RUNTIME_ERROR;
  *result = mutex->control;
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_condition_control(mvm_t *mvm, mvm_value_t value,
                                          mvm_sync_control_t **result) {
  mvm_condition_t *condition = mvm_checked_condition(mvm, value);
  if (!condition || !condition->control ||
      condition->control->kind != MVM_SYNC_CONDITION)
    return MVM_RESULT_RUNTIME_ERROR;
  *result = condition->control;
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_atomic_control(mvm_t *mvm, mvm_value_t value,
                                       mvm_sync_control_t **result) {
  mvm_atomic_t *atomic = mvm_checked_atomic(mvm, value);
  if (!atomic || !atomic->control || atomic->control->kind != MVM_SYNC_ATOMIC)
    return MVM_RESULT_RUNTIME_ERROR;
  *result = atomic->control;
  return MVM_RESULT_OK;
}

static bool mvm_sync_waiter_notify(mvm_sync_waiter_t *waiter) {
  mvm_platform_lock(&waiter->mutex);
  const bool pending = !waiter->notified;
  waiter->notified = true;
  if (pending)
    mvm_platform_signal(&waiter->cond);
  mvm_platform_unlock(&waiter->mutex);
  return pending;
}

static void mvm_sync_control_notify(mvm_sync_control_t *control, bool all) {
  for (mvm_sync_waiter_t *waiter = control->waiters; waiter;
       waiter = waiter->next) {
    const bool pending = mvm_sync_waiter_notify(waiter);
    if (!all && pending)
      break;
  }
}

void mvm_sync_interrupt_execution(mvm_execution_t *execution) {
  if (execution && execution->waiting)
    mvm_sync_waiter_notify(execution->waiting);
}

static bool mvm_sync_shutting_down(mvm_t *mvm) {
  if (!mvm || !mvm->runtime)
    return true;
  mvm_runtime_lock(mvm);
  const bool shutting_down = mvm->shutting_down;
  mvm_runtime_unlock(mvm);
  return shutting_down;
}

bool mvm_sync_interrupted(mvm_t *mvm) {
  return mvm_thread_execution_interrupted(mvm_current_execution(mvm)) ||
         mvm_sync_shutting_down(mvm);
}

static bool mvm_sync_waiter_init(mvm_sync_waiter_t *waiter) {
  *waiter = (mvm_sync_waiter_t){0};
  if (!mvm_platform_mutex_init(&waiter->mutex))
    return false;
  if (!mvm_platform_cond_init(&waiter->cond)) {
    mvm_platform_mutex_destroy(&waiter->mutex);
    return false;
  }
  return true;
}

static void mvm_sync_waiter_destroy(mvm_sync_waiter_t *waiter) {
  mvm_platform_cond_destroy(&waiter->cond);
  mvm_platform_mutex_destroy(&waiter->mutex);
}

static void mvm_sync_waiter_attach(mvm_t *mvm, mvm_sync_control_t *control,
                                   mvm_sync_waiter_t *waiter) {
  mvm_execution_t *execution = mvm_current_execution(mvm);
  waiter->next = control->waiters;
  control->waiters = waiter;
  mvm_runtime_lock(mvm);
  if (execution)
    execution->waiting = waiter;
  else
    mvm->waiting = waiter;
  if (mvm->shutting_down || mvm_thread_execution_interrupted(execution))
    mvm_sync_waiter_notify(waiter);
  mvm_runtime_unlock(mvm);
}

static mvm_result_t mvm_sync_waiter_wait(mvm_t *mvm,
                                         mvm_sync_waiter_t *waiter) {
  mvm_execution_t *execution = mvm_current_execution(mvm);
  const bool active = execution ? execution->execution_depth > 0
                                : mvm->execution_depth > 0;
  if (active)
    mvm_execution_leave(mvm);
  mvm_result_t status = MVM_RESULT_OK;
  mvm_platform_lock(&waiter->mutex);
  while (!waiter->notified) {
    if (!mvm_platform_wait(&waiter->cond, &waiter->mutex)) {
      status = MVM_RESULT_VM_ERROR;
      break;
    }
  }
  mvm_platform_unlock(&waiter->mutex);
  if (active)
    mvm_execution_enter(mvm);
  return status;
}

static void mvm_sync_waiter_detach(mvm_t *mvm, mvm_sync_control_t *control,
                                   mvm_sync_waiter_t *waiter) {
  mvm_execution_t *execution = mvm_current_execution(mvm);
  mvm_runtime_lock(mvm);
  if (execution) {
    if (execution->waiting == waiter)
      execution->waiting = NULL;
  } else if (mvm->waiting == waiter) {
    mvm->waiting = NULL;
  }
  mvm_runtime_unlock(mvm);
  for (mvm_sync_waiter_t **current = &control->waiters; *current;
       current = &(*current)->next) {
    if (*current == waiter) {
      *current = waiter->next;
      break;
    }
  }
}

static bool mvm_sync_mutex_owned_by(const mvm_sync_control_t *control,
                                    mvm_execution_t *execution) {
  if (!control->as.mutex.locked || !control->as.mutex.owner_held)
    return false;
  if (execution)
    return control->as.mutex.owner == execution;
  return !control->as.mutex.owner &&
         mvm_platform_thread_equal(control->as.mutex.owner_id,
                                   mvm_platform_thread_id());
}

static void mvm_sync_mutex_acquire_unlocked(mvm_sync_control_t *control,
                                            mvm_execution_t *execution) {
  control->as.mutex.locked = true;
  control->as.mutex.owner_held = true;
  control->as.mutex.owner = execution;
  control->as.mutex.owner_id = mvm_platform_thread_id();
}

static mvm_result_t mvm_sync_mutex_acquire(mvm_t *mvm,
                                           mvm_sync_control_t *control,
                                           bool interruptible) {
  mvm_execution_t *execution = mvm_current_execution(mvm);
  for (;;) {
    mvm_platform_lock(&control->mutex);
    if (mvm_sync_mutex_owned_by(control, execution)) {
      mvm_platform_unlock(&control->mutex);
      return MVM_RESULT_RUNTIME_ERROR;
    }
    if (!control->as.mutex.locked) {
      mvm_sync_mutex_acquire_unlocked(control, execution);
      mvm_platform_unlock(&control->mutex);
      return MVM_RESULT_OK;
    }
    if (mvm_sync_shutting_down(mvm) ||
        (interruptible && mvm_thread_execution_interrupted(execution))) {
      mvm_platform_unlock(&control->mutex);
      return MVM_RESULT_INTERRUPT;
    }
    mvm_sync_waiter_t waiter;
    if (!mvm_sync_waiter_init(&waiter)) {
      mvm_platform_unlock(&control->mutex);
      return MVM_RESULT_VM_ERROR;
    }
    mvm_sync_waiter_attach(mvm, control, &waiter);
    mvm_platform_unlock(&control->mutex);
    const mvm_result_t status = mvm_sync_waiter_wait(mvm, &waiter);
    mvm_platform_lock(&control->mutex);
    mvm_sync_waiter_detach(mvm, control, &waiter);
    mvm_platform_unlock(&control->mutex);
    mvm_sync_waiter_destroy(&waiter);
    if (status != MVM_RESULT_OK)
      return status;
  }
}

mvm_result_t mvm_mutex_new(mvm_t *mvm, mvm_value_t *result) {
  return mvm_sync_handle_new(mvm, MVM_SYNC_MUTEX, result);
}

mvm_result_t mvm_mutex_lock(mvm_t *mvm, mvm_value_t mutex) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  mvm_sync_control_t *control;
  const mvm_result_t validation = mvm_mutex_control(mvm, mutex, &control);
  if (validation != MVM_RESULT_OK)
    return validation;
  mvm_sync_control_retain(control);
  const mvm_result_t status = mvm_sync_mutex_acquire(mvm, control, true);
  if (status != MVM_RESULT_OK)
    mvm_sync_control_release(control);
  return status;
}

mvm_result_t mvm_mutex_try_lock(mvm_t *mvm, mvm_value_t mutex, bool *acquired) {
  if (!mvm || !acquired)
    return MVM_RESULT_VM_ERROR;
  mvm_sync_control_t *control;
  const mvm_result_t validation = mvm_mutex_control(mvm, mutex, &control);
  if (validation != MVM_RESULT_OK)
    return validation;
  mvm_execution_t *execution = mvm_current_execution(mvm);
  mvm_sync_control_retain(control);
  mvm_platform_lock(&control->mutex);
  mvm_result_t status = MVM_RESULT_OK;
  bool locked = false;
  if (mvm_sync_mutex_owned_by(control, execution)) {
    status = MVM_RESULT_RUNTIME_ERROR;
  } else if (!control->as.mutex.locked) {
    mvm_sync_mutex_acquire_unlocked(control, execution);
    locked = true;
  }
  mvm_platform_unlock(&control->mutex);
  if (status == MVM_RESULT_OK) {
    *acquired = locked;
    if (!locked)
      mvm_sync_control_release(control);
  } else {
    mvm_sync_control_release(control);
  }
  return status;
}

mvm_result_t mvm_mutex_unlock(mvm_t *mvm, mvm_value_t mutex) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  mvm_sync_control_t *control;
  const mvm_result_t validation = mvm_mutex_control(mvm, mutex, &control);
  if (validation != MVM_RESULT_OK)
    return validation;
  mvm_sync_control_retain(control);
  mvm_execution_t *execution = mvm_current_execution(mvm);
  mvm_platform_lock(&control->mutex);
  if (!mvm_sync_mutex_owned_by(control, execution)) {
    mvm_platform_unlock(&control->mutex);
    mvm_sync_control_release(control);
    return MVM_RESULT_RUNTIME_ERROR;
  }
  control->as.mutex.locked = false;
  control->as.mutex.owner_held = false;
  control->as.mutex.owner = NULL;
  mvm_sync_control_notify(control, true);
  mvm_platform_unlock(&control->mutex);
  mvm_sync_control_release(control);
  mvm_sync_control_release(control);
  return MVM_RESULT_OK;
}

mvm_result_t mvm_condition_new(mvm_t *mvm, mvm_value_t *result) {
  return mvm_sync_handle_new(mvm, MVM_SYNC_CONDITION, result);
}

mvm_result_t mvm_condition_wait(mvm_t *mvm, mvm_value_t condition,
                                mvm_value_t mutex) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  mvm_sync_control_t *condition_control;
  mvm_sync_control_t *mutex_control;
  mvm_result_t status = mvm_condition_control(mvm, condition, &condition_control);
  if (status != MVM_RESULT_OK)
    return status;
  status = mvm_mutex_control(mvm, mutex, &mutex_control);
  if (status != MVM_RESULT_OK)
    return status;
  mvm_execution_t *execution = mvm_current_execution(mvm);
  mvm_sync_control_retain(condition_control);
  mvm_sync_control_retain(mutex_control);

  mvm_sync_waiter_t waiter;
  if (!mvm_sync_waiter_init(&waiter)) {
    status = MVM_RESULT_VM_ERROR;
    goto release_controls;
  }
  mvm_platform_lock(&condition_control->mutex);
  mvm_platform_lock(&mutex_control->mutex);
  if (!mvm_sync_mutex_owned_by(mutex_control, execution)) {
    mvm_platform_unlock(&mutex_control->mutex);
    mvm_platform_unlock(&condition_control->mutex);
    mvm_sync_waiter_destroy(&waiter);
    status = MVM_RESULT_RUNTIME_ERROR;
    goto release_controls;
  }
  mvm_sync_waiter_attach(mvm, condition_control, &waiter);
  mutex_control->as.mutex.locked = false;
  mutex_control->as.mutex.owner_held = false;
  mutex_control->as.mutex.owner = NULL;
  mvm_sync_control_notify(mutex_control, true);
  mvm_platform_unlock(&mutex_control->mutex);
  mvm_platform_unlock(&condition_control->mutex);
  mvm_sync_control_release(mutex_control);

  const mvm_result_t wait_status = mvm_sync_waiter_wait(mvm, &waiter);
  const bool interrupted = mvm_sync_interrupted(mvm);
  mvm_platform_lock(&condition_control->mutex);
  mvm_sync_waiter_detach(mvm, condition_control, &waiter);
  mvm_platform_unlock(&condition_control->mutex);
  mvm_sync_waiter_destroy(&waiter);

  status = mvm_sync_mutex_acquire(mvm, mutex_control, false);
  if (status != MVM_RESULT_OK)
    mvm_sync_control_release(mutex_control);
  if (wait_status != MVM_RESULT_OK)
    status = wait_status;
  else if (status == MVM_RESULT_OK && interrupted)
    status = MVM_RESULT_INTERRUPT;
  mvm_sync_control_release(condition_control);
  return status;

release_controls:
  mvm_sync_control_release(mutex_control);
  mvm_sync_control_release(condition_control);
  return status;
}

static mvm_result_t mvm_condition_notify(mvm_t *mvm, mvm_value_t condition,
                                         bool all) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  mvm_sync_control_t *control;
  const mvm_result_t validation =
      mvm_condition_control(mvm, condition, &control);
  if (validation != MVM_RESULT_OK)
    return validation;
  mvm_sync_control_retain(control);
  mvm_platform_lock(&control->mutex);
  mvm_sync_control_notify(control, all);
  mvm_platform_unlock(&control->mutex);
  mvm_sync_control_release(control);
  return MVM_RESULT_OK;
}

mvm_result_t mvm_condition_signal(mvm_t *mvm, mvm_value_t condition) {
  return mvm_condition_notify(mvm, condition, false);
}

mvm_result_t mvm_condition_broadcast(mvm_t *mvm, mvm_value_t condition) {
  return mvm_condition_notify(mvm, condition, true);
}

static bool mvm_atomic_kind_valid(mvm_kind_t kind) {
  return kind == MVM_KIND_BOOL ||
         (kind >= MVM_KIND_SBYTE && kind <= MVM_KIND_SLONG);
}

static size_t mvm_atomic_width(mvm_kind_t kind) {
  switch (kind) {
  case MVM_KIND_SBYTE:
    return 1;
  case MVM_KIND_SHALF:
    return 2;
  case MVM_KIND_SWORD:
    return 4;
  case MVM_KIND_SLONG:
    return 8;
  default:
    return 0;
  }
}

static uint64_t mvm_atomic_mask(mvm_kind_t kind) {
  const size_t width = mvm_atomic_width(kind);
  return !width || width == 8 ? UINT64_MAX : (UINT64_C(1) << (width * 8)) - 1;
}

static uint64_t mvm_atomic_bits(mvm_value_t value) {
  if (value.kind == MVM_KIND_BOOL)
    return value.as.b;
  const int64_t signed_value = value.kind == MVM_KIND_SBYTE   ? value.as.sb
                               : value.kind == MVM_KIND_SHALF ? value.as.sh
                               : value.kind == MVM_KIND_SWORD ? value.as.sw
                                                              : value.as.sl;
  return (uint64_t)signed_value & mvm_atomic_mask(value.kind);
}

static int64_t mvm_atomic_signed(uint64_t bits, mvm_kind_t kind) {
  const size_t width = mvm_atomic_width(kind);
  const unsigned sign_bit = (unsigned)(width * 8 - 1);
  const uint64_t sign = UINT64_C(1) << sign_bit;
  bits &= mvm_atomic_mask(kind);
  if (!(bits & sign))
    return (int64_t)bits;
  const uint64_t magnitude = ((~bits) & mvm_atomic_mask(kind)) + 1;
  if (magnitude == sign)
    return width == 8 ? INT64_MIN : -(int64_t)sign;
  return -(int64_t)magnitude;
}

static mvm_value_t mvm_atomic_value(mvm_kind_t kind, uint64_t bits) {
  mvm_value_t value = {.kind = kind};
  const int64_t signed_value =
      kind == MVM_KIND_BOOL ? 0 : mvm_atomic_signed(bits, kind);
  switch (kind) {
  case MVM_KIND_BOOL:
    value.as.b = (bits & 1) != 0;
    break;
  case MVM_KIND_SBYTE:
    value.as.sb = (int8_t)signed_value;
    break;
  case MVM_KIND_SHALF:
    value.as.sh = (int16_t)signed_value;
    break;
  case MVM_KIND_SWORD:
    value.as.sw = (int32_t)signed_value;
    break;
  case MVM_KIND_SLONG:
    value.as.sl = signed_value;
    break;
  default:
    break;
  }
  return value;
}

mvm_result_t mvm_atomic_new(mvm_t *mvm, mvm_value_t initial,
                            mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  if (!mvm_atomic_kind_valid(initial.kind))
    return MVM_RESULT_RUNTIME_ERROR;
  mvm_sync_control_t *control = mvm_sync_control_new(mvm, MVM_SYNC_ATOMIC);
  if (!control)
    return MVM_RESULT_VM_ERROR;
  control->as.atomic.kind = initial.kind;
  atomic_init(&control->as.atomic.bits, mvm_atomic_bits(initial));
  mvm_object_t *object =
      mvm_gc_allocate(mvm, MVM_OBJECT_ATOMIC, sizeof(mvm_atomic_t));
  if (!object) {
    mvm_sync_control_abandon(control);
    return MVM_RESULT_VM_ERROR;
  }
  mvm_atomic_t *atomic = (mvm_atomic_t *)object->data;
  atomic->control = control;
  *result = (mvm_value_t){.kind = MVM_KIND_ATOMIC, .as.atomic = atomic};
  return MVM_RESULT_OK;
}

mvm_result_t mvm_atomic_load(mvm_t *mvm, mvm_value_t atomic,
                             mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  mvm_sync_control_t *control;
  const mvm_result_t validation = mvm_atomic_control(mvm, atomic, &control);
  if (validation != MVM_RESULT_OK)
    return validation;
  mvm_sync_control_retain(control);
  const uint64_t bits = atomic_load_explicit(&control->as.atomic.bits,
                                             memory_order_seq_cst);
  *result = mvm_atomic_value(control->as.atomic.kind, bits);
  mvm_sync_control_release(control);
  return MVM_RESULT_OK;
}

mvm_result_t mvm_atomic_store(mvm_t *mvm, mvm_value_t atomic,
                              mvm_value_t value) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  mvm_sync_control_t *control;
  const mvm_result_t validation = mvm_atomic_control(mvm, atomic, &control);
  if (validation != MVM_RESULT_OK)
    return validation;
  if (value.kind != control->as.atomic.kind)
    return MVM_RESULT_RUNTIME_ERROR;
  mvm_sync_control_retain(control);
  atomic_store_explicit(&control->as.atomic.bits, mvm_atomic_bits(value),
                        memory_order_seq_cst);
  mvm_sync_control_release(control);
  return MVM_RESULT_OK;
}

mvm_result_t mvm_atomic_exchange(mvm_t *mvm, mvm_value_t atomic,
                                 mvm_value_t value, mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  mvm_sync_control_t *control;
  const mvm_result_t validation = mvm_atomic_control(mvm, atomic, &control);
  if (validation != MVM_RESULT_OK)
    return validation;
  if (value.kind != control->as.atomic.kind)
    return MVM_RESULT_RUNTIME_ERROR;
  mvm_sync_control_retain(control);
  const uint64_t previous = atomic_exchange_explicit(
      &control->as.atomic.bits, mvm_atomic_bits(value), memory_order_seq_cst);
  *result = mvm_atomic_value(control->as.atomic.kind, previous);
  mvm_sync_control_release(control);
  return MVM_RESULT_OK;
}

mvm_result_t mvm_atomic_compare_exchange(mvm_t *mvm, mvm_value_t atomic,
                                         mvm_value_t expected,
                                         mvm_value_t desired,
                                         mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  mvm_sync_control_t *control;
  const mvm_result_t validation = mvm_atomic_control(mvm, atomic, &control);
  if (validation != MVM_RESULT_OK)
    return validation;
  if (expected.kind != control->as.atomic.kind ||
      desired.kind != control->as.atomic.kind)
    return MVM_RESULT_RUNTIME_ERROR;
  mvm_sync_control_retain(control);
  uint64_t previous = mvm_atomic_bits(expected);
  atomic_compare_exchange_strong_explicit(
      &control->as.atomic.bits, &previous, mvm_atomic_bits(desired),
      memory_order_seq_cst, memory_order_seq_cst);
  *result = mvm_atomic_value(control->as.atomic.kind, previous);
  mvm_sync_control_release(control);
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_atomic_arithmetic(mvm_t *mvm, mvm_value_t atomic,
                                          mvm_value_t value, bool subtract,
                                          mvm_value_t *result) {
  if (!mvm || !result)
    return MVM_RESULT_VM_ERROR;
  mvm_sync_control_t *control;
  const mvm_result_t validation = mvm_atomic_control(mvm, atomic, &control);
  if (validation != MVM_RESULT_OK)
    return validation;
  if (control->as.atomic.kind == MVM_KIND_BOOL ||
      value.kind != control->as.atomic.kind)
    return MVM_RESULT_RUNTIME_ERROR;
  mvm_sync_control_retain(control);
  const uint64_t operand = mvm_atomic_bits(value);
  uint64_t previous;
  if (subtract) {
    previous = atomic_fetch_sub_explicit(&control->as.atomic.bits, operand,
                                         memory_order_seq_cst);
  } else {
    previous = atomic_fetch_add_explicit(&control->as.atomic.bits, operand,
                                         memory_order_seq_cst);
  }
  *result = mvm_atomic_value(control->as.atomic.kind, previous);
  mvm_sync_control_release(control);
  return MVM_RESULT_OK;
}

mvm_result_t mvm_atomic_add(mvm_t *mvm, mvm_value_t atomic, mvm_value_t value,
                            mvm_value_t *result) {
  return mvm_atomic_arithmetic(mvm, atomic, value, false, result);
}

mvm_result_t mvm_atomic_sub(mvm_t *mvm, mvm_value_t atomic, mvm_value_t value,
                            mvm_value_t *result) {
  return mvm_atomic_arithmetic(mvm, atomic, value, true, result);
}

void mvm_sync_shutdown(mvm_t *mvm) {
  if (!mvm || !mvm->runtime)
    return;
  mvm_runtime_lock(mvm);
  mvm_sync_control_t *controls = mvm->sync_controls;
  mvm->sync_controls = NULL;
  for (mvm_sync_control_t *control = controls; control; control = control->next)
    control->linked = false;
  mvm_runtime_unlock(mvm);
  for (mvm_sync_control_t *control = controls; control; control = control->next) {
    mvm_platform_lock(&control->mutex);
    mvm_sync_control_notify(control, true);
    if (control->kind == MVM_SYNC_MUTEX && control->as.mutex.owner_held) {
      control->as.mutex.owner_held = false;
      control->as.mutex.owner = NULL;
    }
    mvm_platform_unlock(&control->mutex);
  }

  for (mvm_sync_control_t *control = controls; control;) {
    mvm_sync_control_t *next = control->next;
    if (control->kind == MVM_SYNC_MUTEX && control->as.mutex.locked)
      mvm_sync_control_release_raw(control);
    mvm_sync_control_release_raw(control);
    control = next;
  }
}

static mvm_result_t mvm_builtin_mutex(mvm_t *mvm, const mvm_value_t *args,
                                      mvm_value_t *result) {
  (void)args;
  return mvm_mutex_new(mvm, result);
}

static mvm_result_t mvm_builtin_condition(mvm_t *mvm, const mvm_value_t *args,
                                          mvm_value_t *result) {
  (void)args;
  return mvm_condition_new(mvm, result);
}

static mvm_result_t mvm_builtin_atomic(mvm_t *mvm, const mvm_value_t *args,
                                       mvm_value_t *result) {
  return mvm_atomic_new(mvm, args[0], result);
}

static mvm_result_t mvm_builtin_void_status(mvm_result_t status,
                                            mvm_value_t *result) {
  if (status == MVM_RESULT_OK)
    *result = (mvm_value_t){.kind = MVM_KIND_VOID};
  return status;
}

static mvm_result_t mvm_builtin_lock(mvm_t *mvm, const mvm_value_t *args,
                                     mvm_value_t *result) {
  return mvm_builtin_void_status(mvm_mutex_lock(mvm, args[0]), result);
}

static mvm_result_t mvm_builtin_try_lock(mvm_t *mvm, const mvm_value_t *args,
                                         mvm_value_t *result) {
  bool acquired;
  const mvm_result_t status = mvm_mutex_try_lock(mvm, args[0], &acquired);
  if (status == MVM_RESULT_OK)
    *result = (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = acquired};
  return status;
}

static mvm_result_t mvm_builtin_unlock(mvm_t *mvm, const mvm_value_t *args,
                                       mvm_value_t *result) {
  return mvm_builtin_void_status(mvm_mutex_unlock(mvm, args[0]), result);
}

static mvm_result_t mvm_builtin_wait(mvm_t *mvm, const mvm_value_t *args,
                                     mvm_value_t *result) {
  return mvm_builtin_void_status(mvm_condition_wait(mvm, args[0], args[1]),
                                 result);
}

static mvm_result_t mvm_builtin_signal(mvm_t *mvm, const mvm_value_t *args,
                                       mvm_value_t *result) {
  return mvm_builtin_void_status(mvm_condition_signal(mvm, args[0]), result);
}

static mvm_result_t mvm_builtin_broadcast(mvm_t *mvm, const mvm_value_t *args,
                                          mvm_value_t *result) {
  return mvm_builtin_void_status(mvm_condition_broadcast(mvm, args[0]), result);
}

static mvm_result_t mvm_builtin_load(mvm_t *mvm, const mvm_value_t *args,
                                     mvm_value_t *result) {
  return mvm_atomic_load(mvm, args[0], result);
}

static mvm_result_t mvm_builtin_store(mvm_t *mvm, const mvm_value_t *args,
                                      mvm_value_t *result) {
  return mvm_builtin_void_status(mvm_atomic_store(mvm, args[0], args[1]),
                                 result);
}

static mvm_result_t mvm_builtin_exchange(mvm_t *mvm, const mvm_value_t *args,
                                         mvm_value_t *result) {
  return mvm_atomic_exchange(mvm, args[0], args[1], result);
}

static mvm_result_t mvm_builtin_compare_exchange(mvm_t *mvm,
                                                 const mvm_value_t *args,
                                                 mvm_value_t *result) {
  return mvm_atomic_compare_exchange(mvm, args[0], args[1], args[2], result);
}

static mvm_result_t mvm_builtin_add(mvm_t *mvm, const mvm_value_t *args,
                                    mvm_value_t *result) {
  return mvm_atomic_add(mvm, args[0], args[1], result);
}

static mvm_result_t mvm_builtin_sub(mvm_t *mvm, const mvm_value_t *args,
                                    mvm_value_t *result) {
  return mvm_atomic_sub(mvm, args[0], args[1], result);
}

static mvm_result_t mvm_builtin_semaphore_count(mvm_t *mvm,
                                                const mvm_value_t *args,
                                                mvm_value_t *result) {
  (void)mvm;
  if (args[0].kind != MVM_KIND_SLONG || args[1].kind != MVM_KIND_BOOL ||
      args[0].as.sl < 0 || (args[1].as.b && args[0].as.sl == INT64_MAX))
    return MVM_RESULT_RUNTIME_ERROR;
  *result = (mvm_value_t){.kind = MVM_KIND_SLONG,
                          .as.sl = args[0].as.sl + (args[1].as.b ? 1 : 0)};
  return MVM_RESULT_OK;
}

mvm_result_t mvm_sync_register(mvm_t *mvm) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  static const char *const names[] = {
      "mutex",          "lock",   "tryLock", "unlock",
      "condition",      "wait",   "signal",  "broadcast",
      "atomic",         "load",   "store",   "exchange",
      "compareExchange", "add",    "sub",     "syncSemaphoreCount",
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

  const size_t initial_count = mvm->native_count;
  mvm_result_t status = mvm_register_builtin(mvm, names[0], mvm_builtin_mutex,
                                             MVM_KIND_MUTEX, 0, NULL, NULL);
  static const mvm_kind_t mutex_arg[] = {MVM_KIND_MUTEX};
  if (status == MVM_RESULT_OK)
    status = mvm_register_builtin(mvm, names[1], mvm_builtin_lock, MVM_KIND_VOID,
                                  1, mutex_arg, NULL);
  if (status == MVM_RESULT_OK)
    status = mvm_register_builtin(mvm, names[2], mvm_builtin_try_lock,
                                  MVM_KIND_BOOL, 1, mutex_arg, NULL);
  if (status == MVM_RESULT_OK)
    status = mvm_register_builtin(mvm, names[3], mvm_builtin_unlock,
                                  MVM_KIND_VOID, 1, mutex_arg, NULL);
  if (status == MVM_RESULT_OK)
    status = mvm_register_builtin(mvm, names[4], mvm_builtin_condition,
                                  MVM_KIND_CONDITION, 0, NULL, NULL);
  static const mvm_kind_t wait_args[] = {MVM_KIND_CONDITION, MVM_KIND_MUTEX};
  static const mvm_kind_t condition_arg[] = {MVM_KIND_CONDITION};
  if (status == MVM_RESULT_OK)
    status = mvm_register_builtin(mvm, names[5], mvm_builtin_wait, MVM_KIND_VOID,
                                  2, wait_args, NULL);
  if (status == MVM_RESULT_OK)
    status = mvm_register_builtin(mvm, names[6], mvm_builtin_signal,
                                  MVM_KIND_VOID, 1, condition_arg, NULL);
  if (status == MVM_RESULT_OK)
    status = mvm_register_builtin(mvm, names[7], mvm_builtin_broadcast,
                                  MVM_KIND_VOID, 1, condition_arg, NULL);
  static mvm_builtin_fn const dynamic_functions[] = {
      mvm_builtin_atomic, mvm_builtin_load,    mvm_builtin_store,
      mvm_builtin_exchange, mvm_builtin_compare_exchange,
      mvm_builtin_add,    mvm_builtin_sub,
  };
  for (size_t i = 0; status == MVM_RESULT_OK && i < 7; ++i)
    status = mvm_register_builtin_dynamic(mvm, names[8 + i],
                                          dynamic_functions[i], NULL);
  static const mvm_kind_t semaphore_args[] = {MVM_KIND_SLONG, MVM_KIND_BOOL};
  if (status == MVM_RESULT_OK)
    status = mvm_register_builtin(mvm, names[15], mvm_builtin_semaphore_count,
                                  MVM_KIND_SLONG, 2, semaphore_args, NULL);
  if (status != MVM_RESULT_OK)
    mvm_native_rewind(mvm, initial_count);
  return status;
}
