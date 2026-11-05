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

#include "vm_fixture.h"
#include "mvm.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdatomic.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static mvm_t *create_vm(const size_t size) {
  mvm_t *vm = test_vm_create(NULL, size, MVM_FRAME_DEFAULT_LIMIT);
  assert(vm && vm->ram && vm->stack);
  return vm;
}

static void emit_u8(uint8_t *program, size_t *pc, const uint8_t value) {
  program[(*pc)++] = value;
}

static void emit_u16(uint8_t *program, size_t *pc, const uint16_t value) {
  for (size_t i = 0; i < sizeof(value); ++i)
    emit_u8(program, pc, (uint8_t)(value >> (i * 8)));
}

static void emit_u32(uint8_t *program, size_t *pc, const uint32_t value) {
  for (size_t i = 0; i < sizeof(value); ++i)
    emit_u8(program, pc, (uint8_t)(value >> (i * 8)));
}

static void emit_u64(uint8_t *program, size_t *pc, const uint64_t value) {
  for (size_t i = 0; i < sizeof(value); ++i)
    emit_u8(program, pc, (uint8_t)(value >> (i * 8)));
}

static void patch_u32(uint8_t *program, const size_t address,
                      const uint32_t value) {
  for (size_t i = 0; i < sizeof(value); ++i)
    program[address + i] = (uint8_t)(value >> (i * 8));
}

static void emit_native(uint8_t *program, size_t *pc, const uint16_t id,
                        const uint8_t argc) {
  emit_u8(program, pc, MVM_OP_NATIVE);
  emit_u16(program, pc, id);
  emit_u8(program, pc, argc);
}

static void emit_double(uint8_t *program, size_t *pc) {
  emit_u8(program, pc, MVM_OP_LOCAL_GET);
  emit_u16(program, pc, 0);
  emit_u8(program, pc, MVM_OP_LIT_SLONG);
  emit_u64(program, pc, 2);
  emit_u8(program, pc, MVM_OP_MUL);
  emit_u8(program, pc, MVM_OP_RETURN);
}

static void run(mvm_t *vm) {
  for (size_t steps = 0; vm->working; ++steps) {
    assert(steps < 10000);
    assert(mvm_update(vm) == MVM_RESULT_OK);
  }
}

static mvm_result_t spawn_long(mvm_t *vm, mvm_value_t entry, int64_t argument,
                               mvm_value_t *result) {
  const mvm_value_t value = {.kind = MVM_KIND_SLONG, .as.sl = argument};
  return mvm_thread_spawn(vm, entry, &value, 1, result);
}

static mvm_result_t join_long(mvm_t *vm, mvm_value_t thread, int64_t *result) {
  if (!result)
    return mvm_thread_join(vm, thread, NULL);
  mvm_value_t value;
  const mvm_result_t status = mvm_thread_join(vm, thread, &value);
  if (status == MVM_RESULT_OK) {
    assert(value.kind == MVM_KIND_SLONG);
    *result = value.as.sl;
  }
  return status;
}

static void test_spawn_join_and_validation(void) {
  uint8_t program[32];
  size_t pc = 0;
  emit_double(program, &pc);
  mvm_t *vm = create_vm(pc);
  memcpy(vm->ram->data, program, pc);

  mvm_value_t thread = {.kind = MVM_KIND_ULONG, .as.ul = 99};
  assert(spawn_long(vm, (mvm_value_t){.kind = MVM_KIND_NULL}, 1,
                          &thread) == MVM_RESULT_RUNTIME_ERROR);
  assert(thread.kind == MVM_KIND_ULONG && thread.as.ul == 99);
  assert(spawn_long(
             vm, (mvm_value_t){.kind = MVM_KIND_FUNC, .as.func = (uint32_t)pc},
             1, &thread) == MVM_RESULT_RUNTIME_ERROR);
  assert(thread.kind == MVM_KIND_ULONG && thread.as.ul == 99);

  const mvm_value_t entry = {.kind = MVM_KIND_FUNC, .as.func = 0};
  assert(spawn_long(vm, entry, 21, &thread) == MVM_RESULT_OK);
  assert(thread.kind == MVM_KIND_THREAD && thread.as.thread);
  int64_t result = 0;
  assert(join_long(vm, thread, &result) == MVM_RESULT_OK);
  assert(result == 42);
  result = 0;
  assert(join_long(vm, thread, &result) == MVM_RESULT_OK);
  assert(result == 42);
  assert(mvm_thread_detach(vm, thread) == MVM_RESULT_OK);
  assert(mvm_thread_detach(vm, thread) == MVM_RESULT_OK);
  result = 0;
  assert(join_long(vm, thread, &result) == MVM_RESULT_OK);
  assert(result == 42);
  assert(mvm_thread_interrupt(vm, thread) == MVM_RESULT_OK);
  assert(join_long(vm, thread, NULL) == MVM_RESULT_VM_ERROR);

  mvm_t *foreign = create_vm(pc);
  memcpy(foreign->ram->data, program, pc);
  result = 99;
  assert(join_long(foreign, thread, &result) ==
         MVM_RESULT_RUNTIME_ERROR);
  assert(result == 99);
  assert(mvm_thread_detach(foreign, thread) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm_thread_interrupt(foreign, thread) == MVM_RESULT_RUNTIME_ERROR);
  mvm_destroy(foreign);

  const mvm_value_t fabricated = {
      .kind = MVM_KIND_THREAD, .as.thread = (mvm_thread_t *)vm};
  assert(mvm_thread_detach(vm, fabricated) == MVM_RESULT_RUNTIME_ERROR);
  mvm_destroy(vm);
}

static void test_handle_gc_and_typed_fields(void) {
  uint8_t program[32];
  size_t pc = 0;
  emit_double(program, &pc);
  mvm_t *vm = create_vm(pc);
  memcpy(vm->ram->data, program, pc);
  mvm_value_t thread;
  assert(spawn_long(vm, (mvm_value_t){.kind = MVM_KIND_FUNC}, 5,
                          &thread) == MVM_RESULT_OK);

  mvm_value_t array;
  assert(mvm_array_new(vm, MVM_KIND_THREAD, 1, &array) == MVM_RESULT_OK);
  assert(mvm_stack_push(vm->stack, array));
  assert(mvm_array_set(vm, array, 0, thread) == MVM_RESULT_OK);
  assert(mvm_gc_collect(vm) == 0);

  const mvm_type_field_t field = {
      .offset = 0, .size = 8, .kind = MVM_KIND_THREAD};
  const mvm_type_reference_t reference = {
      .offset = 0, .kind = MVM_KIND_THREAD};
  const mvm_type_desc_t desc = {
      .size = 8,
      .alignment = 8,
      .field_count = 1,
      .fields = &field,
      .reference_count = 1,
      .references = &reference,
  };
  uint32_t type;
  assert(mvm_type_register(vm, &desc, &type) == MVM_RESULT_OK);
  mvm_value_t object;
  assert(mvm_typed_alloc(vm, type, &object) == MVM_RESULT_OK);
  assert(mvm_stack_push(vm->stack, object));
  assert(mvm_typed_field_set(vm, object, 0, thread) == MVM_RESULT_OK);
  assert(mvm_array_set(vm, array, 0,
                       (mvm_value_t){.kind = MVM_KIND_NULL}) == MVM_RESULT_OK);
  assert(mvm_gc_collect(vm) == 0);

  mvm_value_t loaded;
  assert(mvm_typed_field_get(vm, object, 0, &loaded) == MVM_RESULT_OK);
  assert(loaded.kind == MVM_KIND_THREAD && loaded.as.thread == thread.as.thread);
  int64_t result;
  assert(join_long(vm, thread, &result) == MVM_RESULT_OK);
  assert(result == 10);
  assert(mvm_stack_push(vm->stack, loaded));
  assert(mvm_stack_push(vm->stack, thread));
  vm->ram->data[0] = MVM_OP_EQ;
  vm->pc = 0;
  assert(mvm_update(vm) == MVM_RESULT_OK);
  assert(vm->stack->data[vm->stack->size - 1].kind == MVM_KIND_BOOL);
  assert(vm->stack->data[vm->stack->size - 1].as.b);

  assert(mvm_typed_field_set(vm, object, 0,
                             (mvm_value_t){.kind = MVM_KIND_NULL}) ==
         MVM_RESULT_OK);
  assert(mvm_gc_collect(vm) == 1);
  mvm_stack_clear(vm->stack);
  assert(mvm_gc_collect(vm) == 2);
  assert(!vm->objects && !vm->gc_bytes);
  mvm_destroy(vm);
}

static void test_worker_errors_and_interrupt(void) {
  const uint8_t wrong_result[] = {MVM_OP_POP, MVM_OP_RETURN};
  mvm_t *vm = create_vm(sizeof(wrong_result));
  memcpy(vm->ram->data, wrong_result, sizeof(wrong_result));
  mvm_value_t thread;
  assert(spawn_long(vm, (mvm_value_t){.kind = MVM_KIND_FUNC}, 0,
                          &thread) == MVM_RESULT_OK);
  int64_t result = 99;
  assert(join_long(vm, thread, &result) == MVM_RESULT_RUNTIME_ERROR);
  assert(result == 99);
  assert(join_long(vm, thread, &result) == MVM_RESULT_RUNTIME_ERROR);
  assert(result == 99);
  mvm_destroy(vm);

  const uint8_t infinite[] = {MVM_OP_JUMP, 0, 0, 0, 0};
  vm = create_vm(sizeof(infinite));
  memcpy(vm->ram->data, infinite, sizeof(infinite));
  assert(spawn_long(vm, (mvm_value_t){.kind = MVM_KIND_FUNC}, 0,
                          &thread) == MVM_RESULT_OK);
  assert(mvm_thread_interrupt(vm, thread) == MVM_RESULT_OK);
  assert(join_long(vm, thread, &result) == MVM_RESULT_INTERRUPT);
  assert(result == 99);
  assert(join_long(vm, thread, &result) == MVM_RESULT_INTERRUPT);
  mvm_destroy(vm);
}

static void test_globals_are_shared(void) {
  uint8_t program[32];
  size_t pc = 0;
  emit_u8(program, &pc, MVM_OP_LOCAL_GET);
  emit_u16(program, &pc, 0);
  emit_u8(program, &pc, MVM_OP_GLOBAL_SET);
  emit_u32(program, &pc, 0);
  emit_u8(program, &pc, MVM_OP_GLOBAL_GET);
  emit_u32(program, &pc, 0);
  emit_u8(program, &pc, MVM_OP_RETURN);
  const size_t parent_set = pc;
  emit_u8(program, &pc, MVM_OP_GLOBAL_SET);
  emit_u32(program, &pc, 0);

  mvm_t *vm = create_vm(pc);
  memcpy(vm->ram->data, program, pc);
  assert(mvm_stack_push(vm->stack,
                        (mvm_value_t){.kind = MVM_KIND_SLONG, .as.sl = 99}));
  vm->pc = parent_set;
  assert(mvm_update(vm) == MVM_RESULT_OK);
  assert(vm->global_count == 1 && vm->globals[0].as.sl == 99);

  mvm_value_t thread;
  assert(spawn_long(vm, (mvm_value_t){.kind = MVM_KIND_FUNC}, 7,
                          &thread) == MVM_RESULT_OK);
  int64_t result;
  assert(join_long(vm, thread, &result) == MVM_RESULT_OK);
  assert(result == 7);
  assert(vm->global_count == 1 && vm->globals[0].as.sl == 7);
  mvm_destroy(vm);
}

static void test_parallel_gc(void) {
  enum { worker_count = 8, iterations = 128, ram_size = 32768, text_at = 16000 };
  uint8_t program[ram_size];
  memset(program, 0, sizeof(program));
  memcpy(program + text_at, "worker", sizeof("worker"));
  size_t pc = 0;
  for (size_t i = 0; i < iterations; ++i) {
    emit_u8(program, &pc, MVM_OP_LIT_UWORD);
    emit_u32(program, &pc, 1024);
    emit_u8(program, &pc, MVM_OP_ARRAY_NEW);
    emit_u8(program, &pc, MVM_KIND_SLONG);
    emit_u8(program, &pc, MVM_OP_POP);
    emit_u8(program, &pc, MVM_OP_LIT_STRING);
    emit_u32(program, &pc, text_at);
    emit_u8(program, &pc, MVM_OP_LIT_STRING);
    emit_u32(program, &pc, text_at);
    emit_u8(program, &pc, MVM_OP_CONCAT);
    emit_u8(program, &pc, MVM_OP_POP);
  }
  emit_double(program, &pc);

  mvm_t *vm = create_vm(sizeof(program));
  memcpy(vm->ram->data, program, sizeof(program));
  mvm_value_t threads[worker_count];
  for (size_t i = 0; i < worker_count; ++i)
    threads[i] = (mvm_value_t){.kind = MVM_KIND_NULL};
  mvm_gc_root_t root;
  assert(mvm_gc_root_register(vm, &root, threads, worker_count));
  for (size_t i = 0; i < worker_count; ++i)
    assert(spawn_long(vm, (mvm_value_t){.kind = MVM_KIND_FUNC}, (int64_t)i,
                            &threads[i]) == MVM_RESULT_OK);
  for (size_t i = 0; i < worker_count; ++i) {
    int64_t result;
    assert(join_long(vm, threads[i], &result) == MVM_RESULT_OK);
    assert(result == (int64_t)i * 2);
  }
  mvm_gc_root_unregister(vm, &root);
  mvm_destroy(vm);
}

static void test_typed_arguments_and_gc_result(void) {
  uint8_t program[32];
  size_t pc = 0;
  emit_u8(program, &pc, MVM_OP_LOCAL_GET);
  emit_u16(program, &pc, 1);
  emit_u8(program, &pc, MVM_OP_LOCAL_GET);
  emit_u16(program, &pc, 0);
  emit_u8(program, &pc, MVM_OP_CAST);
  emit_u8(program, &pc, MVM_KIND_DOUBLE);
  emit_u8(program, &pc, MVM_OP_ADD);
  emit_u8(program, &pc, MVM_OP_RETURN);

  mvm_t *vm = create_vm(pc);
  memcpy(vm->ram->data, program, pc);
  const mvm_value_t arguments[] = {
      {.kind = MVM_KIND_SLONG, .as.sl = 2},
      {.kind = MVM_KIND_DOUBLE, .as.d = 40.0},
      {.kind = MVM_KIND_BOOL, .as.b = true},
  };
  mvm_value_t thread;
  assert(mvm_thread_spawn(vm, (mvm_value_t){.kind = MVM_KIND_FUNC}, arguments,
                          3, &thread) == MVM_RESULT_OK);
  mvm_value_t result;
  assert(mvm_thread_join(vm, thread, &result) == MVM_RESULT_OK);
  assert(result.kind == MVM_KIND_DOUBLE && result.as.d == 42.0);
  mvm_destroy(vm);

  const uint8_t identity[] = {MVM_OP_LOCAL_GET, 0, 0, MVM_OP_RETURN};
  vm = create_vm(sizeof(identity));
  memcpy(vm->ram->data, identity, sizeof(identity));
  mvm_value_t array;
  assert(mvm_array_new(vm, MVM_KIND_SLONG, 1, &array) == MVM_RESULT_OK);
  assert(mvm_array_set(vm, array, 0,
                       (mvm_value_t){.kind = MVM_KIND_SLONG, .as.sl = 42}) ==
         MVM_RESULT_OK);
  assert(mvm_stack_push(vm->stack, array));
  assert(mvm_thread_spawn(vm, (mvm_value_t){.kind = MVM_KIND_FUNC}, &array, 1,
                          &thread) == MVM_RESULT_OK);
  assert(mvm_thread_join(vm, thread, &result) == MVM_RESULT_OK);
  assert(result.kind == MVM_KIND_ARRAY && result.as.array == array.as.array);
  mvm_value_t roots[] = {array, thread, result};
  mvm_gc_root_t root;
  assert(mvm_gc_root_register(vm, &root, roots, 3));
  mvm_stack_clear(vm->stack);
  assert(mvm_gc_collect(vm) == 0);
  mvm_value_t element;
  assert(mvm_array_get(vm, result, 0, &element) == MVM_RESULT_OK);
  assert(element.kind == MVM_KIND_SLONG && element.as.sl == 42);
  mvm_gc_root_unregister(vm, &root);
  mvm_destroy(vm);
}

static int64_t native_add_seven(const int64_t value) { return value + 7; }

static void test_native_and_recursive_spawn(void) {
  mvm_t *vm = create_vm(128);
  assert(mvm_thread_register(vm) == MVM_RESULT_OK);
  const size_t native_count = vm->native_count;
  assert(mvm_thread_register(vm) == MVM_RESULT_OK);
  assert(vm->native_count == native_count);
  uint16_t spawn;
  uint16_t join;
  assert(mvm_find_native(vm, "threadSpawn", &spawn));
  assert(mvm_find_native(vm, "threadJoin", &join));

  uint8_t program[128];
  size_t pc = 0;
  emit_u8(program, &pc, MVM_OP_LIT_FUNC);
  const size_t entry_operand = pc;
  emit_u32(program, &pc, 0);
  emit_u8(program, &pc, MVM_OP_LIT_SLONG);
  emit_u64(program, &pc, 21);
  emit_native(program, &pc, spawn, 2);
  emit_native(program, &pc, join, 1);
  emit_u8(program, &pc, MVM_OP_RETURN);
  const uint32_t worker = (uint32_t)pc;
  emit_double(program, &pc);
  patch_u32(program, entry_operand, worker);
  memcpy(vm->ram->data, program, pc);
  run(vm);
  assert(vm->stack->size == 1);
  assert(vm->stack->data[0].kind == MVM_KIND_SLONG);
  assert(vm->stack->data[0].as.sl == 42);
  mvm_destroy(vm);

  vm = create_vm(128);
  assert(mvm_thread_register(vm) == MVM_RESULT_OK);
  assert(mvm_find_native(vm, "threadSpawn", &spawn));
  assert(mvm_find_native(vm, "threadJoin", &join));
  pc = 0;
  emit_u8(program, &pc, MVM_OP_LIT_FUNC);
  const size_t inner_operand = pc;
  emit_u32(program, &pc, 0);
  emit_u8(program, &pc, MVM_OP_LIT_SLONG);
  emit_u64(program, &pc, 20);
  emit_native(program, &pc, spawn, 2);
  emit_native(program, &pc, join, 1);
  emit_u8(program, &pc, MVM_OP_LOCAL_GET);
  emit_u16(program, &pc, 0);
  emit_u8(program, &pc, MVM_OP_ADD);
  emit_u8(program, &pc, MVM_OP_RETURN);
  const uint32_t inner = (uint32_t)pc;
  emit_double(program, &pc);
  patch_u32(program, inner_operand, inner);
  memcpy(vm->ram->data, program, pc);
  mvm_value_t thread;
  assert(spawn_long(vm, (mvm_value_t){.kind = MVM_KIND_FUNC}, 2,
                          &thread) == MVM_RESULT_OK);
  int64_t result;
  assert(join_long(vm, thread, &result) == MVM_RESULT_OK);
  assert(result == 42);
  mvm_destroy(vm);

  const mvm_kind_t slong_arg = MVM_KIND_SLONG;
  uint16_t add_seven;
  vm = create_vm(32);
  assert(mvm_register_native(vm, "addSeven",
                             (void (*)(void))native_add_seven, MVM_KIND_SLONG, 1,
                             &slong_arg, &add_seven) == MVM_RESULT_OK);
  pc = 0;
  emit_u8(program, &pc, MVM_OP_LOCAL_GET);
  emit_u16(program, &pc, 0);
  emit_native(program, &pc, add_seven, 1);
  emit_u8(program, &pc, MVM_OP_RETURN);
  memcpy(vm->ram->data, program, pc);
  assert(spawn_long(vm, (mvm_value_t){.kind = MVM_KIND_FUNC}, 35,
                          &thread) == MVM_RESULT_OK);
  assert(join_long(vm, thread, &result) == MVM_RESULT_OK);
  assert(result == 42);
  mvm_destroy(vm);
}

static atomic_int gate_state;
static atomic_int worker_done;

static void native_gate(void) {
  atomic_store_explicit(&gate_state, 1, memory_order_release);
  while (atomic_load_explicit(&gate_state, memory_order_acquire) != 2)
    ;
}

static void native_done(void) {
  atomic_store_explicit(&worker_done, 1, memory_order_release);
}

static void wait_for(atomic_int *value, const int expected) {
  for (size_t i = 0; atomic_load_explicit(value, memory_order_acquire) != expected;
       ++i) {
    assert(i < 100000000);
  }
}

static void test_detach_and_native_library_lifetime(void) {
  enum { ram_size = 16384 };
  mvm_t *vm = create_vm(ram_size);
  memset(vm->ram->data, 0, vm->ram->size);
  assert(mvm_register_native(vm, "testGate", (void (*)(void))native_gate,
                             MVM_KIND_VOID, 0, NULL, NULL) == MVM_RESULT_OK);
  assert(mvm_register_native(vm, "testDone", (void (*)(void))native_done,
                             MVM_KIND_VOID, 0, NULL, NULL) == MVM_RESULT_OK);
  assert(mvm_load_native_library(vm, MVM_TEST_LIBRARY_PATH) == MVM_RESULT_OK);
  uint16_t gate;
  uint16_t done;
  uint16_t answer;
  assert(mvm_find_native(vm, "testGate", &gate));
  assert(mvm_find_native(vm, "testDone", &done));
  assert(mvm_find_native(vm, "nativeTestAnswer", &answer));

  uint8_t program[64];
  size_t pc = 0;
  emit_native(program, &pc, gate, 0);
  emit_u8(program, &pc, MVM_OP_POP);
  emit_native(program, &pc, answer, 0);
  emit_u8(program, &pc, MVM_OP_POP);
  emit_native(program, &pc, done, 0);
  emit_u8(program, &pc, MVM_OP_POP);
  emit_u8(program, &pc, MVM_OP_LIT_SLONG);
  emit_u64(program, &pc, 42);
  emit_u8(program, &pc, MVM_OP_RETURN);
  memcpy(vm->ram->data, program, pc);

  atomic_store(&gate_state, 0);
  atomic_store(&worker_done, 0);
  mvm_value_t thread;
  assert(spawn_long(vm, (mvm_value_t){.kind = MVM_KIND_FUNC}, 0,
                          &thread) == MVM_RESULT_OK);
  wait_for(&gate_state, 1);
  assert(mvm_thread_detach(vm, thread) == MVM_RESULT_OK);
  assert(mvm_thread_detach(vm, thread) == MVM_RESULT_OK);
  int64_t result;
  assert(join_long(vm, thread, &result) == MVM_RESULT_RUNTIME_ERROR);
  atomic_store_explicit(&gate_state, 2, memory_order_release);
  wait_for(&worker_done, 1);
  mvm_destroy(vm);
}

static void test_dispose_during_native_call(void) {
  mvm_t *vm = create_vm(5);
  assert(mvm_load_native_library(vm, MVM_TEST_LIBRARY_PATH) == MVM_RESULT_OK);
  uint16_t answer;
  assert(mvm_find_native(vm, "nativeTestAnswer", &answer));
  typedef void (*set_hook_fn)(void (*hook)(void));
#if defined(_WIN32)
  HMODULE handle = LoadLibraryA(MVM_TEST_LIBRARY_PATH);
  assert(handle);
  set_hook_fn set_hook =
      (set_hook_fn)GetProcAddress(handle, "mvm_test_set_answer_hook");
#else
  void *handle = dlopen(MVM_TEST_LIBRARY_PATH, RTLD_NOW | RTLD_LOCAL);
  assert(handle);
  set_hook_fn set_hook = (set_hook_fn)dlsym(handle, "mvm_test_set_answer_hook");
#endif
  assert(set_hook);
  set_hook(native_gate);
#if defined(_WIN32)
  assert(FreeLibrary(handle));
#else
  assert(dlclose(handle) == 0);
#endif
  // Only the VM now owns the OS handle; the test's symbol lookup released it.
  uint8_t program[5];
  size_t pc = 0;
  emit_native(program, &pc, answer, 0);
  emit_u8(program, &pc, MVM_OP_RETURN);
  memcpy(vm->ram->data, program, pc);
  atomic_store(&gate_state, 0);
  mvm_value_t thread;
  assert(spawn_long(vm, (mvm_value_t){.kind = MVM_KIND_FUNC}, 0, &thread) ==
         MVM_RESULT_OK);
  wait_for(&gate_state, 1);
  // The release/acquire gate guarantees that the hook has already been read.
  set_hook(NULL);
  assert(mvm_dispose_native_library(vm, MVM_TEST_LIBRARY_PATH) ==
         MVM_RESULT_OK);
  assert(!mvm_find_native(vm, "nativeTestAnswer", &answer));
  atomic_store_explicit(&gate_state, 2, memory_order_release);
  int64_t result;
  assert(join_long(vm, thread, &result) == MVM_RESULT_OK);
  assert(result == 42);
  mvm_destroy(vm);
}

int main(void) {
  test_spawn_join_and_validation();
  test_handle_gc_and_typed_fields();
  test_worker_errors_and_interrupt();
  test_globals_are_shared();
  test_parallel_gc();
  test_typed_arguments_and_gc_result();
  test_native_and_recursive_spawn();
  test_detach_and_native_library_lifetime();
  test_dispose_during_native_call();
  return 0;
}
