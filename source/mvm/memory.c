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

#include <string.h>

// Raw RAM stores scalar payloads only, never host pointers or GC roots.
size_t mvm_ram_kind_width(const mvm_kind_t kind) {
  switch (kind) {
  case MVM_KIND_BOOL:
  case MVM_KIND_UBYTE:
  case MVM_KIND_SBYTE:
    return 1;
  case MVM_KIND_UHALF:
  case MVM_KIND_SHALF:
    return 2;
  case MVM_KIND_UWORD:
  case MVM_KIND_SWORD:
  case MVM_KIND_FLOAT:
    return 4;
  case MVM_KIND_ULONG:
  case MVM_KIND_SLONG:
  case MVM_KIND_DOUBLE:
    return 8;
  default:
    return 0;
  }
}

// The caller validates the complete range before either helper is used.
mvm_result_t mvm_load_ram(mvm_ram_t *ram, const size_t address,
                                 const mvm_kind_t kind, mvm_value_t *value) {
  *value = (mvm_value_t){.kind = kind};
  switch (kind) {
  case MVM_KIND_BOOL: {
    const uint8_t byte = mvm_ram_peek_ubyte(ram, address);
    if (byte > 1)
      return MVM_RESULT_RUNTIME_ERROR;
    value->as.b = byte != 0;
  } break;
  case MVM_KIND_UBYTE:
    value->as.ub = mvm_ram_peek_ubyte(ram, address);
    break;
  case MVM_KIND_UHALF:
    value->as.uh = mvm_ram_peek_uhalf(ram, address);
    break;
  case MVM_KIND_UWORD:
    value->as.uw = mvm_ram_peek_uword(ram, address);
    break;
  case MVM_KIND_ULONG:
    value->as.ul = mvm_ram_peek_ulong(ram, address);
    break;
  case MVM_KIND_SBYTE:
    value->as.sb = mvm_ram_peek_sbyte(ram, address);
    break;
  case MVM_KIND_SHALF:
    value->as.sh = mvm_ram_peek_shalf(ram, address);
    break;
  case MVM_KIND_SWORD:
    value->as.sw = mvm_ram_peek_sword(ram, address);
    break;
  case MVM_KIND_SLONG:
    value->as.sl = mvm_ram_peek_slong(ram, address);
    break;
  case MVM_KIND_FLOAT: {
    const uint32_t bits = mvm_ram_peek_uword(ram, address);
    memcpy(&value->as.f, &bits, sizeof(bits));
  } break;
  case MVM_KIND_DOUBLE: {
    const uint64_t bits = mvm_ram_peek_ulong(ram, address);
    memcpy(&value->as.d, &bits, sizeof(bits));
  } break;
  default:
    return MVM_RESULT_RUNTIME_ERROR;
  }
  return MVM_RESULT_OK;
}

bool mvm_store_ram(mvm_ram_t *ram, const size_t address,
                          const mvm_value_t value) {
  switch (value.kind) {
  case MVM_KIND_BOOL:
    return mvm_ram_poke_ubyte(ram, address, value.as.b ? 1 : 0);
  case MVM_KIND_UBYTE:
    return mvm_ram_poke_ubyte(ram, address, value.as.ub);
  case MVM_KIND_UHALF:
    return mvm_ram_poke_uhalf(ram, address, value.as.uh);
  case MVM_KIND_UWORD:
    return mvm_ram_poke_uword(ram, address, value.as.uw);
  case MVM_KIND_ULONG:
    return mvm_ram_poke_ulong(ram, address, value.as.ul);
  case MVM_KIND_SBYTE:
    return mvm_ram_poke_sbyte(ram, address, value.as.sb);
  case MVM_KIND_SHALF:
    return mvm_ram_poke_shalf(ram, address, value.as.sh);
  case MVM_KIND_SWORD:
    return mvm_ram_poke_sword(ram, address, value.as.sw);
  case MVM_KIND_SLONG:
    return mvm_ram_poke_slong(ram, address, value.as.sl);
  case MVM_KIND_FLOAT: {
    uint32_t bits;
    memcpy(&bits, &value.as.f, sizeof(bits));
    return mvm_ram_poke_uword(ram, address, bits);
  }
  case MVM_KIND_DOUBLE: {
    uint64_t bits;
    memcpy(&bits, &value.as.d, sizeof(bits));
    return mvm_ram_poke_ulong(ram, address, bits);
  }
  default:
    return false;
  }
}
