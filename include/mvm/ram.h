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
#include <stdlib.h>
#include "mvm/memory.h"

typedef struct mvm_ram {
  mvm_memory_t *memory;
  uint8_t *data;
  size_t size;
} mvm_ram_t;

mvm_ram_t *mvm_ram_create(const size_t size);
mvm_ram_t *mvm_ram_create_managed(mvm_memory_t *memory, size_t size);
// Resizing is only valid before execution: literal strings point into RAM.
bool mvm_ram_resize(mvm_ram_t *ram, size_t size);
void mvm_ram_destroy(mvm_ram_t *ram);

// Read little-endian integers without modifying RAM. Signed values use two's
// complement. Return zero for null RAM/data or if the entire value does not fit.
uint8_t mvm_ram_peek_ubyte(mvm_ram_t *ram, const size_t offset);
uint16_t mvm_ram_peek_uhalf(mvm_ram_t *ram, const size_t offset);
uint32_t mvm_ram_peek_uword(mvm_ram_t *ram, const size_t offset);
uint64_t mvm_ram_peek_ulong(mvm_ram_t *ram, const size_t offset);

int8_t mvm_ram_peek_sbyte(mvm_ram_t *ram, const size_t offset);
int16_t mvm_ram_peek_shalf(mvm_ram_t *ram, const size_t offset);
int32_t mvm_ram_peek_sword(mvm_ram_t *ram, const size_t offset);
int64_t mvm_ram_peek_slong(mvm_ram_t *ram, const size_t offset);

// Write little-endian integers. Signed values use two's complement. Return
// false for null RAM/data or if the entire value does not fit; failure leaves
// RAM unchanged.
bool mvm_ram_poke_ubyte(mvm_ram_t *ram, const size_t offset, const uint8_t value);
bool mvm_ram_poke_uhalf(mvm_ram_t *ram, const size_t offset, const uint16_t value);
bool mvm_ram_poke_uword(mvm_ram_t *ram, const size_t offset, const uint32_t value);
bool mvm_ram_poke_ulong(mvm_ram_t *ram, const size_t offset, const uint64_t value);

bool mvm_ram_poke_sbyte(mvm_ram_t *ram, const size_t offset, const int8_t value);
bool mvm_ram_poke_shalf(mvm_ram_t *ram, const size_t offset, const int16_t value);
bool mvm_ram_poke_sword(mvm_ram_t *ram, const size_t offset, const int32_t value);
bool mvm_ram_poke_slong(mvm_ram_t *ram, const size_t offset, const int64_t value);
