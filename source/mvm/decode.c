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

#include <stdio.h>
#include <string.h>

// Decode operands without changing pc, the value stack, or call frames.
mvm_result_t mvm_decode(const mvm_t *mvm, mvm_execution_t *execution,
                        const mvm_opcodes_t op, mvm_instruction_t *instruction) {
  *instruction = (mvm_instruction_t){
      .op = op, .next_pc = *mvm_execution_pc_for((mvm_t *)mvm, execution)};

  switch (op) {
  case MVM_OP_RETURN:
  case MVM_OP_BREAKPOINT:
  case MVM_OP_NULL_CHECK:
  case MVM_OP_POP:
  case MVM_OP_DUP:
  case MVM_OP_SWAP:
  case MVM_OP_ADD:
  case MVM_OP_SUB:
  case MVM_OP_MUL:
  case MVM_OP_DIV:
  case MVM_OP_MOD:
  case MVM_OP_NEG:
  case MVM_OP_INC:
  case MVM_OP_DEC:
  case MVM_OP_EQ:
  case MVM_OP_NE:
  case MVM_OP_LT:
  case MVM_OP_LE:
  case MVM_OP_GT:
  case MVM_OP_GE:
  case MVM_OP_NOT:
  case MVM_OP_BAND:
  case MVM_OP_BOR:
  case MVM_OP_BXOR:
  case MVM_OP_BNOT:
  case MVM_OP_SHL:
  case MVM_OP_SHR:
  case MVM_OP_TYPEOF:
  case MVM_OP_UNION_VARIANTOF:
  case MVM_OP_CONCAT:
  case MVM_OP_STRING_LEN:
  case MVM_OP_STRING_GET:
  case MVM_OP_ALLOC:
  case MVM_OP_ARRAY_GET:
  case MVM_OP_ARRAY_SET:
  case MVM_OP_ARRAY_LEN:
  case MVM_OP_ARRAY_CAP:
  case MVM_OP_ARRAY_RESERVE:
  case MVM_OP_ARRAY_RESIZE:
  case MVM_OP_ARRAY_PUSH:
  case MVM_OP_ARRAY_POP:
  case MVM_OP_ARRAY_INSERT:
  case MVM_OP_ARRAY_REMOVE:
  case MVM_OP_VA_FORWARD:
  case MVM_OP_VA_COUNT:
    break;
  case MVM_OP_CAST:
  case MVM_OP_IS:
  case MVM_OP_LIT_TYPE:
  case MVM_OP_LOAD:
  case MVM_OP_STORE:
  case MVM_OP_ARRAY_NEW: {
    if (!mvm_has_operand_for(mvm, execution, sizeof(uint8_t)))
      return MVM_RESULT_RUNTIME_ERROR;

    const uint8_t kind = mvm_ram_peek_ubyte(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution));
    if (kind >= MVM_KIND_COUNT ||
        (op == MVM_OP_ARRAY_NEW && kind == MVM_KIND_VOID))
      return MVM_RESULT_RUNTIME_ERROR;
    if ((op == MVM_OP_LOAD || op == MVM_OP_STORE) &&
        mvm_ram_kind_width((mvm_kind_t)kind) == 0)
      return MVM_RESULT_RUNTIME_ERROR;
    instruction->as.kind = (mvm_kind_t)kind;
    instruction->next_pc += sizeof(kind);
  } break;
  case MVM_OP_CALL:
  case MVM_OP_TAIL_CALL: {
    if (!mvm_has_operand_for(mvm, execution, sizeof(uint32_t) + sizeof(uint8_t)))
      return MVM_RESULT_RUNTIME_ERROR;
    instruction->as.call.address = mvm_ram_peek_uword(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution));
    instruction->as.call.argc =
        mvm_ram_peek_ubyte(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution) + sizeof(uint32_t));
    instruction->next_pc += sizeof(uint32_t) + sizeof(uint8_t);
  } break;
  case MVM_OP_CALL_INDIRECT: {
    if (!mvm_has_operand_for(mvm, execution, sizeof(uint8_t)))
      return MVM_RESULT_RUNTIME_ERROR;
    instruction->as.call.argc = mvm_ram_peek_ubyte(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution));
    instruction->next_pc += sizeof(uint8_t);
  } break;
  case MVM_OP_LIT_FUNC: {
    if (!mvm_has_operand_for(mvm, execution, sizeof(uint32_t)))
      return MVM_RESULT_RUNTIME_ERROR;
    const uint32_t address = mvm_ram_peek_uword(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution));
    if (address >= mvm->ram->size)
      return MVM_RESULT_RUNTIME_ERROR;
    instruction->as.value =
        (mvm_value_t){.kind = MVM_KIND_FUNC, .as.func = address};
    instruction->next_pc += sizeof(uint32_t);
  } break;
  case MVM_OP_NATIVE: {
    if (!mvm_has_operand_for(mvm, execution, sizeof(uint16_t) + sizeof(uint8_t)))
      return MVM_RESULT_RUNTIME_ERROR;

    instruction->as.native.id = mvm_ram_peek_uhalf(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution));
    instruction->as.native.argc =
        mvm_ram_peek_ubyte(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution) + sizeof(uint16_t));
    instruction->next_pc += sizeof(uint16_t) + sizeof(uint8_t);
  } break;
  case MVM_OP_JUMP:
  case MVM_OP_JUMP_IF_FALSE:
  case MVM_OP_JUMP_IF_TRUE: {
    if (!mvm_has_operand_for(mvm, execution, sizeof(uint32_t)))
      return MVM_RESULT_RUNTIME_ERROR;

    instruction->as.jump_address = mvm_ram_peek_uword(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution));
    instruction->next_pc += sizeof(uint32_t);
  } break;
  case MVM_OP_JUMP_IF_LOCAL: {
    const size_t width = sizeof(uint16_t) * 2 + sizeof(uint8_t) * 2 + sizeof(uint32_t);
    if (!mvm_has_operand_for(mvm, execution, width))
      return MVM_RESULT_RUNTIME_ERROR;
    const size_t pc = *mvm_execution_pc_for((mvm_t *)mvm, execution);
    instruction->as.local_jump.left = mvm_ram_peek_uhalf(mvm->ram, pc);
    instruction->as.local_jump.right =
        mvm_ram_peek_uhalf(mvm->ram, pc + sizeof(uint16_t));
    instruction->as.local_jump.comparison =
        (mvm_opcodes_t)mvm_ram_peek_ubyte(mvm->ram, pc + sizeof(uint16_t) * 2);
    instruction->as.local_jump.when_true =
        mvm_ram_peek_ubyte(mvm->ram, pc + sizeof(uint16_t) * 2 + sizeof(uint8_t));
    instruction->as.local_jump.address =
        mvm_ram_peek_uword(mvm->ram, pc + sizeof(uint16_t) * 2 + sizeof(uint8_t) * 2);
    if (instruction->as.local_jump.comparison < MVM_OP_EQ ||
        instruction->as.local_jump.comparison > MVM_OP_GE ||
        mvm_ram_peek_ubyte(mvm->ram, pc + sizeof(uint16_t) * 2 + sizeof(uint8_t)) > 1)
      return MVM_RESULT_RUNTIME_ERROR;
    instruction->next_pc = pc + width;
  } break;
  case MVM_OP_LOCAL_GET:
  case MVM_OP_LOCAL_SET:
  case MVM_OP_INC_LOCAL:
  case MVM_OP_DEC_LOCAL: {
    if (!mvm_has_operand_for(mvm, execution, sizeof(uint16_t)))
      return MVM_RESULT_RUNTIME_ERROR;

    instruction->as.local_index = mvm_ram_peek_uhalf(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution));
    instruction->next_pc += sizeof(uint16_t);
  } break;
  case MVM_OP_FIELD_GET:
  case MVM_OP_FIELD_SET:
  case MVM_OP_FIELD_ADDR: {
    if (!mvm_has_operand_for(mvm, execution, sizeof(uint16_t)))
      return MVM_RESULT_RUNTIME_ERROR;

    instruction->as.field_index = mvm_ram_peek_uhalf(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution));
    instruction->next_pc += sizeof(uint16_t);
  } break;
  case MVM_OP_VA_PACK: {
    if (!mvm_has_operand_for(mvm, execution, sizeof(uint32_t)))
      return MVM_RESULT_RUNTIME_ERROR;
    const uint32_t count = mvm_ram_peek_uword(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution));
#if SIZE_MAX < UINT64_MAX
    if (count > (SIZE_MAX - sizeof(uint32_t)) / sizeof(uint32_t))
      return MVM_RESULT_RUNTIME_ERROR;
#endif
    const size_t width = sizeof(uint32_t) + (size_t)count * sizeof(uint32_t);
    if (!mvm_has_operand_for(mvm, execution, width))
      return MVM_RESULT_RUNTIME_ERROR;
    instruction->as.va_pack.count = count;
    instruction->as.va_pack.types_pc = *mvm_execution_pc_for((mvm_t *)mvm, execution) + sizeof(uint32_t);
    instruction->next_pc = *mvm_execution_pc_for((mvm_t *)mvm, execution) + width;
    for (size_t i = 0; i < count; ++i) {
      const uint32_t type_id =
          mvm_ram_peek_uword(mvm->ram, instruction->as.va_pack.types_pc + i * 4);
      if (!mvm_type_va_allowed(mvm, type_id))
        return MVM_RESULT_RUNTIME_ERROR;
    }
  } break;
  case MVM_OP_UNION_NEW: {
    if (!mvm_has_operand_for(mvm, execution, 8))
      return MVM_RESULT_RUNTIME_ERROR;
    instruction->as.union_new.union_type_id = mvm_ram_peek_uword(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution));
    instruction->as.union_new.variant_type_id = mvm_ram_peek_uword(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution) + 4);
    const mvm_type_t *type = mvm_type_by_id(mvm, instruction->as.union_new.union_type_id);
    if (!type || type->tag != MVM_TYPE_UNION ||
        !mvm_type_by_id(mvm, instruction->as.union_new.variant_type_id))
      return MVM_RESULT_RUNTIME_ERROR;
    bool member = false;
    for (size_t i = 0; i < type->variant_count; ++i)
      if (type->variant_type_ids[i] == instruction->as.union_new.variant_type_id)
        member = true;
    if (!member) return MVM_RESULT_RUNTIME_ERROR;
    instruction->next_pc += 8;
  } break;
  case MVM_OP_UNION_GET:
  case MVM_OP_UNION_WIDEN:
  case MVM_OP_TYPED_CONSTRUCT:
  case MVM_OP_ARRAY_NEW_TYPED:
  case MVM_OP_TYPED_ALLOC:
  case MVM_OP_VALUE_COPY:
  case MVM_OP_VALUE_ZERO:
  case MVM_OP_VA_ARG:
  case MVM_OP_REF_READ:
  case MVM_OP_REF_WRITE:
  case MVM_OP_TYPED_INIT:
  case MVM_OP_LIT_TYPE_ID: {
    if (!mvm_has_operand_for(mvm, execution, sizeof(uint32_t)))
      return MVM_RESULT_RUNTIME_ERROR;
    instruction->as.type_id = mvm_ram_peek_uword(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution));
    const mvm_type_t *type = mvm_type_by_id(mvm, instruction->as.type_id);
    if (!type ||
        ((op == MVM_OP_UNION_GET || op == MVM_OP_TYPED_CONSTRUCT) && !type->size) ||
        (op == MVM_OP_UNION_WIDEN && type->tag != MVM_TYPE_UNION) ||
        (op == MVM_OP_ARRAY_NEW_TYPED && type->tag != MVM_TYPE_ARRAY) ||
        (op == MVM_OP_VA_ARG &&
                  !mvm_type_va_allowed(mvm, instruction->as.type_id)) ||
        ((op == MVM_OP_REF_READ || op == MVM_OP_REF_WRITE) &&
         (type->tag != MVM_TYPE_REFERENCE ||
          (op == MVM_OP_REF_WRITE && !(type->flags & MVM_TYPE_FLAG_MUTABLE)))) ||
        (op == MVM_OP_TYPED_INIT && !type->size))
      return MVM_RESULT_RUNTIME_ERROR;
    instruction->next_pc += sizeof(uint32_t);
  } break;
  case MVM_OP_GLOBAL_GET:
  case MVM_OP_GLOBAL_SET: {
    if (!mvm_has_operand_for(mvm, execution, sizeof(uint32_t)))
      return MVM_RESULT_RUNTIME_ERROR;

    instruction->as.global_index = mvm_ram_peek_uword(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution));
    instruction->next_pc += sizeof(uint32_t);
  } break;
  case MVM_OP_LIT_UBYTE: {
    if (!mvm_has_operand_for(mvm, execution, sizeof(uint8_t)))
      return MVM_RESULT_RUNTIME_ERROR;

    const uint8_t v = mvm_ram_peek_ubyte(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution));
    const mvm_value_t value = {
        .kind = MVM_KIND_UBYTE,
        .as.ub = v,
    };
    instruction->as.value = value;
    instruction->next_pc += sizeof(v);
  } break;
  case MVM_OP_LIT_UHALF: {
    if (!mvm_has_operand_for(mvm, execution, sizeof(uint16_t)))
      return MVM_RESULT_RUNTIME_ERROR;

    const uint16_t v = mvm_ram_peek_uhalf(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution));
    const mvm_value_t value = {
        .kind = MVM_KIND_UHALF,
        .as.uh = v,
    };
    instruction->as.value = value;
    instruction->next_pc += sizeof(v);
  } break;
  case MVM_OP_LIT_UWORD: {
    if (!mvm_has_operand_for(mvm, execution, sizeof(uint32_t)))
      return MVM_RESULT_RUNTIME_ERROR;

    const uint32_t v = mvm_ram_peek_uword(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution));
    const mvm_value_t value = {
        .kind = MVM_KIND_UWORD,
        .as.uw = v,
    };
    instruction->as.value = value;
    instruction->next_pc += sizeof(v);
  } break;
  case MVM_OP_LIT_ULONG: {
    if (!mvm_has_operand_for(mvm, execution, sizeof(uint64_t)))
      return MVM_RESULT_RUNTIME_ERROR;

    const uint64_t v = mvm_ram_peek_ulong(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution));
    const mvm_value_t value = {
        .kind = MVM_KIND_ULONG,
        .as.ul = v,
    };
    instruction->as.value = value;
    instruction->next_pc += sizeof(v);
  } break;
  case MVM_OP_LIT_SBYTE: {
    if (!mvm_has_operand_for(mvm, execution, sizeof(int8_t)))
      return MVM_RESULT_RUNTIME_ERROR;

    const int8_t v = mvm_ram_peek_sbyte(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution));
    const mvm_value_t value = {
        .kind = MVM_KIND_SBYTE,
        .as.sb = v,
    };
    instruction->as.value = value;
    instruction->next_pc += sizeof(v);
  } break;
  case MVM_OP_LIT_SHALF: {
    if (!mvm_has_operand_for(mvm, execution, sizeof(int16_t)))
      return MVM_RESULT_RUNTIME_ERROR;

    const int16_t v = mvm_ram_peek_shalf(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution));
    const mvm_value_t value = {
        .kind = MVM_KIND_SHALF,
        .as.sh = v,
    };
    instruction->as.value = value;
    instruction->next_pc += sizeof(v);
  } break;
  case MVM_OP_LIT_SWORD: {
    if (!mvm_has_operand_for(mvm, execution, sizeof(int32_t)))
      return MVM_RESULT_RUNTIME_ERROR;

    const int32_t v = mvm_ram_peek_sword(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution));
    const mvm_value_t value = {
        .kind = MVM_KIND_SWORD,
        .as.sw = v,
    };
    instruction->as.value = value;
    instruction->next_pc += sizeof(v);
  } break;
  case MVM_OP_LIT_SLONG: {
    if (!mvm_has_operand_for(mvm, execution, sizeof(int64_t)))
      return MVM_RESULT_RUNTIME_ERROR;

    const int64_t v = mvm_ram_peek_slong(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution));
    const mvm_value_t value = {
        .kind = MVM_KIND_SLONG,
        .as.sl = v,
    };
    instruction->as.value = value;
    instruction->next_pc += sizeof(v);
  } break;
  case MVM_OP_LIT_FLOAT: {
    _Static_assert(sizeof(float) == sizeof(uint32_t),
                   "Float literals require 32-bit float");
    if (!mvm_has_operand_for(mvm, execution, sizeof(uint32_t)))
      return MVM_RESULT_RUNTIME_ERROR;

    const uint32_t bits = mvm_ram_peek_uword(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution));
    mvm_value_t value = {.kind = MVM_KIND_FLOAT};
    memcpy(&value.as.f, &bits, sizeof(bits));
    instruction->as.value = value;
    instruction->next_pc += sizeof(bits);
  } break;
  case MVM_OP_LIT_DOUBLE: {
    _Static_assert(sizeof(double) == sizeof(uint64_t),
                   "Double literals require 64-bit double");
    if (!mvm_has_operand_for(mvm, execution, sizeof(uint64_t)))
      return MVM_RESULT_RUNTIME_ERROR;

    const uint64_t bits = mvm_ram_peek_ulong(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution));
    mvm_value_t value = {.kind = MVM_KIND_DOUBLE};
    memcpy(&value.as.d, &bits, sizeof(bits));
    instruction->as.value = value;
    instruction->next_pc += sizeof(bits);
  } break;
  case MVM_OP_LIT_NULL: {
    const mvm_value_t value = {
        .kind = MVM_KIND_NULL,
    };
    instruction->as.value = value;
  } break;
  case MVM_OP_LIT_FALSE: {
    const mvm_value_t value = {
        .kind = MVM_KIND_BOOL,
        .as.b = false,
    };
    instruction->as.value = value;
  } break;
  case MVM_OP_LIT_TRUE: {
    const mvm_value_t value = {
        .kind = MVM_KIND_BOOL,
        .as.b = true,
    };
    instruction->as.value = value;
  } break;
  case MVM_OP_LIT_STRING: {
    if (!mvm_has_operand_for(mvm, execution, sizeof(uint32_t)))
      return MVM_RESULT_RUNTIME_ERROR;

    const uint32_t address = mvm_ram_peek_uword(mvm->ram, *mvm_execution_pc_for((mvm_t *)mvm, execution));
    if (address >= mvm->ram->size)
      return MVM_RESULT_RUNTIME_ERROR;
    const char *data = (const char *)mvm->ram->data + address;
    if (!memchr(data, '\0', mvm->ram->size - address))
      return MVM_RESULT_RUNTIME_ERROR;

    const mvm_value_t value = {.kind = MVM_KIND_STRING, .as.s = (char *)data};
    size_t byte_length;
    size_t character_length;
    const mvm_result_t status =
        mvm_measure_string(mvm, value, &byte_length, &character_length);
    if (status != MVM_RESULT_OK)
      return status;
    instruction->as.value = value;
    instruction->next_pc += sizeof(address);
  } break;
  case MVM_OP_COUNT:
  default:
    fprintf(stderr, "[FATAL ERROR]: Unknown opcode %i at %zX\n", op,
            *mvm_execution_pc_for((mvm_t *)mvm, execution) - 1);
    return MVM_RESULT_RUNTIME_ERROR;
  }

  return MVM_RESULT_OK;
}
