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

#include "mvm/ram.h"
#include <string.h>

mvm_ram_t *mvm_ram_create(const size_t size) {
  return mvm_ram_create_managed(NULL, size);
}

mvm_ram_t *mvm_ram_create_managed(mvm_memory_t *memory, size_t size) {
  mvm_ram_t *ram = mvm_memory_calloc(memory, 1, sizeof(mvm_ram_t));
  if (!ram)
    return NULL;
  ram->memory = memory;
  if (!mvm_ram_resize(ram, size)) {
    mvm_memory_free(ram);
    return NULL;
  }
  return ram;
}

bool mvm_ram_resize(mvm_ram_t *ram, size_t size) {
  if (!ram)
    return false;
  if (size == ram->size)
    return true;
  uint8_t *data = mvm_memory_realloc(ram->memory, ram->data, size);
  if (size && !data)
    return false;
  if (size > ram->size)
    memset(data + ram->size, 0, size - ram->size);
  ram->data = data;
  ram->size = size;
  return true;
}

void mvm_ram_destroy(mvm_ram_t *ram) {
  if (!ram)
    return;
  mvm_memory_free(ram->data);
  ram->size = 0;
  mvm_memory_free(ram);
}

static uint64_t mvm_ram_peek_unsigned(mvm_ram_t *ram, size_t offset,
                                      size_t width) {
  if (!ram || !ram->data || offset > ram->size || width > ram->size - offset)
    return 0;

  uint64_t value = 0;
  for (size_t i = 0; i < width; ++i)
    value |= (uint64_t)ram->data[offset + i] << (i * 8);
  return value;
}

static int64_t mvm_ram_peek_signed(mvm_ram_t *ram, size_t offset,
                                   size_t width) {
  uint64_t value = mvm_ram_peek_unsigned(ram, offset, width);
  const uint64_t mask = UINT64_MAX >> ((sizeof(uint64_t) - width) * 8);

  // Decode negative values without out-of-range unsigned-to-signed casts.
  if (value > (mask >> 1))
    return -1 - (int64_t)(mask - value);
  return (int64_t)value;
}

uint8_t mvm_ram_peek_ubyte(mvm_ram_t *ram, const size_t offset) {
  return (uint8_t)mvm_ram_peek_unsigned(ram, offset, sizeof(uint8_t));
}

uint16_t mvm_ram_peek_uhalf(mvm_ram_t *ram, const size_t offset) {
  return (uint16_t)mvm_ram_peek_unsigned(ram, offset, sizeof(uint16_t));
}

uint32_t mvm_ram_peek_uword(mvm_ram_t *ram, const size_t offset) {
  return (uint32_t)mvm_ram_peek_unsigned(ram, offset, sizeof(uint32_t));
}

uint64_t mvm_ram_peek_ulong(mvm_ram_t *ram, const size_t offset) {
  return mvm_ram_peek_unsigned(ram, offset, sizeof(uint64_t));
}

int8_t mvm_ram_peek_sbyte(mvm_ram_t *ram, const size_t offset) {
  return (int8_t)mvm_ram_peek_signed(ram, offset, sizeof(int8_t));
}

int16_t mvm_ram_peek_shalf(mvm_ram_t *ram, const size_t offset) {
  return (int16_t)mvm_ram_peek_signed(ram, offset, sizeof(int16_t));
}

int32_t mvm_ram_peek_sword(mvm_ram_t *ram, const size_t offset) {
  return (int32_t)mvm_ram_peek_signed(ram, offset, sizeof(int32_t));
}

int64_t mvm_ram_peek_slong(mvm_ram_t *ram, const size_t offset) {
  return mvm_ram_peek_signed(ram, offset, sizeof(int64_t));
}

static bool mvm_ram_poke_unsigned(mvm_ram_t *ram, size_t offset, size_t width,
                                  uint64_t value) {
  if (!ram || !ram->data || offset > ram->size || width > ram->size - offset)
    return false;

  for (size_t i = 0; i < width; ++i)
    ram->data[offset + i] = (uint8_t)(value >> (i * 8));
  return true;
}

bool mvm_ram_poke_ubyte(mvm_ram_t *ram, const size_t offset,
                        const uint8_t value) {
  return mvm_ram_poke_unsigned(ram, offset, sizeof(uint8_t), value);
}

bool mvm_ram_poke_uhalf(mvm_ram_t *ram, const size_t offset,
                        const uint16_t value) {
  return mvm_ram_poke_unsigned(ram, offset, sizeof(uint16_t), value);
}

bool mvm_ram_poke_uword(mvm_ram_t *ram, const size_t offset,
                        const uint32_t value) {
  return mvm_ram_poke_unsigned(ram, offset, sizeof(uint32_t), value);
}

bool mvm_ram_poke_ulong(mvm_ram_t *ram, const size_t offset,
                        const uint64_t value) {
  return mvm_ram_poke_unsigned(ram, offset, sizeof(uint64_t), value);
}

// Signed-to-unsigned conversion is defined modulo 2^N, yielding two's
// complement bytes regardless of the host representation.
bool mvm_ram_poke_sbyte(mvm_ram_t *ram, const size_t offset,
                        const int8_t value) {
  return mvm_ram_poke_unsigned(ram, offset, sizeof(int8_t), (uint64_t)value);
}

bool mvm_ram_poke_shalf(mvm_ram_t *ram, const size_t offset,
                        const int16_t value) {
  return mvm_ram_poke_unsigned(ram, offset, sizeof(int16_t), (uint64_t)value);
}

bool mvm_ram_poke_sword(mvm_ram_t *ram, const size_t offset,
                        const int32_t value) {
  return mvm_ram_poke_unsigned(ram, offset, sizeof(int32_t), (uint64_t)value);
}

bool mvm_ram_poke_slong(mvm_ram_t *ram, const size_t offset,
                        const int64_t value) {
  return mvm_ram_poke_unsigned(ram, offset, sizeof(int64_t), (uint64_t)value);
}
