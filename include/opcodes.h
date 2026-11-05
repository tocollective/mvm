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

typedef enum mvm_opcodes {
  MVM_OP_RETURN,
  MVM_OP_CALL,      // uword address, ubyte argument count
  MVM_OP_TAIL_CALL, // uword address, ubyte argument count; reuses current frame
  MVM_OP_LIT_FUNC,  // uword bytecode address; → FUNC
  MVM_OP_CALL_INDIRECT, // ubyte argument count; args… FUNC → result
  MVM_OP_JUMP,          // uword address
  MVM_OP_JUMP_IF_FALSE, // uword address; b →
  MVM_OP_JUMP_IF_TRUE,  // uword address; b →
  MVM_OP_LOCAL_GET,     // uhalf index
  MVM_OP_LOCAL_SET,     // uhalf index
  MVM_OP_GLOBAL_GET,    // uword index
  MVM_OP_GLOBAL_SET,    // uword index
  MVM_OP_LIT_UBYTE,     // unsigned char
  MVM_OP_LIT_UHALF,     // unsigned short
  MVM_OP_LIT_UWORD,     // unsigned int
  MVM_OP_LIT_ULONG,     // unsigned long
  MVM_OP_LIT_SBYTE,     // signed char
  MVM_OP_LIT_SHALF,     // signed short
  MVM_OP_LIT_SWORD,     // signed word
  MVM_OP_LIT_SLONG,     // signed long
  MVM_OP_LIT_FLOAT,     // float
  MVM_OP_LIT_DOUBLE,    // double
  MVM_OP_LIT_NULL,      // null
  MVM_OP_LIT_FALSE,     // true
  MVM_OP_LIT_TRUE,      // false
  MVM_OP_LIT_STRING,    // uword address
  MVM_OP_POP,           // a →
  MVM_OP_DUP,           // a → a a
  MVM_OP_SWAP,          // a b → b a
  MVM_OP_ADD,           // a b → a + b
  MVM_OP_SUB,           // a b → a - b
  MVM_OP_MUL,           // a b → a * b
  MVM_OP_DIV,           // a b → a / b
  MVM_OP_MOD,           // a b → a % b
  MVM_OP_NEG,           // a → -a
  MVM_OP_INC,           // a → a + 1
  MVM_OP_DEC,           // a → a - 1
  MVM_OP_EQ,            // a b → a == b
  MVM_OP_NE,            // a b → a != b
  MVM_OP_LT,            // a b → a < b
  MVM_OP_LE,            // a b → a <= b
  MVM_OP_GT,            // a b → a > b
  MVM_OP_GE,            // a b → a >= b
  MVM_OP_NOT,           // b → !b
  MVM_OP_BAND,          // a b → a & b
  MVM_OP_BOR,           // a b → a | b
  MVM_OP_BXOR,          // a b → a ^ b
  MVM_OP_BNOT,          // a → ~a
  MVM_OP_SHL,           // a b → a << b
  MVM_OP_SHR,           // a b → a >> b
  MVM_OP_CAST,          // ubyte kind; v → v'
  MVM_OP_TYPEOF,        // v → type
  MVM_OP_IS,            // ubyte kind; v → bool
  MVM_OP_LIT_TYPE,      // ubyte kind; → type
  MVM_OP_CONCAT,        // a b → a..b
  MVM_OP_STRING_LEN,    // string → length
  MVM_OP_STRING_GET,    // string index → code point
  MVM_OP_NATIVE,        // uhalf id, ubyte argument count
  MVM_OP_LOAD,        // ubyte kind; integer byte offset → value (BOOL/numeric)
  MVM_OP_STORE,       // ubyte kind; integer byte offset, value → (BOOL/numeric)
  MVM_OP_ALLOC,       // integer byte size → managed REFERENCE
  MVM_OP_ARRAY_NEW,   // ubyte kind; integer length → array
  MVM_OP_ARRAY_GET,   // array, integer index → value
  MVM_OP_ARRAY_SET,   // array, integer index, value →
  MVM_OP_ARRAY_LEN,   // array → UWORD length
  MVM_OP_FIELD_GET,   // uhalf index; object → value
  MVM_OP_FIELD_SET,   // uhalf index; object, value →
  MVM_OP_FIELD_ADDR,  // uhalf index; typed object → interior reference
  MVM_OP_TYPED_ALLOC, // uword type id; → reference
  MVM_OP_VALUE_COPY,  // uword type id; destination, source →
  MVM_OP_VALUE_ZERO,  // uword type id; destination →
  // Append new opcodes to preserve existing bytecode numbers.
  MVM_OP_ARRAY_CAP,     // array → UWORD capacity
  MVM_OP_ARRAY_RESERVE, // array, integer capacity →
  MVM_OP_ARRAY_RESIZE,  // array, integer length →
  MVM_OP_ARRAY_PUSH,    // array, value →
  MVM_OP_ARRAY_POP,     // array → removed value
  MVM_OP_ARRAY_INSERT,  // array, integer index, value →
  MVM_OP_ARRAY_REMOVE,  // array, integer index → removed value
  MVM_OP_VA_PACK,       // uword count, uword type ids[count]; extras… → packet
  MVM_OP_VA_FORWARD,    // → current frame's packet
  MVM_OP_VA_COUNT,      // → ULONG packet element count
  MVM_OP_VA_ARG,        // uword type id; unsigned index → value
  MVM_OP_REF_READ,      // uword reference type id; reference → value
  MVM_OP_REF_WRITE,     // uword mutable reference type id; reference, value →
  MVM_OP_TYPED_INIT,    // uword pointee type id; value → mutable reference
  MVM_OP_LIT_TYPE_ID,   // uword type id; → full TYPE
  MVM_OP_UNION_NEW,       // uword union type, uword variant type; value → union
  MVM_OP_UNION_VARIANTOF, // union → full Type of active variant
  MVM_OP_UNION_GET,       // uword variant type; union → copied payload
  MVM_OP_UNION_WIDEN,     // uword target union type; union → widened union
  MVM_OP_TYPED_CONSTRUCT, // uword type; → internal zeroed construction storage
  MVM_OP_ARRAY_NEW_TYPED, // uword array type; length → typed array
  MVM_OP_INC_LOCAL,       // uhalf index; local numeric value += 1
  MVM_OP_DEC_LOCAL,       // uhalf index; local numeric value -= 1
  MVM_OP_BREAKPOINT,      // no operands; suspend execution
  MVM_OP_JUMP_IF_LOCAL,   // left/right uhalf, comparison ubyte, when_true ubyte, address uword
  MVM_OP_NULL_CHECK,      // nullable value → same value; traps on null
  MVM_OP_COUNT,
} mvm_opcodes_t;
