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

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

static void assert_zero_reads(mvm_ram_t *ram, size_t offset) {
  assert(mvm_ram_peek_ubyte(ram, offset) == 0);
  assert(mvm_ram_peek_uhalf(ram, offset) == 0);
  assert(mvm_ram_peek_uword(ram, offset) == 0);
  assert(mvm_ram_peek_ulong(ram, offset) == 0);
  assert(mvm_ram_peek_sbyte(ram, offset) == 0);
  assert(mvm_ram_peek_shalf(ram, offset) == 0);
  assert(mvm_ram_peek_sword(ram, offset) == 0);
  assert(mvm_ram_peek_slong(ram, offset) == 0);
}

static void test_little_endian_and_unaligned_reads(void) {
  const uint8_t bytes[] = {0xFF, 0x78, 0x56, 0x34, 0x12,
                          0xEF, 0xCD, 0xAB, 0x09};
  mvm_ram_t *ram = mvm_ram_create(sizeof(bytes));
  assert(ram);
  memcpy(ram->data, bytes, sizeof(bytes));

  assert(mvm_ram_peek_ubyte(ram, 1) == UINT8_C(0x78));
  assert(mvm_ram_peek_uhalf(ram, 1) == UINT16_C(0x5678));
  assert(mvm_ram_peek_uword(ram, 1) == UINT32_C(0x12345678));
  assert(mvm_ram_peek_ulong(ram, 1) == UINT64_C(0x09ABCDEF12345678));
  assert(mvm_ram_peek_sbyte(ram, 1) == INT8_C(0x78));
  assert(mvm_ram_peek_shalf(ram, 1) == INT16_C(0x5678));
  assert(mvm_ram_peek_sword(ram, 1) == INT32_C(0x12345678));
  assert(mvm_ram_peek_slong(ram, 1) == INT64_C(0x09ABCDEF12345678));
  assert(ram->size == sizeof(bytes));
  assert(memcmp(ram->data, bytes, sizeof(bytes)) == 0);
  mvm_ram_destroy(ram);
}

static void test_limits_and_boundaries(void) {
  mvm_ram_t *ram = mvm_ram_create(8);
  assert(ram);
  memset(ram->data, 0xFF, ram->size);

  // Each read ends exactly at the last allocated byte.
  assert(mvm_ram_peek_ubyte(ram, 7) == UINT8_MAX);
  assert(mvm_ram_peek_uhalf(ram, 6) == UINT16_MAX);
  assert(mvm_ram_peek_uword(ram, 4) == UINT32_MAX);
  assert(mvm_ram_peek_ulong(ram, 0) == UINT64_MAX);
  assert(mvm_ram_peek_sbyte(ram, 7) == -1);
  assert(mvm_ram_peek_shalf(ram, 6) == -1);
  assert(mvm_ram_peek_sword(ram, 4) == -1);
  assert(mvm_ram_peek_slong(ram, 0) == -1);

  // Partial values must fail instead of reading beyond RAM.
  assert(mvm_ram_peek_uhalf(ram, 7) == 0);
  assert(mvm_ram_peek_uword(ram, 5) == 0);
  assert(mvm_ram_peek_ulong(ram, 1) == 0);
  assert(mvm_ram_peek_shalf(ram, 7) == 0);
  assert(mvm_ram_peek_sword(ram, 5) == 0);
  assert(mvm_ram_peek_slong(ram, 1) == 0);
  assert_zero_reads(ram, 8);
  assert_zero_reads(ram, 9);
  assert_zero_reads(ram, SIZE_MAX);
  assert_zero_reads(ram, SIZE_MAX - 1);

  ram->data[0] = 0xFE;
  assert(mvm_ram_peek_sbyte(ram, 0) == -2);
  assert(mvm_ram_peek_shalf(ram, 0) == -2);
  assert(mvm_ram_peek_sword(ram, 0) == -2);
  assert(mvm_ram_peek_slong(ram, 0) == -2);
  ram->data[0] = 0xFF;

  ram->data[7] = 0x7F;
  assert(mvm_ram_peek_sbyte(ram, 7) == INT8_MAX);
  assert(mvm_ram_peek_shalf(ram, 6) == INT16_MAX);
  assert(mvm_ram_peek_sword(ram, 4) == INT32_MAX);
  assert(mvm_ram_peek_slong(ram, 0) == INT64_MAX);

  memset(ram->data, 0, ram->size);
  ram->data[7] = 0x80;
  assert(mvm_ram_peek_sbyte(ram, 7) == INT8_MIN);
  assert(mvm_ram_peek_shalf(ram, 6) == INT16_MIN);
  assert(mvm_ram_peek_sword(ram, 4) == INT32_MIN);
  assert(mvm_ram_peek_slong(ram, 0) == INT64_MIN);

  ram->data[7] = 0;
  assert_zero_reads(ram, 0);
  mvm_ram_destroy(ram);
}

static void test_invalid_ram(void) {
  assert_zero_reads(NULL, 0);
  assert_zero_reads(NULL, SIZE_MAX);

  mvm_ram_t *empty = mvm_ram_create(0);
  assert(empty);
  assert_zero_reads(empty, 0);
  mvm_ram_destroy(empty);

  mvm_ram_t missing_data = {.data = NULL, .size = 8};
  assert_zero_reads(&missing_data, 0);
}

static void assert_failed_pokes(mvm_ram_t *ram, size_t offset) {
  assert(!mvm_ram_poke_ubyte(ram, offset, UINT8_MAX));
  assert(!mvm_ram_poke_uhalf(ram, offset, UINT16_MAX));
  assert(!mvm_ram_poke_uword(ram, offset, UINT32_MAX));
  assert(!mvm_ram_poke_ulong(ram, offset, UINT64_MAX));
  assert(!mvm_ram_poke_sbyte(ram, offset, -1));
  assert(!mvm_ram_poke_shalf(ram, offset, -1));
  assert(!mvm_ram_poke_sword(ram, offset, -1));
  assert(!mvm_ram_poke_slong(ram, offset, -1));
}

static void test_little_endian_and_unaligned_writes(void) {
  const uint8_t expected[] = {0x00, 0x78, 0x56, 0x34, 0x12,
                              0xEF, 0xCD, 0xAB, 0x09};
  mvm_ram_t *ram = mvm_ram_create(sizeof(expected));
  assert(ram);
  memset(ram->data, 0, ram->size);

  assert(mvm_ram_poke_ulong(ram, 1, UINT64_C(0x09ABCDEF12345678)));
  assert(memcmp(ram->data, expected, sizeof(expected)) == 0);

  // Narrow writes only touch their own bytes.
  assert(mvm_ram_poke_ubyte(ram, 1, 0xAA));
  assert(mvm_ram_poke_uhalf(ram, 2, 0xCCBB));
  assert(mvm_ram_poke_uword(ram, 4, UINT32_C(0x11223344)));
  assert(ram->data[0] == 0x00);
  assert(mvm_ram_peek_ulong(ram, 1) == UINT64_C(0x0911223344CCBBAA));
  assert(ram->data[8] == 0x09);

  // Round trips through peek for every width and sign.
  assert(mvm_ram_poke_uhalf(ram, 3, UINT16_C(0x5678)));
  assert(mvm_ram_peek_uhalf(ram, 3) == UINT16_C(0x5678));
  assert(mvm_ram_poke_uword(ram, 3, UINT32_C(0x12345678)));
  assert(mvm_ram_peek_uword(ram, 3) == UINT32_C(0x12345678));
  assert(mvm_ram_poke_sbyte(ram, 1, INT8_MIN));
  assert(mvm_ram_peek_sbyte(ram, 1) == INT8_MIN);
  assert(mvm_ram_poke_shalf(ram, 1, -2));
  assert(mvm_ram_peek_shalf(ram, 1) == -2);
  assert(mvm_ram_poke_sword(ram, 1, INT32_MIN));
  assert(mvm_ram_peek_sword(ram, 1) == INT32_MIN);
  assert(mvm_ram_poke_slong(ram, 1, INT64_MIN));
  assert(mvm_ram_peek_slong(ram, 1) == INT64_MIN);
  assert(mvm_ram_poke_slong(ram, 1, INT64_MAX));
  assert(mvm_ram_peek_slong(ram, 1) == INT64_MAX);
  assert(ram->data[0] == 0x00);
  mvm_ram_destroy(ram);
}

static void test_write_limits_and_boundaries(void) {
  mvm_ram_t *ram = mvm_ram_create(8);
  assert(ram);
  memset(ram->data, 0, ram->size);

  // Each write ends exactly at the last allocated byte.
  assert(mvm_ram_poke_sbyte(ram, 7, -1));
  assert(ram->data[7] == 0xFF);
  assert(mvm_ram_poke_shalf(ram, 6, -1));
  assert(mvm_ram_peek_uhalf(ram, 6) == UINT16_MAX);
  assert(mvm_ram_poke_sword(ram, 4, -1));
  assert(mvm_ram_peek_uword(ram, 4) == UINT32_MAX);
  assert(mvm_ram_poke_slong(ram, 0, -1));
  assert(mvm_ram_peek_ulong(ram, 0) == UINT64_MAX);

  // Partial or out-of-range writes must fail and leave RAM unchanged.
  memset(ram->data, 0, ram->size);
  assert(!mvm_ram_poke_uhalf(ram, 7, UINT16_MAX));
  assert(!mvm_ram_poke_uword(ram, 5, UINT32_MAX));
  assert(!mvm_ram_poke_ulong(ram, 1, UINT64_MAX));
  assert(!mvm_ram_poke_shalf(ram, 7, -1));
  assert(!mvm_ram_poke_sword(ram, 5, -1));
  assert(!mvm_ram_poke_slong(ram, 1, -1));
  assert_failed_pokes(ram, 8);
  assert_failed_pokes(ram, 9);
  assert_failed_pokes(ram, SIZE_MAX);
  assert_failed_pokes(ram, SIZE_MAX - 1);
  assert_zero_reads(ram, 0);
  mvm_ram_destroy(ram);
}

static void test_invalid_ram_writes(void) {
  assert_failed_pokes(NULL, 0);
  assert_failed_pokes(NULL, SIZE_MAX);

  mvm_ram_t *empty = mvm_ram_create(0);
  assert(empty);
  assert_failed_pokes(empty, 0);
  mvm_ram_destroy(empty);

  mvm_ram_t missing_data = {.data = NULL, .size = 8};
  assert_failed_pokes(&missing_data, 0);
}

int main(void) {
  test_little_endian_and_unaligned_reads();
  test_limits_and_boundaries();
  test_invalid_ram();
  test_little_endian_and_unaligned_writes();
  test_write_limits_and_boundaries();
  test_invalid_ram_writes();
  return 0;
}
