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

static mvm_t *create_vm(size_t size) {
  mvm_t *vm = test_vm_create(NULL, size, MVM_FRAME_DEFAULT_LIMIT);
  assert(vm && vm->ram && vm->stack);
  assert(mvm_sync_register(vm) == MVM_RESULT_OK);
  const size_t count = vm->native_count;
  assert(mvm_sync_register(vm) == MVM_RESULT_OK);
  assert(vm->native_count == count);
  return vm;
}

static void emit_u8(uint8_t *program, size_t *pc, uint8_t value) {
  program[(*pc)++] = value;
}

static void emit_u16(uint8_t *program, size_t *pc, uint16_t value) {
  for (size_t i = 0; i < 2; ++i)
    emit_u8(program, pc, (uint8_t)(value >> (i * 8)));
}

static void emit_u32(uint8_t *program, size_t *pc, uint32_t value) {
  for (size_t i = 0; i < 4; ++i)
    emit_u8(program, pc, (uint8_t)(value >> (i * 8)));
}

static void emit_u64(uint8_t *program, size_t *pc, uint64_t value) {
  for (size_t i = 0; i < 8; ++i)
    emit_u8(program, pc, (uint8_t)(value >> (i * 8)));
}

static void patch_u32(uint8_t *program, size_t address, uint32_t value) {
  for (size_t i = 0; i < 4; ++i)
    program[address + i] = (uint8_t)(value >> (i * 8));
}

static uint16_t native_id(mvm_t *vm, const char *name) {
  uint16_t id;
  assert(mvm_find_native(vm, name, &id));
  return id;
}

static void emit_native(uint8_t *program, size_t *pc, uint16_t id,
                        uint8_t argc) {
  emit_u8(program, pc, MVM_OP_NATIVE);
  emit_u16(program, pc, id);
  emit_u8(program, pc, argc);
}

static void emit_local(uint8_t *program, size_t *pc, uint16_t index) {
  emit_u8(program, pc, MVM_OP_LOCAL_GET);
  emit_u16(program, pc, index);
}

static void emit_long(uint8_t *program, size_t *pc, int64_t value) {
  emit_u8(program, pc, MVM_OP_LIT_SLONG);
  emit_u64(program, pc, (uint64_t)value);
}

static void emit_bool(uint8_t *program, size_t *pc, bool value) {
  emit_u8(program, pc, value ? MVM_OP_LIT_TRUE : MVM_OP_LIT_FALSE);
}

static void emit_pop(uint8_t *program, size_t *pc) {
  emit_u8(program, pc, MVM_OP_POP);
}

static void emit_return_zero(uint8_t *program, size_t *pc) {
  emit_long(program, pc, 0);
  emit_u8(program, pc, MVM_OP_RETURN);
}

static mvm_value_t long_value(int64_t value) {
  return (mvm_value_t){.kind = MVM_KIND_SLONG, .as.sl = value};
}

static mvm_value_t bool_value(bool value) {
  return (mvm_value_t){.kind = MVM_KIND_BOOL, .as.b = value};
}

static int64_t signed_out(mvm_value_t value) {
  switch (value.kind) {
  case MVM_KIND_SBYTE: return value.as.sb;
  case MVM_KIND_SHALF: return value.as.sh;
  case MVM_KIND_SWORD: return value.as.sw;
  case MVM_KIND_SLONG: return value.as.sl;
  default: assert(false); return 0;
  }
}

static mvm_result_t spawn(mvm_t *vm, const mvm_value_t *arguments, size_t count,
                          mvm_value_t *thread) {
  return mvm_thread_spawn(vm, (mvm_value_t){.kind = MVM_KIND_FUNC}, arguments,
                          count, thread);
}

static void test_atomic_operations(void) {
  mvm_t *vm = create_vm(0);
  mvm_value_t atomic;
  mvm_value_t out = long_value(99);
  assert(mvm_atomic_new(vm, long_value(1), NULL) == MVM_RESULT_VM_ERROR);
  assert(mvm_atomic_new(vm, (mvm_value_t){.kind = MVM_KIND_ULONG}, &atomic) ==
         MVM_RESULT_RUNTIME_ERROR);
  assert(out.as.sl == 99);

  const mvm_kind_t kinds[] = {MVM_KIND_SBYTE, MVM_KIND_SHALF, MVM_KIND_SWORD,
                              MVM_KIND_SLONG};
  const int64_t maxima[] = {INT8_MAX, INT16_MAX, INT32_MAX, INT64_MAX};
  const int64_t minima[] = {INT8_MIN, INT16_MIN, INT32_MIN, INT64_MIN};
  for (size_t i = 0; i < 4; ++i) {
    mvm_value_t initial = {.kind = kinds[i]};
    if (kinds[i] == MVM_KIND_SBYTE)
      initial.as.sb = INT8_MAX;
    else if (kinds[i] == MVM_KIND_SHALF)
      initial.as.sh = INT16_MAX;
    else if (kinds[i] == MVM_KIND_SWORD)
      initial.as.sw = INT32_MAX;
    else
      initial.as.sl = INT64_MAX;
    assert(mvm_atomic_new(vm, initial, &atomic) == MVM_RESULT_OK);
    mvm_value_t one = {.kind = kinds[i]};
    one.as.sb = 1;
    if (kinds[i] == MVM_KIND_SHALF)
      one.as.sh = 1;
    else if (kinds[i] == MVM_KIND_SWORD)
      one.as.sw = 1;
    else if (kinds[i] == MVM_KIND_SLONG)
      one.as.sl = 1;
    assert(mvm_atomic_add(vm, atomic, one, &out) == MVM_RESULT_OK);
    assert(out.kind == kinds[i]);
    assert(signed_out(out) == maxima[i]);
    assert(mvm_atomic_load(vm, atomic, &out) == MVM_RESULT_OK);
    assert(signed_out(out) == minima[i]);
    assert(mvm_atomic_sub(vm, atomic, one, &out) == MVM_RESULT_OK);
    assert(signed_out(out) == minima[i]);
    assert(mvm_atomic_load(vm, atomic, &out) == MVM_RESULT_OK);
    assert(signed_out(out) == maxima[i]);
    mvm_value_t wrong = long_value(1);
    if (kinds[i] == MVM_KIND_SLONG)
      wrong = bool_value(true);
    out = long_value(77);
    assert(mvm_atomic_store(vm, atomic, wrong) == MVM_RESULT_RUNTIME_ERROR);
    assert(mvm_atomic_add(vm, atomic, wrong, &out) == MVM_RESULT_RUNTIME_ERROR);
    assert(out.kind == MVM_KIND_SLONG && out.as.sl == 77);
    mvm_value_t roots[] = {atomic};
    mvm_gc_root_t root;
    assert(mvm_gc_root_register(vm, &root, roots, 1));
    assert(mvm_gc_collect(vm) == 0);
    mvm_gc_root_unregister(vm, &root);
    assert(mvm_gc_collect(vm) == 1);
  }

  assert(mvm_atomic_new(vm, bool_value(false), &atomic) == MVM_RESULT_OK);
  assert(mvm_atomic_exchange(vm, atomic, bool_value(true), &out) ==
         MVM_RESULT_OK);
  assert(out.kind == MVM_KIND_BOOL && !out.as.b);
  assert(mvm_atomic_compare_exchange(vm, atomic, bool_value(false),
                                     bool_value(false), &out) == MVM_RESULT_OK);
  assert(out.kind == MVM_KIND_BOOL && out.as.b);
  assert(mvm_atomic_compare_exchange(vm, atomic, bool_value(true),
                                     bool_value(false), &out) == MVM_RESULT_OK);
  assert(out.kind == MVM_KIND_BOOL && out.as.b);
  assert(mvm_atomic_load(vm, atomic, &out) == MVM_RESULT_OK && !out.as.b);
  assert(mvm_atomic_add(vm, atomic, bool_value(true), &out) ==
         MVM_RESULT_RUNTIME_ERROR);
  mvm_value_t root_value = atomic;
  mvm_gc_root_t root;
  assert(mvm_gc_root_register(vm, &root, &root_value, 1));
  mvm_t *foreign = create_vm(0);
  out = long_value(88);
  assert(mvm_atomic_load(foreign, atomic, &out) == MVM_RESULT_RUNTIME_ERROR);
  assert(out.kind == MVM_KIND_SLONG && out.as.sl == 88);
  mvm_value_t fabricated = {.kind = MVM_KIND_ATOMIC, .as.atomic = (void *)vm};
  assert(mvm_atomic_load(vm, fabricated, &out) == MVM_RESULT_RUNTIME_ERROR);
  mvm_destroy(foreign);
  mvm_gc_root_unregister(vm, &root);
  mvm_destroy(vm);
}

static size_t emit_add_worker(uint8_t *program, mvm_t *vm, size_t iterations) {
  const uint16_t lock = native_id(vm, "lock");
  const uint16_t unlock = native_id(vm, "unlock");
  const uint16_t add = native_id(vm, "add");
  size_t pc = 0;
  for (size_t i = 0; i < iterations; ++i) {
    emit_local(program, &pc, 0);
    emit_native(program, &pc, lock, 1);
    emit_pop(program, &pc);
    emit_local(program, &pc, 1);
    emit_long(program, &pc, 1);
    emit_native(program, &pc, add, 2);
    emit_pop(program, &pc);
    emit_local(program, &pc, 0);
    emit_native(program, &pc, unlock, 1);
    emit_pop(program, &pc);
  }
  emit_return_zero(program, &pc);
  return pc;
}

static void test_mutex_and_atomic_stress(void) {
  enum { workers = 8, iterations = 256 };
  uint8_t program[32768];
  mvm_t *vm = create_vm(0);
  const size_t pc = emit_add_worker(program, vm, iterations);
  assert(mvm_ram_resize(vm->ram, pc));
  memcpy(vm->ram->data, program, pc);
  mvm_value_t mutex;
  mvm_value_t atomic;
  assert(mvm_mutex_new(vm, &mutex) == MVM_RESULT_OK);
  assert(mvm_atomic_new(vm, long_value(0), &atomic) == MVM_RESULT_OK);
  bool acquired = false;
  assert(mvm_mutex_try_lock(vm, mutex, &acquired) == MVM_RESULT_OK && acquired);
  acquired = true;
  assert(mvm_mutex_try_lock(vm, mutex, &acquired) == MVM_RESULT_RUNTIME_ERROR);
  assert(acquired);
  assert(mvm_mutex_lock(vm, mutex) == MVM_RESULT_RUNTIME_ERROR);
  assert(mvm_mutex_unlock(vm, mutex) == MVM_RESULT_OK);
  acquired = false;
  assert(mvm_mutex_try_lock(vm, mutex, &acquired) == MVM_RESULT_OK && acquired);
  assert(mvm_mutex_unlock(vm, mutex) == MVM_RESULT_OK);

  mvm_value_t roots[2 + workers];
  roots[0] = mutex;
  roots[1] = atomic;
  for (size_t i = 0; i < workers; ++i)
    roots[2 + i] = (mvm_value_t){.kind = MVM_KIND_NULL};
  mvm_gc_root_t root;
  assert(mvm_gc_root_register(vm, &root, roots, 2 + workers));
  for (size_t i = 0; i < workers; ++i) {
    const mvm_value_t arguments[] = {mutex, atomic};
    assert(spawn(vm, arguments, 2, &roots[2 + i]) == MVM_RESULT_OK);
  }
  for (size_t i = 0; i < workers; ++i) {
    mvm_value_t result;
    assert(mvm_thread_join(vm, roots[2 + i], &result) == MVM_RESULT_OK);
  }
  mvm_value_t out;
  assert(mvm_atomic_load(vm, atomic, &out) == MVM_RESULT_OK);
  assert(out.kind == MVM_KIND_SLONG && out.as.sl == workers * iterations);
  assert(mvm_gc_collect(vm) == 0);
  mvm_gc_root_unregister(vm, &root);
  mvm_destroy(vm);
}

static size_t emit_cas_worker(uint8_t *program, mvm_t *vm) {
  size_t pc = 0;
  emit_local(program, &pc, 0);
  emit_long(program, &pc, 0);
  emit_local(program, &pc, 1);
  emit_native(program, &pc, native_id(vm, "compareExchange"), 3);
  emit_u8(program, &pc, MVM_OP_RETURN);
  return pc;
}

static void test_cas_single_winner(void) {
  uint8_t program[64];
  mvm_t *vm = create_vm(0);
  const size_t pc = emit_cas_worker(program, vm);
  assert(mvm_ram_resize(vm->ram, pc));
  memcpy(vm->ram->data, program, pc);
  mvm_value_t atomic;
  assert(mvm_atomic_new(vm, long_value(0), &atomic) == MVM_RESULT_OK);
  mvm_value_t roots[] = {atomic, {.kind = MVM_KIND_NULL}, {.kind = MVM_KIND_NULL}};
  mvm_gc_root_t root;
  assert(mvm_gc_root_register(vm, &root, roots, 3));
  const mvm_value_t first_args[] = {atomic, long_value(1)};
  const mvm_value_t second_args[] = {atomic, long_value(2)};
  assert(spawn(vm, first_args, 2, &roots[1]) == MVM_RESULT_OK);
  assert(spawn(vm, second_args, 2, &roots[2]) == MVM_RESULT_OK);
  mvm_value_t results[2];
  assert(mvm_thread_join(vm, roots[1], &results[0]) == MVM_RESULT_OK);
  assert(mvm_thread_join(vm, roots[2], &results[1]) == MVM_RESULT_OK);
  assert((results[0].as.sl == 0) != (results[1].as.sl == 0));
  const int64_t winner = results[0].as.sl == 0 ? 1 : 2;
  const int64_t loser_previous = results[0].as.sl == 0 ? results[1].as.sl
                                                       : results[0].as.sl;
  assert(loser_previous == winner);
  mvm_gc_root_unregister(vm, &root);
  mvm_destroy(vm);
}

static size_t emit_lock_worker(uint8_t *program, mvm_t *vm) {
  size_t pc = 0;
  emit_local(program, &pc, 0);
  emit_native(program, &pc, native_id(vm, "lock"), 1);
  emit_pop(program, &pc);
  emit_return_zero(program, &pc);
  return pc;
}

static void test_mutex_interrupt_gc_and_validation(void) {
  uint8_t program[64];
  mvm_t *vm = create_vm(0);
  const size_t pc = emit_lock_worker(program, vm);
  assert(mvm_ram_resize(vm->ram, pc));
  memcpy(vm->ram->data, program, pc);
  mvm_value_t mutex;
  mvm_value_t roots[] = {{.kind = MVM_KIND_NULL}, {.kind = MVM_KIND_NULL}};
  assert(mvm_mutex_new(vm, &roots[0]) == MVM_RESULT_OK);
  mutex = roots[0];
  assert(mvm_mutex_lock(vm, mutex) == MVM_RESULT_OK);
  mvm_gc_root_t root;
  assert(mvm_gc_root_register(vm, &root, roots, 2));
  assert(spawn(vm, &mutex, 1, &roots[1]) == MVM_RESULT_OK);
  assert(mvm_gc_collect(vm) == 0);
  assert(mvm_thread_interrupt(vm, roots[1]) == MVM_RESULT_OK);
  mvm_value_t result = long_value(42);
  assert(mvm_thread_join(vm, roots[1], &result) == MVM_RESULT_INTERRUPT);
  assert(result.as.sl == 42);
  assert(mvm_mutex_unlock(vm, mutex) == MVM_RESULT_OK);
  assert(mvm_mutex_unlock(vm, mutex) == MVM_RESULT_RUNTIME_ERROR);
  mvm_t *foreign = create_vm(0);
  assert(mvm_mutex_lock(foreign, mutex) == MVM_RESULT_RUNTIME_ERROR);
  mvm_value_t fabricated = {.kind = MVM_KIND_MUTEX, .as.mutex = (void *)vm};
  assert(mvm_mutex_lock(vm, fabricated) == MVM_RESULT_RUNTIME_ERROR);
  mvm_destroy(foreign);
  mvm_gc_root_unregister(vm, &root);
  mvm_destroy(vm);
}

static size_t emit_wait_worker(uint8_t *program, mvm_t *vm, bool mark_stage) {
  const uint16_t lock = native_id(vm, "lock");
  const uint16_t unlock = native_id(vm, "unlock");
  const uint16_t wait = native_id(vm, "wait");
  const uint16_t load = native_id(vm, "load");
  const uint16_t store = native_id(vm, "store");
  size_t pc = 0;
  emit_local(program, &pc, 0);
  emit_native(program, &pc, lock, 1);
  emit_pop(program, &pc);
  if (mark_stage) {
    emit_local(program, &pc, 3);
    emit_long(program, &pc, 1);
    emit_native(program, &pc, store, 2);
    emit_pop(program, &pc);
  }
  const size_t loop = pc;
  emit_local(program, &pc, 1);
  emit_native(program, &pc, load, 1);
  emit_u8(program, &pc, MVM_OP_JUMP_IF_TRUE);
  const size_t done_operand = pc;
  emit_u32(program, &pc, 0);
  emit_local(program, &pc, 2);
  emit_local(program, &pc, 0);
  emit_native(program, &pc, wait, 2);
  emit_pop(program, &pc);
  emit_u8(program, &pc, MVM_OP_JUMP);
  const size_t loop_operand = pc;
  emit_u32(program, &pc, (uint32_t)loop);
  const size_t done = pc;
  patch_u32(program, done_operand, (uint32_t)done);
  emit_local(program, &pc, 0);
  emit_native(program, &pc, unlock, 1);
  emit_pop(program, &pc);
  emit_return_zero(program, &pc);
  (void)loop_operand;
  return pc;
}

static void test_conditionProducerConsumer(void) {
  enum { consumers = 4 };
  uint8_t consumer_program[256];
  uint8_t producer_program[128];
  mvm_t *vm = create_vm(0);
  const size_t consumer_size = emit_wait_worker(consumer_program, vm, false);
  size_t producer_size = 0;
  emit_local(producer_program, &producer_size, 0);
  emit_native(producer_program, &producer_size, native_id(vm, "lock"), 1);
  emit_pop(producer_program, &producer_size);
  emit_local(producer_program, &producer_size, 1);
  emit_bool(producer_program, &producer_size, true);
  emit_native(producer_program, &producer_size, native_id(vm, "store"), 2);
  emit_pop(producer_program, &producer_size);
  emit_local(producer_program, &producer_size, 2);
  emit_native(producer_program, &producer_size, native_id(vm, "broadcast"), 1);
  emit_pop(producer_program, &producer_size);
  emit_local(producer_program, &producer_size, 0);
  emit_native(producer_program, &producer_size, native_id(vm, "unlock"), 1);
  emit_pop(producer_program, &producer_size);
  emit_return_zero(producer_program, &producer_size);
  const size_t total = consumer_size + producer_size;
  assert(mvm_ram_resize(vm->ram, total));
  memcpy(vm->ram->data, consumer_program, consumer_size);
  memcpy(vm->ram->data + consumer_size, producer_program, producer_size);

  mvm_value_t mutex;
  mvm_value_t ready;
  mvm_value_t condition;
  assert(mvm_mutex_new(vm, &mutex) == MVM_RESULT_OK);
  assert(mvm_atomic_new(vm, bool_value(false), &ready) == MVM_RESULT_OK);
  assert(mvm_condition_new(vm, &condition) == MVM_RESULT_OK);
  mvm_value_t roots[3 + consumers + 1];
  roots[0] = mutex;
  roots[1] = ready;
  roots[2] = condition;
  for (size_t i = 0; i <= consumers; ++i)
    roots[3 + i] = (mvm_value_t){.kind = MVM_KIND_NULL};
  mvm_gc_root_t root;
  assert(mvm_gc_root_register(vm, &root, roots, 4 + consumers));
  for (size_t i = 0; i < consumers; ++i) {
    const mvm_value_t args[] = {mutex, ready, condition, condition};
    assert(spawn(vm, args, 4, &roots[3 + i]) == MVM_RESULT_OK);
  }
  const mvm_value_t producer_args[] = {mutex, ready, condition};
  assert(mvm_thread_spawn(vm,
                          (mvm_value_t){.kind = MVM_KIND_FUNC,
                                        .as.func = (uint32_t)consumer_size},
                          producer_args, 3, &roots[3 + consumers]) ==
         MVM_RESULT_OK);
  for (size_t i = 0; i <= consumers; ++i) {
    mvm_value_t result;
    assert(mvm_thread_join(vm, roots[3 + i], &result) == MVM_RESULT_OK);
  }
  assert(mvm_condition_signal(vm, condition) == MVM_RESULT_OK);
  assert(mvm_condition_wait(vm, condition, mutex) == MVM_RESULT_RUNTIME_ERROR);
  mvm_gc_root_unregister(vm, &root);
  mvm_destroy(vm);
}

static void test_condition_interrupt_reacquires(void) {
  uint8_t program[256];
  mvm_t *vm = create_vm(0);
  const size_t pc = emit_wait_worker(program, vm, true);
  assert(mvm_ram_resize(vm->ram, pc));
  memcpy(vm->ram->data, program, pc);
  mvm_value_t mutex;
  mvm_value_t ready;
  mvm_value_t condition;
  mvm_value_t stage;
  assert(mvm_mutex_new(vm, &mutex) == MVM_RESULT_OK);
  assert(mvm_atomic_new(vm, bool_value(false), &ready) == MVM_RESULT_OK);
  assert(mvm_condition_new(vm, &condition) == MVM_RESULT_OK);
  assert(mvm_atomic_new(vm, long_value(0), &stage) == MVM_RESULT_OK);
  mvm_value_t roots[] = {mutex, ready, condition, stage, {.kind = MVM_KIND_NULL}};
  mvm_gc_root_t root;
  assert(mvm_gc_root_register(vm, &root, roots, 5));
  const mvm_value_t args[] = {mutex, ready, condition, stage};
  assert(spawn(vm, args, 4, &roots[4]) == MVM_RESULT_OK);
  mvm_value_t out;
  do {
    assert(mvm_atomic_load(vm, stage, &out) == MVM_RESULT_OK);
  } while (out.as.sl == 0);
  assert(mvm_mutex_lock(vm, mutex) == MVM_RESULT_OK);
  assert(mvm_thread_interrupt(vm, roots[4]) == MVM_RESULT_OK);
  assert(mvm_mutex_unlock(vm, mutex) == MVM_RESULT_OK);
  assert(mvm_thread_join(vm, roots[4], &out) == MVM_RESULT_INTERRUPT);
  assert(mvm_mutex_unlock(vm, mutex) == MVM_RESULT_RUNTIME_ERROR);
  mvm_gc_root_unregister(vm, &root);
  mvm_destroy(vm);
}

static void test_handles_in_containers(void) {
  mvm_t *vm = create_vm(0);
  mvm_value_t handles[3];
  assert(mvm_mutex_new(vm, &handles[0]) == MVM_RESULT_OK);
  assert(mvm_condition_new(vm, &handles[1]) == MVM_RESULT_OK);
  assert(mvm_atomic_new(vm, long_value(0), &handles[2]) == MVM_RESULT_OK);
  const mvm_kind_t kinds[] = {MVM_KIND_MUTEX, MVM_KIND_CONDITION, MVM_KIND_ATOMIC};
  mvm_value_t roots[4];
  for (size_t i = 0; i < 3; ++i) {
    assert(mvm_array_new(vm, kinds[i], 1, &roots[i]) == MVM_RESULT_OK);
    assert(mvm_array_set(vm, roots[i], 0, handles[i]) == MVM_RESULT_OK);
  }
  mvm_type_field_t fields[3];
  mvm_type_reference_t references[3];
  for (size_t i = 0; i < 3; ++i) {
    fields[i] = (mvm_type_field_t){.offset = i * 8, .size = 8, .kind = kinds[i]};
    references[i] = (mvm_type_reference_t){.offset = i * 8, .kind = kinds[i]};
  }
  const mvm_type_desc_t desc = {.size = 24,
                                .alignment = 8,
                                .field_count = 3,
                                .fields = fields,
                                .reference_count = 3,
                                .references = references};
  uint32_t type;
  assert(mvm_type_register(vm, &desc, &type) == MVM_RESULT_OK);
  assert(mvm_typed_alloc(vm, type, &roots[3]) == MVM_RESULT_OK);
  for (size_t i = 0; i < 3; ++i)
    assert(mvm_typed_field_set(vm, roots[3], i, handles[i]) == MVM_RESULT_OK);
  mvm_gc_root_t root;
  assert(mvm_gc_root_register(vm, &root, roots, 4));
  assert(mvm_gc_collect(vm) == 0);
  for (size_t i = 0; i < 3; ++i) {
    assert(mvm_array_set(vm, roots[i], 0,
                         (mvm_value_t){.kind = MVM_KIND_NULL}) == MVM_RESULT_OK);
    assert(mvm_typed_field_set(vm, roots[3], i,
                               (mvm_value_t){.kind = MVM_KIND_NULL}) ==
           MVM_RESULT_OK);
  }
  assert(mvm_gc_collect(vm) == 3);
  assert(!vm->sync_controls);
  mvm_gc_root_unregister(vm, &root);
  mvm_destroy(vm);
}

static void test_shutdown_wakes_mutex(void) {
  uint8_t program[64];
  mvm_t *vm = create_vm(0);
  const size_t pc = emit_lock_worker(program, vm);
  assert(mvm_ram_resize(vm->ram, pc));
  memcpy(vm->ram->data, program, pc);
  mvm_value_t mutex;
  mvm_value_t thread;
  assert(mvm_mutex_new(vm, &mutex) == MVM_RESULT_OK);
  assert(mvm_mutex_lock(vm, mutex) == MVM_RESULT_OK);
  assert(spawn(vm, &mutex, 1, &thread) == MVM_RESULT_OK);
  mvm_destroy(vm);
}

static void test_shutdown_wakes_condition(void) {
  uint8_t program[256];
  mvm_t *vm = create_vm(0);
  const size_t pc = emit_wait_worker(program, vm, true);
  assert(mvm_ram_resize(vm->ram, pc));
  memcpy(vm->ram->data, program, pc);
  mvm_value_t mutex;
  mvm_value_t ready;
  mvm_value_t condition;
  mvm_value_t stage;
  assert(mvm_mutex_new(vm, &mutex) == MVM_RESULT_OK);
  assert(mvm_atomic_new(vm, bool_value(false), &ready) == MVM_RESULT_OK);
  assert(mvm_condition_new(vm, &condition) == MVM_RESULT_OK);
  assert(mvm_atomic_new(vm, long_value(0), &stage) == MVM_RESULT_OK);
  const mvm_value_t args[] = {mutex, ready, condition, stage};
  mvm_value_t thread;
  assert(spawn(vm, args, 4, &thread) == MVM_RESULT_OK);
  mvm_value_t out;
  do {
    assert(mvm_atomic_load(vm, stage, &out) == MVM_RESULT_OK);
  } while (out.as.sl == 0);
  mvm_destroy(vm);
}

int main(void) {
  test_atomic_operations();
  test_mutex_and_atomic_stress();
  test_cas_single_winner();
  test_mutex_interrupt_gc_and_validation();
  test_conditionProducerConsumer();
  test_condition_interrupt_reacquires();
  test_handles_in_containers();
  test_shutdown_wakes_mutex();
  test_shutdown_wakes_condition();
  return 0;
}
