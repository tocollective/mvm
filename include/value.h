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
#include <stdint.h>

typedef enum mvm_kind {
  MVM_KIND_VOID,      // void
  MVM_KIND_NULL,      // null
  MVM_KIND_BOOL,      // bool
  MVM_KIND_UBYTE,     // unsigned
  MVM_KIND_UHALF,     //
  MVM_KIND_UWORD,     //
  MVM_KIND_ULONG,     //
  MVM_KIND_SBYTE,     // signed
  MVM_KIND_SHALF,     //
  MVM_KIND_SWORD,     //
  MVM_KIND_SLONG,     //
  MVM_KIND_FLOAT,     // float
  MVM_KIND_DOUBLE,    // double
  MVM_KIND_STRING,    // string
  MVM_KIND_ARRAY,     // array
  MVM_KIND_TYPE,      // type
  MVM_KIND_REFERENCE, // reference
  MVM_KIND_FUNC,      // bytecode function (not a host pointer or GC object)
  MVM_KIND_THREAD,    // managed OS thread handle
  MVM_KIND_PACKET,    // internal variadic argument packet, not an M value
  MVM_KIND_UNION,     // immutable managed closed-union value
  MVM_KIND_MUTEX,     // nonrecursive mutual-exclusion handle
  MVM_KIND_CONDITION, // condition-variable handle
  MVM_KIND_ATOMIC,    // fixed-type scalar atomic handle
  MVM_KIND_COUNT,
} mvm_kind_t;

typedef struct mvm_array mvm_array_t;
typedef struct mvm_thread mvm_thread_t;
typedef struct mvm_packet mvm_packet_t;
typedef struct mvm_union mvm_union_t;
typedef struct mvm_mutex mvm_mutex_t;
typedef struct mvm_condition mvm_condition_t;
typedef struct mvm_atomic mvm_atomic_t;

typedef struct mvm_value {
  mvm_kind_t kind;
  union {
    uint8_t ub;  // ubyte
    uint16_t uh; // uhalf
    uint32_t uw; // uword
    uint64_t ul; // ulong
    int8_t sb;   // sbyte
    int16_t sh;  // shalf
    int32_t sw;  // sword
    int64_t sl;  // slong
    float f;     // float
    double d;    // double
    bool b;         // bool
    char *s;        // string
    void *ptr;      // reference
    mvm_array_t *array; // managed array (opaque to the host)
    uint32_t type;   // legacy runtime-kind or canonical full type ID
    uint32_t func;   // absolute bytecode address; zero is a valid address
    mvm_thread_t *thread; // managed thread handle (opaque to the host)
    mvm_packet_t *packet; // managed internal packet (opaque to the host)
    mvm_union_t *union_value;
    mvm_mutex_t *mutex;
    mvm_condition_t *condition;
    mvm_atomic_t *atomic;
  } as;
  uint32_t type_id;
  bool has_full_type;
} mvm_value_t;
