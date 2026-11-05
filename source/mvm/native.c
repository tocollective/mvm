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

#include <ffi.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

typedef struct mvm_library_handle {
  void *handle;
  char *path;
  atomic_size_t references;
} mvm_library_handle_t;

struct mvm_native {
  char *name;
  void (*function)(void);
  mvm_builtin_fn builtin;
  ffi_cif cif;
  ffi_type **arg_types;
  mvm_kind_t *arg_kinds;
  uint8_t argc;
  bool variadic;
  bool deferred;
  bool unloaded;
  mvm_library_handle_t *library;
  bool dynamic;
  mvm_kind_t result_kind;
  uint32_t result_type_id;
  uint32_t *arg_type_ids;
  bool has_full_signature;
};

struct mvm_native_library {
  mvm_library_handle_t *shared;
  mvm_native_library_t *next;
};

static void *mvm_library_open(const char *path) {
#if defined(_WIN32)
  return (void *)LoadLibraryA(path);
#else
  return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

static void *mvm_library_symbol(void *handle, const char *name) {
#if defined(_WIN32)
  return (void *)GetProcAddress((HMODULE)handle, name);
#else
  return dlsym(handle, name);
#endif
}

static void mvm_library_close(void *handle) {
#if defined(_WIN32)
  FreeLibrary((HMODULE)handle);
#else
  dlclose(handle);
#endif
}

static void mvm_library_release(mvm_library_handle_t *library) {
  if (library && atomic_fetch_sub_explicit(&library->references, 1,
                                           memory_order_acq_rel) == 1) {
    mvm_library_close(library->handle);
    mvm_memory_free(library->path);
    mvm_memory_free(library);
  }
}

static void mvm_native_clear(mvm_native_t *native) {
  if (!native)
    return;
  mvm_memory_free(native->name);
  mvm_memory_free(native->arg_types);
  mvm_memory_free(native->arg_kinds);
  mvm_memory_free(native->arg_type_ids);
}

void mvm_native_rewind(mvm_t *mvm, const size_t count) {
  if (!mvm || count > mvm->native_count)
    return;
  for (size_t i = count; i < mvm->native_count; ++i)
    mvm_native_clear(&mvm->natives[i]);
  mvm->native_count = count;
}

void mvm_native_destroy(mvm_t *mvm) {
  mvm_native_rewind(mvm, 0);
  mvm_memory_free(mvm->natives);
  mvm->natives = NULL;
  while (mvm->native_libraries) {
    mvm_native_library_t *library = mvm->native_libraries;
    mvm->native_libraries = library->next;
    mvm_library_release(library->shared);
    mvm_memory_free(library);
  }
}

static ffi_type *mvm_native_ffi_type(const mvm_kind_t kind) {
  switch (kind) {
  case MVM_KIND_VOID:
    return &ffi_type_void;
  case MVM_KIND_NULL:
  case MVM_KIND_STRING:
  case MVM_KIND_ARRAY:
  case MVM_KIND_REFERENCE:
  case MVM_KIND_THREAD:
  case MVM_KIND_MUTEX:
  case MVM_KIND_CONDITION:
  case MVM_KIND_ATOMIC:
    return &ffi_type_pointer;
  case MVM_KIND_FUNC:
    return &ffi_type_uint32;
  case MVM_KIND_BOOL:
  case MVM_KIND_UBYTE:
    return &ffi_type_uint8;
  case MVM_KIND_UHALF:
    return &ffi_type_uint16;
  case MVM_KIND_UWORD:
    return &ffi_type_uint32;
  case MVM_KIND_ULONG:
    return &ffi_type_uint64;
  case MVM_KIND_SBYTE:
    return &ffi_type_sint8;
  case MVM_KIND_SHALF:
    return &ffi_type_sint16;
  case MVM_KIND_SWORD:
    return &ffi_type_sint32;
  case MVM_KIND_SLONG:
    return &ffi_type_sint64;
  case MVM_KIND_FLOAT:
    return &ffi_type_float;
  case MVM_KIND_DOUBLE:
    return &ffi_type_double;
  default:
    return NULL;
  }
}

static bool mvm_native_signature_valid(const mvm_kind_t result_kind,
                                       const uint8_t argc,
                                       const mvm_kind_t *arg_kinds) {
  if (!mvm_native_ffi_type(result_kind) || (argc && !arg_kinds))
    return false;
  for (size_t i = 0; i < argc; ++i)
    if (arg_kinds[i] == MVM_KIND_VOID || !mvm_native_ffi_type(arg_kinds[i]))
      return false;
  return true;
}

static mvm_result_t mvm_native_register_unlocked(
    mvm_t *mvm, const char *name, void (*function)(void),
    const mvm_builtin_fn builtin, const mvm_kind_t result_kind,
    const uint8_t argc, const mvm_kind_t *arg_kinds, const bool variadic,
    const bool deferred, uint16_t *id) {
  if (!mvm || !name || (!deferred && (!function == !builtin)) ||
      (deferred && (function || builtin)) ||
      !mvm_native_signature_valid(result_kind, argc, arg_kinds) ||
      (variadic && ((!builtin && !deferred) || argc == UINT8_MAX)) ||
      mvm->native_count > UINT16_MAX)
    return MVM_RESULT_VM_ERROR;

  ffi_type *result_type = mvm_native_ffi_type(result_kind);
  mvm_native_t native = {
      .function = function,
      .builtin = builtin,
      .argc = argc,
      .variadic = variadic,
      .deferred = deferred,
      .result_kind = result_kind,
  };
  const size_t name_length = strlen(name);
  native.name = mvm_memory_alloc(mvm->memory, name_length + 1);
  if (!native.name)
    return MVM_RESULT_VM_ERROR;
  memcpy(native.name, name, name_length + 1);

  if (argc > 0) {
    native.arg_types =
        mvm_memory_calloc(mvm->memory, argc, sizeof(*native.arg_types));
    native.arg_kinds =
        mvm_memory_alloc(mvm->memory, argc * sizeof(*native.arg_kinds));
    if (!native.arg_types || !native.arg_kinds) {
      mvm_native_clear(&native);
      return MVM_RESULT_VM_ERROR;
    }
    memcpy(native.arg_kinds, arg_kinds, argc * sizeof(*native.arg_kinds));
  }

  for (size_t i = 0; i < argc; ++i)
    native.arg_types[i] = mvm_native_ffi_type(arg_kinds[i]);

  if (!builtin && !deferred &&
      ffi_prep_cif(&native.cif, FFI_DEFAULT_ABI, argc, result_type,
                   native.arg_types) != FFI_OK) {
    mvm_native_clear(&native);
    return MVM_RESULT_VM_ERROR;
  }

  mvm_native_t *natives =
      mvm_memory_realloc(mvm->memory, mvm->natives,
                         (mvm->native_count + 1) * sizeof(*mvm->natives));
  if (!natives) {
    mvm_native_clear(&native);
    return MVM_RESULT_VM_ERROR;
  }
  mvm->natives = natives;
  mvm->natives[mvm->native_count] = native;
  if (id)
    *id = (uint16_t)mvm->native_count;
  ++mvm->native_count;
  return MVM_RESULT_OK;
}

static mvm_result_t
mvm_native_register(mvm_t *mvm, const char *name, void (*function)(void),
                    const mvm_builtin_fn builtin, const mvm_kind_t result_kind,
                    const uint8_t argc, const mvm_kind_t *arg_kinds,
                    const bool variadic, const bool deferred, uint16_t *id) {
  mvm_runtime_lock(mvm);
  const mvm_result_t status =
      mvm_native_register_unlocked(mvm, name, function, builtin, result_kind,
                                   argc, arg_kinds, variadic, deferred, id);
  mvm_runtime_unlock(mvm);
  return status;
}

mvm_result_t mvm_register_native(mvm_t *mvm, const char *name,
                                 void (*function)(void),
                                 const mvm_kind_t result_kind,
                                 const uint8_t argc,
                                 const mvm_kind_t *arg_kinds, uint16_t *id) {
  return mvm_native_register(mvm, name, function, NULL, result_kind, argc,
                             arg_kinds, false, false, id);
}

mvm_result_t mvm_register_builtin(mvm_t *mvm, const char *name,
                                  const mvm_builtin_fn function,
                                  const mvm_kind_t result_kind,
                                  const uint8_t argc,
                                  const mvm_kind_t *arg_kinds, uint16_t *id) {
  return mvm_native_register(mvm, name, NULL, function, result_kind, argc,
                             arg_kinds, false, false, id);
}

mvm_result_t mvm_register_builtin_variadic(mvm_t *mvm, const char *name,
                                           const mvm_builtin_fn function,
                                           const mvm_kind_t result_kind,
                                           const uint8_t fixed_argc,
                                           const mvm_kind_t *fixed_arg_kinds,
                                           uint16_t *id) {
  return mvm_native_register(mvm, name, NULL, function, result_kind, fixed_argc,
                             fixed_arg_kinds, true, false, id);
}

mvm_result_t mvm_native_defer(mvm_t *mvm, const char *name,
                              const mvm_kind_t result_kind, const uint8_t argc,
                              const mvm_kind_t *arg_kinds, const bool variadic,
                              uint16_t *id) {
  return mvm_native_register(mvm, name, NULL, NULL, result_kind, argc,
                             arg_kinds, variadic, true, id);
}

mvm_result_t mvm_register_builtin_dynamic(mvm_t *mvm, const char *name,
                                          const mvm_builtin_fn function,
                                          uint16_t *id) {
  uint16_t registered;
  mvm_runtime_lock(mvm);
  const mvm_result_t status =
      mvm_native_register_unlocked(mvm, name, NULL, function, MVM_KIND_VOID, 0,
                                   NULL, false, false, &registered);
  if (status == MVM_RESULT_OK)
    mvm->natives[registered].dynamic = true;
  if (status == MVM_RESULT_OK && id)
    *id = registered;
  mvm_runtime_unlock(mvm);
  return status;
}

bool mvm_find_native(const mvm_t *mvm, const char *name, uint16_t *id) {
  if (!mvm || !name || !id)
    return false;
  mvm_runtime_lock((mvm_t *)mvm);
  bool found = false;
  for (size_t i = 0; i < mvm->native_count; ++i) {
    if (!mvm->natives[i].unloaded && strcmp(mvm->natives[i].name, name) == 0) {
      *id = (uint16_t)i;
      found = true;
      break;
    }
  }
  mvm_runtime_unlock((mvm_t *)mvm);
  return found;
}

static mvm_result_t
mvm_native_bind_signature_unlocked(mvm_t *mvm, uint16_t id,
                                   uint32_t result_type_id,
                                   const uint32_t *arg_type_ids) {
  if (!mvm || (size_t)id >= mvm->native_count)
    return MVM_RESULT_VM_ERROR;
  mvm_native_t *native = &mvm->natives[id];
  if (native->dynamic)
    return MVM_RESULT_OK;
  if (native->has_full_signature) {
    if (native->result_type_id != result_type_id)
      return MVM_RESULT_RUNTIME_ERROR;
    for (size_t i = 0; i < native->argc; ++i)
      if (native->arg_type_ids[i] != arg_type_ids[i])
        return MVM_RESULT_RUNTIME_ERROR;
    return MVM_RESULT_OK;
  }
  uint32_t *copy = NULL;
  if (native->argc) {
    if (!arg_type_ids)
      return MVM_RESULT_VM_ERROR;
    copy = mvm_memory_alloc(mvm->memory, native->argc * sizeof(*arg_type_ids));
    if (!copy)
      return MVM_RESULT_VM_ERROR;
    memcpy(copy, arg_type_ids, native->argc * sizeof(*arg_type_ids));
  }
  native->arg_type_ids = copy;
  native->result_type_id = result_type_id;
  native->has_full_signature = true;
  return MVM_RESULT_OK;
}

mvm_result_t mvm_native_bind_signature(mvm_t *mvm, uint16_t id,
                                       uint32_t result_type_id,
                                       const uint32_t *arg_type_ids) {
  mvm_runtime_lock(mvm);
  const mvm_result_t status =
      mvm_native_bind_signature_unlocked(mvm, id, result_type_id, arg_type_ids);
  mvm_runtime_unlock(mvm);
  return status;
}

mvm_result_t mvm_native_clone(mvm_t *destination, const mvm_t *source) {
  if (!destination || !source || destination == source ||
      destination->native_count || destination->native_libraries)
    return MVM_RESULT_VM_ERROR;
  for (size_t i = 0; i < source->native_count; ++i) {
    const mvm_native_t *native = &source->natives[i];
    uint16_t id;
    const mvm_result_t status =
        (native->deferred || native->unloaded)
            ? mvm_native_defer(destination, native->name, native->result_kind,
                               native->argc, native->arg_kinds,
                               native->variadic, &id)
        : !native->builtin
            ? mvm_register_native(destination, native->name, native->function,
                                  native->result_kind, native->argc,
                                  native->arg_kinds, &id)
        : native->variadic
            ? mvm_register_builtin_variadic(
                  destination, native->name, native->builtin,
                  native->result_kind, native->argc, native->arg_kinds, &id)
            : mvm_register_builtin(destination, native->name, native->builtin,
                                   native->result_kind, native->argc,
                                   native->arg_kinds, &id);
    if (status != MVM_RESULT_OK)
      return status;
    if (id != i)
      return MVM_RESULT_VM_ERROR;
    destination->natives[id].unloaded = native->unloaded;
    destination->natives[id].library = native->library;
    if (native->has_full_signature &&
        mvm_native_bind_signature(destination, id, native->result_type_id,
                                  native->arg_type_ids) != MVM_RESULT_OK)
      return MVM_RESULT_VM_ERROR;
  }
  mvm_native_library_t **tail = &destination->native_libraries;
  for (mvm_native_library_t *library = source->native_libraries; library;
       library = library->next) {
    mvm_native_library_t *copy =
        mvm_memory_calloc(destination->memory, 1, sizeof(*copy));
    if (!copy)
      return MVM_RESULT_VM_ERROR;
    copy->shared = library->shared;
    atomic_fetch_add_explicit(&copy->shared->references, 1,
                              memory_order_relaxed);
    *tail = copy;
    tail = &copy->next;
  }
  return MVM_RESULT_OK;
}

bool mvm_native_describe(const mvm_t *mvm, uint16_t id, uint8_t *argc,
                         mvm_kind_t *result_kind, const mvm_kind_t **arg_kinds,
                         bool *variadic, bool *dynamic) {
  if (!mvm || (size_t)id >= mvm->native_count)
    return false;
  const mvm_native_t *native = &mvm->natives[id];
  if (argc)
    *argc = native->argc;
  if (result_kind)
    *result_kind = native->result_kind;
  if (arg_kinds)
    *arg_kinds = native->arg_kinds;
  if (variadic)
    *variadic = native->variadic;
  if (dynamic)
    *dynamic = native->dynamic;
  return true;
}

static mvm_result_t mvm_native_library_error(mvm_t *mvm, void *handle,
                                             const size_t initial_count,
                                             const char *path,
                                             const mvm_result_t status,
                                             const char *message) {
  while (mvm->native_count > initial_count) {
    --mvm->native_count;
    mvm_native_clear(&mvm->natives[mvm->native_count]);
  }
  mvm_library_close(handle);
  fprintf(stderr, "[VM ERROR]: Cannot load native library '%s': %s\n", path,
          message);
  return status;
}

static mvm_result_t mvm_load_native_library_unlocked(mvm_t *mvm,
                                                     const char *path) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  if (!path || !*path) {
    fprintf(stderr, "[VM ERROR]: Native library path is empty\n");
    return MVM_RESULT_RUNTIME_ERROR;
  }

  void *handle = mvm_library_open(path);
  if (!handle) {
    fprintf(stderr, "[VM ERROR]: Cannot open native library '%s'\n", path);
    return MVM_RESULT_RUNTIME_ERROR;
  }

  const size_t initial_count = mvm->native_count;
  void *symbol = mvm_library_symbol(handle, MVM_NATIVE_EXPORTS_SYMBOL);
  if (!symbol)
    return mvm_native_library_error(mvm, handle, initial_count, path,
                                    MVM_RESULT_RUNTIME_ERROR,
                                    "missing " MVM_NATIVE_EXPORTS_SYMBOL);

  const mvm_native_exports_fn get_exports = (mvm_native_exports_fn)symbol;
  const mvm_native_exports_t *exports = get_exports();
  if (!exports)
    return mvm_native_library_error(mvm, handle, initial_count, path,
                                    MVM_RESULT_RUNTIME_ERROR,
                                    "export table is null");
  if (exports->abi_version != MVM_NATIVE_ABI_VERSION)
    return mvm_native_library_error(mvm, handle, initial_count, path,
                                    MVM_RESULT_RUNTIME_ERROR,
                                    "unsupported ABI version");
  if (exports->count > 0 && !exports->functions)
    return mvm_native_library_error(mvm, handle, initial_count, path,
                                    MVM_RESULT_RUNTIME_ERROR,
                                    "function table is null");
  if (exports->count > (size_t)UINT16_MAX + 1 - initial_count)
    return mvm_native_library_error(mvm, handle, initial_count, path,
                                    MVM_RESULT_RUNTIME_ERROR,
                                    "too many functions");

  // Reject invalid descriptors before registration so resource failures below
  // remain distinguishable from a library that the caller can reject safely.
  for (size_t i = 0; i < exports->count; ++i) {
    const mvm_native_export_t *function = &exports->functions[i];
    if (!function->name || !function->function ||
        !mvm_native_signature_valid(function->result_kind, function->argc,
                                    function->arg_kinds))
      return mvm_native_library_error(mvm, handle, initial_count, path,
                                      MVM_RESULT_RUNTIME_ERROR,
                                      "invalid function signature");
  }

  for (size_t i = 0; i < exports->count; ++i) {
    const mvm_native_export_t *function = &exports->functions[i];
    const mvm_result_t result = mvm_native_register_unlocked(
        mvm, function->name, function->function, NULL, function->result_kind,
        function->argc, function->arg_kinds, false, false, NULL);
    if (result != MVM_RESULT_OK)
      return mvm_native_library_error(mvm, handle, initial_count, path, result,
                                      "cannot register function");
  }

  // The shared handle can outlive this instance through clones or active calls.
  mvm_native_library_t *library =
      mvm_memory_calloc(mvm->memory, 1, sizeof(*library));
  mvm_library_handle_t *shared = mvm_memory_calloc(NULL, 1, sizeof(*shared));
  char *saved_path = mvm_memory_alloc(NULL, strlen(path) + 1);
  if (!library || !shared || !saved_path) {
    mvm_memory_free(library);
    mvm_memory_free(shared);
    mvm_memory_free(saved_path);
    return mvm_native_library_error(mvm, handle, initial_count, path,
                                    MVM_RESULT_VM_ERROR, "out of memory");
  }
  memcpy(saved_path, path, strlen(path) + 1);
  shared->handle = handle;
  shared->path = saved_path;
  atomic_init(&shared->references, 1);
  library->shared = shared;
  library->next = mvm->native_libraries;
  mvm->native_libraries = library;
  for (size_t i = initial_count; i < mvm->native_count; ++i)
    mvm->natives[i].library = shared;
  return MVM_RESULT_OK;
}

mvm_result_t mvm_load_native_library(mvm_t *mvm, const char *path) {
  mvm_runtime_lock(mvm);
  const mvm_result_t status = mvm_load_native_library_unlocked(mvm, path);
  mvm_runtime_unlock(mvm);
  return status;
}

mvm_result_t mvm_dispose_native_library(mvm_t *mvm, const char *path) {
  if (!mvm)
    return MVM_RESULT_VM_ERROR;
  if (!path || !*path)
    return MVM_RESULT_RUNTIME_ERROR;
  mvm_runtime_lock(mvm);
  mvm_native_library_t **link = &mvm->native_libraries;
  while (*link && strcmp((*link)->shared->path, path) != 0)
    link = &(*link)->next;
  mvm_native_library_t *library = *link;
  if (!library) {
    mvm_runtime_unlock(mvm);
    return MVM_RESULT_RUNTIME_ERROR;
  }
  *link = library->next;
  for (size_t i = 0; i < mvm->native_count; ++i) {
    mvm_native_t *native = &mvm->natives[i];
    if (native->library == library->shared) {
      native->function = NULL;
      native->library = NULL;
      native->unloaded = true;
    }
  }
  mvm_runtime_unlock(mvm);
  mvm_library_release(library->shared);
  mvm_memory_free(library);
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_call_native(mvm_t *mvm, mvm_native_t *native,
                                    mvm_value_t *args, mvm_value_t *result) {
  if (native->builtin)
    return native->builtin(mvm, args, result);

  void *values[UINT8_MAX];
  void *null_value = NULL;
  for (size_t i = 0; i < native->argc; ++i) {
    switch (native->arg_kinds[i]) {
    case MVM_KIND_NULL:
      values[i] = &null_value;
      break;
    case MVM_KIND_BOOL:
      values[i] = &args[i].as.b;
      break;
    case MVM_KIND_UBYTE:
      values[i] = &args[i].as.ub;
      break;
    case MVM_KIND_UHALF:
      values[i] = &args[i].as.uh;
      break;
    case MVM_KIND_UWORD:
      values[i] = &args[i].as.uw;
      break;
    case MVM_KIND_ULONG:
      values[i] = &args[i].as.ul;
      break;
    case MVM_KIND_SBYTE:
      values[i] = &args[i].as.sb;
      break;
    case MVM_KIND_SHALF:
      values[i] = &args[i].as.sh;
      break;
    case MVM_KIND_SWORD:
      values[i] = &args[i].as.sw;
      break;
    case MVM_KIND_SLONG:
      values[i] = &args[i].as.sl;
      break;
    case MVM_KIND_FLOAT:
      values[i] = &args[i].as.f;
      break;
    case MVM_KIND_DOUBLE:
      values[i] = &args[i].as.d;
      break;
    case MVM_KIND_STRING:
      values[i] = &args[i].as.s;
      break;
    case MVM_KIND_ARRAY:
      values[i] = &args[i].as.array;
      break;
    case MVM_KIND_REFERENCE:
      values[i] = &args[i].as.ptr;
      break;
    case MVM_KIND_FUNC:
      values[i] = &args[i].as.func;
      break;
    case MVM_KIND_THREAD:
      values[i] = &args[i].as.thread;
      break;
    case MVM_KIND_MUTEX:
      values[i] = &args[i].as.mutex;
      break;
    case MVM_KIND_CONDITION:
      values[i] = &args[i].as.condition;
      break;
    case MVM_KIND_ATOMIC:
      values[i] = &args[i].as.atomic;
      break;
    default:
      return MVM_RESULT_VM_ERROR;
    }
  }

  union {
    ffi_arg arg;
    ffi_sarg sarg;
    uint64_t u64;
    int64_t s64;
    float f;
    double d;
    void *pointer;
  } c_result = {0};

  *mvm_execution_native_error(mvm) = MVM_RESULT_OK;
  ffi_call(&native->cif, native->function, &c_result, values);
  if (*mvm_execution_native_error(mvm) != MVM_RESULT_OK)
    return *mvm_execution_native_error(mvm);

  result->kind = native->result_kind;
  switch (native->result_kind) {
  case MVM_KIND_VOID:
  case MVM_KIND_NULL:
    break;
  case MVM_KIND_BOOL:
    result->as.b = c_result.arg != 0;
    break;
  case MVM_KIND_UBYTE:
    result->as.ub = (uint8_t)c_result.arg;
    break;
  case MVM_KIND_UHALF:
    result->as.uh = (uint16_t)c_result.arg;
    break;
  case MVM_KIND_UWORD:
    result->as.uw = (uint32_t)c_result.arg;
    break;
  case MVM_KIND_ULONG:
    result->as.ul = c_result.u64;
    break;
  case MVM_KIND_SBYTE:
    result->as.sb = (int8_t)c_result.sarg;
    break;
  case MVM_KIND_SHALF:
    result->as.sh = (int16_t)c_result.sarg;
    break;
  case MVM_KIND_SWORD:
    result->as.sw = (int32_t)c_result.sarg;
    break;
  case MVM_KIND_SLONG:
    result->as.sl = c_result.s64;
    break;
  case MVM_KIND_FLOAT:
    result->as.f = c_result.f;
    break;
  case MVM_KIND_DOUBLE:
    result->as.d = c_result.d;
    break;
  case MVM_KIND_STRING:
    result->as.s = c_result.pointer;
    break;
  case MVM_KIND_ARRAY:
    result->as.array = c_result.pointer;
    break;
  case MVM_KIND_REFERENCE:
    result->as.ptr = c_result.pointer;
    break;
  case MVM_KIND_FUNC:
    result->as.func = (uint32_t)c_result.arg;
    break;
  case MVM_KIND_THREAD:
    result->as.thread = c_result.pointer;
    break;
  case MVM_KIND_MUTEX:
    result->as.mutex = c_result.pointer;
    break;
  case MVM_KIND_CONDITION:
    result->as.condition = c_result.pointer;
    break;
  case MVM_KIND_ATOMIC:
    result->as.atomic = c_result.pointer;
    break;
  default:
    return MVM_RESULT_VM_ERROR;
  }
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_native_resolve(mvm_t *mvm, const uint16_t id,
                                       mvm_native_t *native) {
  if (!mvm_execution_stack(mvm))
    return MVM_RESULT_VM_ERROR;

  if ((size_t)id >= mvm->native_count)
    return MVM_RESULT_RUNTIME_ERROR;
  *native = mvm->natives[id];
  if (native->unloaded)
    return MVM_RESULT_RUNTIME_ERROR;
  if (native->deferred) {
    bool resolved = false;
    for (size_t i = 0; i < mvm->native_count; ++i) {
      const mvm_native_t *candidate = &mvm->natives[i];
      if (candidate->deferred || candidate->unloaded ||
          strcmp(candidate->name, native->name) != 0)
        continue;
      if (!candidate->dynamic &&
          (candidate->result_kind != native->result_kind ||
           candidate->argc != native->argc ||
           candidate->variadic != native->variadic ||
           (native->argc &&
            memcmp(candidate->arg_kinds, native->arg_kinds,
                   native->argc * sizeof(*native->arg_kinds)) != 0)))
        return MVM_RESULT_RUNTIME_ERROR;
      if (native->has_full_signature &&
          mvm_native_bind_signature_unlocked(
              mvm, (uint16_t)i, native->result_type_id, native->arg_type_ids) !=
              MVM_RESULT_OK)
        return MVM_RESULT_RUNTIME_ERROR;
      *native = *candidate;
      resolved = true;
      break;
    }
    if (!resolved) {
      fprintf(stderr, "[RUNTIME ERROR]: Native function '%s' is unavailable\n",
              native->name);
      return MVM_RESULT_RUNTIME_ERROR;
    }
  }
  if (native->library)
    atomic_fetch_add_explicit(&native->library->references, 1,
                              memory_order_relaxed);
  return MVM_RESULT_OK;
}

static mvm_result_t mvm_execute_native_call(mvm_t *mvm, mvm_native_t *native,
                                            const uint8_t argc) {
  const size_t expected_argc =
      (size_t)native->argc + (native->variadic ? 1 : 0);
  if (!native->dynamic && argc != expected_argc)
    return MVM_RESULT_RUNTIME_ERROR;
  if (mvm_execution_stack(mvm)->size < mvm_frame_base(mvm) + argc)
    return MVM_RESULT_RUNTIME_ERROR;

  const size_t original_size = mvm_execution_stack(mvm)->size;
  const size_t base = original_size - argc;
  if (native->variadic &&
      !mvm_checked_packet(mvm,
                          mvm_execution_stack(mvm)->data[base + native->argc]))
    return MVM_RESULT_RUNTIME_ERROR;
  for (size_t i = 0; i < native->argc; ++i) {
    const mvm_value_t value = mvm_execution_stack(mvm)->data[base + i];
    if (value.kind == MVM_KIND_NULL) {
      if (!native->has_full_signature ||
          !mvm_value_matches_type(mvm, native->arg_type_ids[i], value))
        return MVM_RESULT_RUNTIME_ERROR;
      continue;
    }
    if (value.kind != native->arg_kinds[i])
      return MVM_RESULT_RUNTIME_ERROR;
    if (native->arg_kinds[i] == MVM_KIND_ARRAY &&
        !mvm_checked_array(mvm, value))
      return MVM_RESULT_RUNTIME_ERROR;
    if (native->arg_kinds[i] == MVM_KIND_THREAD &&
        !mvm_checked_thread(mvm, value))
      return MVM_RESULT_RUNTIME_ERROR;
    if (!mvm_sync_value_valid(mvm, value))
      return MVM_RESULT_RUNTIME_ERROR;
    if (native->arg_kinds[i] == MVM_KIND_STRING) {
      size_t bytes;
      size_t characters;
      const mvm_result_t status =
          mvm_measure_string(mvm, value, &bytes, &characters);
      if (status != MVM_RESULT_OK)
        return status;
    }
    if (native->has_full_signature &&
        !mvm_value_matches_type(mvm, native->arg_type_ids[i], value))
      return MVM_RESULT_RUNTIME_ERROR;
  }

  if (!mvm_stack_push(mvm_execution_stack(mvm),
                      (mvm_value_t){.kind = MVM_KIND_VOID}))
    return MVM_RESULT_VM_ERROR;

  mvm_value_t args[UINT8_MAX];
  memcpy(args, &mvm_execution_stack(mvm)->data[base], argc * sizeof(*args));
  mvm_gc_root_t root;
  if (!mvm_gc_root_register(mvm, &root, args, argc)) {
    mvm_stack_truncate(mvm_execution_stack(mvm), original_size);
    return MVM_RESULT_VM_ERROR;
  }
  mvm_value_t result = {0};
  const mvm_result_t status = mvm_call_native(mvm, native, args, &result);
  mvm_gc_root_unregister(mvm, &root);
  if (status != MVM_RESULT_OK) {
    mvm_stack_truncate(mvm_execution_stack(mvm), original_size);
    return status;
  }
  if (native->has_full_signature && result.kind != MVM_KIND_VOID &&
      !mvm_value_matches_type(mvm, native->result_type_id, result)) {
    mvm_stack_truncate(mvm_execution_stack(mvm), original_size);
    return MVM_RESULT_RUNTIME_ERROR;
  }
  mvm_execution_stack(mvm)->data[base] = result;
  mvm_stack_truncate(mvm_execution_stack(mvm), base + 1);
  return MVM_RESULT_OK;
}

mvm_result_t mvm_execute_native(mvm_t *mvm, const uint16_t id,
                                const uint8_t argc) {
  mvm_native_t native;
  mvm_runtime_lock(mvm);
  mvm_result_t status = mvm_native_resolve(mvm, id, &native);
  mvm_runtime_unlock(mvm);
  if (status != MVM_RESULT_OK)
    return status;
  status = mvm_execute_native_call(mvm, &native, argc);
  mvm_library_release(native.library);
  return status;
}
