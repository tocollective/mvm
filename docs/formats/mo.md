<!--
Copyright 2026 tocollective contributors
SPDX-License-Identifier: Apache-2.0

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# MO – Binary mvm Object

[Format Overview](README.md) · [Text MIR](mir.md) · [mvm Container](mvm.md)

Current version: MO Format 3.0 / mvm ISA 2.5. `.mo` is the result of compiling a `.mir` file. This is a relocatable object, not a standalone executable file.

## 1. Core Properties

MO contains:

- logical module name
- full types, signatures, and layouts of structs
- Global values and constant initializers
- functions and bytecode
- Inter-module and native imports
- Exports, including the API of the root module.
- `rodata` and the relocation table.
- Optional debug string and variable tables.

All multi-byte integers are little-endian. `i8/i16/i32/i64` use two's complement, `float` is IEEE 754 binary32, and `double` is binary64.

Fields are not read by casting a pointer to a C-structure: host format and alignment do not affect the representation.

In version 1, the file size does not exceed `UINT32_MAX`. Any addition and multiplication performed during offset checking is done without overflow. Reserved bytes when writing are equal to zero, and when reading, they must be zero.

## 2. Heading

The first 48 bytes:

| Offset | Size | Field | Value |
|---:|---:|---|---|
| 0 | 8 | `magic` | `4D 56 4D 4F 0D 0A 1A 0A` (`MVMO\r\n\x1a\n`) |
| 8 | 2 | `format_major` | 3 |
| 10 | 2 | `format_minor` | 0 |
| 12 | 2 | `isa_major` | 2 |
| 14 | 2 | `isa_minor` | 5 |
| 16 | 4 | `header_size` | 48 for versions 1–3 |
| 20 | 4 | `flags` | 0 |
| 24 | 8 | `file_size` | exact file size |
| 32 | 8 | `section_table_offset` | Offset of the section table from the beginning of the file |
| 40 | 4 | `section_count` | number of catalog entries |
| 44 | 4 | `reserved` | 0 |

The reader rejects other magic, unsupported major version of the format or ISA,
`header_size < 48`, inaccurate `file_size`, a catalog outside the file, and non-zero flags.
When using the same major version, the reader may skip bytes in the header after 48,
if they are described by a newer minor version; for an unknown minor version, this is
only permissible if there are no other required new features. The MO 1.1 / ISA 1.1 reader accepts MO 1.0 / ISA 1.0. The variability flag and opcodes 78–85 require ISA 1.1; an older reader rejects them as an unknown flag or opcode.

## 3. Section Catalog

Each directory entry has a size of 32 bytes:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `type` |
| 4 | 4 | `flags` |
| 8 | 8 | `offset` from the beginning of the file |
| 16 | 8 | `size` of the usable payload |
| 24 | 4 | `entry_size` |
| 28 | 4 | `entry_count` |

For a table with fixed size, `size == entry_size * entry_count`, and `entry_size` is defined below. For the raw section, `entry_size = 0` and `entry_count = 0`. For a section of variable entries, `entry_size = 0`, and `entry_count` specifies the number of entries.

Sections do not overlap with each other, requiring a mandatory header and directory.

An empty section can have `offset = size = 0`. Duplication of any known section type is an error. The directory for the canonical MO is sorted by `type`.

The flag `SECTION_FLAG_CRITICAL = 0x1` indicates that the section should not be ignored.
Unknown sections without `CRITICAL` are skipped; sections with `CRITICAL` are rejected.
For known sections version 1, all flags are zero. Other flags of an unknown section are ignored only together with the section itself.

### 3.1 Section Types

| ID | Name | Record Type | Power |
|---:|---|---|---|
| 1 | `MODULE` | fixed, 24 bytes | exactly 1 |
| 2 | `STRINGS` | raw bytes | 0..1 |
| 3 | `TYPES` | Variables | 0..1 |
| 4 | `GLOBALS` | fixed, 24 bytes | 0..1 |
| 5 | `FUNCTIONS` | fixed, 36 bytes | 0..1 |
| 6 | `IMPORTS` | fixed, 28 bytes | 0..1 |
| 7 | `EXPORTS` | fixed, 32 bytes | 0..1 |
| 8 | `CONSTANTS` | Variables | 0..1 |
| 9 | `CODE` | raw bytes | 0..1 |
| 10 | `RODATA` | raw bytes | 0..1 |
| 11 | `RELOC` | fixed, 16 bytes | 0..1 |
| 12 | `DEBUG_LINES` | Fixed, 24 bytes | 0..1 |
| 13 | `DEBUG_VARIABLES` | Fixed, 20 bytes | 0..1 |

The absence of the optional section is equivalent to an empty one. `MODULE` and `STRINGS` are mandatory, even if the string table is empty. `FUNCTIONS` requires `CODE`; a non-empty `CODE` requires `FUNCTIONS`. Every non-empty table of constants, types, global values, imports, or exports requires all sections to which its entries refer.

## 4. General Elements

### 4.1 `strref`

`strref` – 8 bytes: `offset: u32`, followed by `length: u32`. The range must reside within `STRINGS`; a trailing NUL character is not allowed and is not required. An empty string is encoded with `offset = length = 0`.

All module names, symbols, fields, and source files referenced by `strref` must be valid UTF-8. Within the table, common suffixes are allowed; string overlap is permitted. String comparison is performed byte-by-byte.

### 4.2 Indexes

`type_ref`, `constant_ref`, function indices, global values, and imports –
`u32`, zero-based indexing within the corresponding section. The special value `UINT32_MAX` indicates "absent" only when explicitly stated.

The indices within MO are local. The final addresses for RAM, global ID, native ID, and type ID exist only after mvm linking.

## 5. Section Formats

### 5.1 `MODULE`, 24 bytes

| Offset | Type | Field |
|---:|---|---|
| 0 | `strref` | logical `name` |
| 8 | `strref` | optional original `source` |
| 16 | `u32` | `flags` |
| 20 | `u32` | `reserved` |

`MODULE_FLAG_ROOT = 0x1`. The remaining flags are reserved. The module name rules and the single root rules are specified in the mvm specification. `source` is used for diagnostics and does not participate in import resolution.

### 5.2 `TYPES`

The section contains `entry_count` consecutive entries:

```text
body_size: u32
name: strref
body: u8[body_size]
```

The record should end precisely on the boundary of the next; the last one – precisely at the end of the section. The `body` is encoded according to section 6. The `name` is empty for non-nominal types and must be non-empty for `struct`/`enum`. The type index is the record's sequential number.

### 5.3 `GLOBALS`, 24 bytes

| Offset | Type | Field |
|---:|---|---|
| 0 | `strref` | Internal name |
| 8 | `type_ref` | full type |
| 12 | `constant_ref` | initializer |
| 16 | `u32` | `flags` |
| 20 | `u32` | Reserved, 0 |

`GLOBAL_FLAG_MUTABLE = 0x1`. The remaining flags are reserved. Internal names are unique. The reserved field must be equal to zero.

The initializer is checked against the full type; the code within it is not executed.

The `GLOBALS` table, along with constants, represents logical segments: `rodata`, `data`, and `bss`. These segments contain immutable values initialized with mutable and zero values, respectively. The physical storage of global slots is not required to match the RAM bytecode and is not part of the format.

### 5.4 `FUNCTIONS`, 36 bytes

| Offset | Type | Field |
|---:|---|---|
| 0 | `strref` | Internal name for debugging |
| 4 | `type_ref` | `FUNCTION` type |
| 8 | `u32` | `code_offset` from the beginning of `CODE` |
| 12 | `u32` | `code_size` |
| 16 | `u16` | `local_count`, no parameters |
| 18 | `u16` | `reserved` |
| 20 | `u32` | `max_stack` |
| 24 | `u32` | `flags`, 0 in version 1 |
| 28 | `u32` | `reserved` |

Internal names are unique. Code ranges do not overlap; they reside in `CODE` and have a non-zero size. The number of fixed parameters in a signature is not more than 255, and the number for a variable function is not more than 254. Within a variable function’s frame, after the fixed parameters, there is one hidden package slot. It has no local index; additional local slots begin after it.

Correct indices for `LOCAL_GET`/`LOCAL_SET`: `0..argc + local_count - 1` for non-variable functions and `0..argc + local_count` for variable functions, excluding the index of the hidden package, `argc`.

`max_stack` includes parameters, a hidden package, local and temporary values of the current frame, and must be validated through stack analysis. The order of the canonical MO table matches the order of increasing `code_offset`.

### 5.5 `IMPORTS`, 28 bytes

| Offset | Type | Field |
|---:|---|---|
| 0 | `strref` | `module`; Null only for native |
| 8 | `strref` | External `name` |
| 16 | `u8` | `kind` |
| 17 | 3 bytes | `reserved` |
| 20 | `type_ref` or `u32` | Expected type; `UINT32_MAX` for importing types |
| 24 | `u32` | `flags`, 0 in version 1 |

Types:

| ID | Type | Requirements |
|---:|---|---|
| 1 | `FUNCTION` | If the `module` is not empty, and the `type_ref` references a `FUNCTION`, |
| 2 | `GLOBAL` | The `module` must not be empty, and its type must not be `VOID`. |
| 3 | `TYPE` | The `module` must not be empty, and the type must be `UINT32_MAX`. |
| 4 | `NATIVE_FUNCTION` | The `module` is empty, and the `type_ref` refers to `FUNCTION` |

The combination `(kind, module, name)` is unique. For native, a stricter mvm rule applies: identical names within an entire archive have a single signature. Importing does not contain a separate local alias: the MIR alias is already reflected through table order and relocations.

### 5.6 `EXPORTS`, 32 bytes

| Offset | Type | Field |
|---:|---|---|
| 0 | `strref` | External name |
| 8 | `u8` | `target_kind` |
| 9 | 3 bytes | `reserved` |
| 12 | `u32` | local function index, global value, or type index |
| 16 | `u32` | `flags` |
| 20 | `strref` | Documentation or an empty string |
| 28 | `u32` | `reserved` |

`target_kind`: 1 – function, 2 – global value, 3 – type.
`EXPORT_FLAG_HOST = 0x1`; all other flags are reserved. External names are unique regardless of type. `HOST` is only allowed for functions/global values in the root module.

### 5.7 `CONSTANTS`

The section contains `entry_count` consecutive entries:

```text
body_size: u32
body: u8[body_size]
```

Encoding of `body` is described in section 7. Constants referenced by another constant must have a lower index: the graph within the section must be acyclic. Cross-dependencies via `GLOBAL` are additionally checked by the linker.

### 5.8 `CODE` and `RODATA`

`CODE` contains instructions specified in the header's ISA. The addresses within it are currently local/unfilled;
symbolic operands receive values via `RELOC`. The canonical compiler writes zeros to each operand being relocated.

`RODATA` contains immutable bytes. A string literal is represented by UTF-8 bytes and a trailing NUL character. The address points to the first byte, and the length is stored in a constant or is known to the compiler; the NUL character is not part of the string value. The content of the string literal does not contain `U+0000` and undergoes UTF-8 validation.

### 5.9 `RELOC`, 16 bytes

| Offset | Type | Field |
|---:|---|---|
| 0 | `u32` | Offset of the first byte of the operand for `code_offset` |
| 4 | `u8` | `kind` |
| 5 | 3 bytes | `reserved` |
| 8 | `u32` | `target` |
| 12 | `i32` | `addend` |

Types of Relocations:

| ID | Name | Stored Value | Width |
|---:|---|---|---:|
| 1 | `MODULE_CODE_ADDRESS` | Base address of the `CODE` module plus `addend` | 4 |
| 2 | `MODULE_RODATA_ADDRESS` | Base address of the `RODATA` section for the module + `addend` | 4 |
| 3 | `LOCAL_FUNCTION_ADDRESS` | Address of the `code_offset` of the `target` function | 4 |
| 4 | `IMPORT_FUNCTION_ADDRESS` | Address of the resolved import `target` | 4 |
| 5 | `LOCAL_GLOBAL_INDEX` | global ID of the `target` | 4 |
| 6 | `IMPORT_GLOBAL_INDEX` | Global ID of the resolved import for the `target` | 4 |
| 7 | `LOCAL_TYPE_ID` | Type ID of the `target` type | 4 |
| 8 | `IMPORT_TYPE_ID` | Type ID for permitted import `target` | 4 |
| 9 | `NATIVE_ID` | Native ID of the import for `target` | 2 |

For target types 1–2, `target = 0`; for all others, `addend = 0`. The `addend` for target types 1–2 must be non-negative in version 1. The import relocation must correspond to the relocation. The relocation must be contained within a single operand in `CODE`, not on an opcode, not exceeding the function boundary, and not overlapping with another relocation.

Each operand of `CALL`, `TAIL_CALL`, `LIT_FUNC`, `JUMP`, `JUMP_IF_FALSE`,
`JUMP_IF_TRUE`, `GLOBAL_GET`, `GLOBAL_SET`, `LIT_STRING`, `NATIVE`,
`TYPED_ALLOC`, `VALUE_COPY`, `VALUE_ZERO`, `LIT_TYPE_ID`, `VA_ARG`, `REF_READ`,
`REF_WRITE`, and `TYPED_INIT` containing an address/ID must have exactly one
matching relocation. Each of the `count` type IDs in `VA_PACK` also has exactly
one `LOCAL_TYPE_ID` or `IMPORT_TYPE_ID` relocation.
`argc`, `count`, `kind`, and other immediate operands are not relocated.

### 5.10 `DEBUG_LINES`, 24 bytes

| Offset | Type | Field |
|---:|---|---|
| 0 | `u32` | function index |
| 4 | `u32` | Instruction offset within `CODE` |
| 8 | `strref` | source file |
| 16 | `u32` | String, 1-based indexing |
| 20 | `u32` | column, 1-based indexing; 0 = unknown |

The offset must point to the beginning of the instruction within the specified function.

Entries are sorted by `(function, code_offset)` and do not contain duplicates of such a pair. Debug data does not affect execution.

### 5.11 `DEBUG_VARIABLES`, 20 bytes

| Offset | Type | Field |
|---:|---|---|
| 0 | `u32` | function index |
| 4 | `u32` | local slot frame index |
| 8 | `strref` | Non-empty original variable or parameter name |
| 16 | `type_ref` | fully declared type |

The slot must exist within the signature and the `local_count` function. A hidden variadic packet is prohibited. Records are sorted by `(function, slot)`, and each such pair is unique. The section associates names and types with slots, but does not modify the bytecode or its verification. The debugger obtains global names and types from `GLOBALS`.

## 6. Type Encoding

The first byte of the `body` is a tag. Following this are unaligned fields. `type_ref` references the `TYPES` entry and can be a forward reference.

| Tag | Type | Remaining `body` |
|---:|---|---|
| `0x00` | `VOID` | None |
| `0x01` | `BOOL` | false |
| `0x02` | `UBYTE` | No |
| `0x03` | `UHALF` | no |
| `0x04` | `UWORD` | no |
| `0x05` | `ULONG` | no |
| `0x06` | `SBYTE` | no |
| `0x07` | `SHALF` | no |
| `0x08` | `SWORD` | No |
| `0x09` | `SLONG` | no |
| `0x0A` | `FLOAT` | No |
| `0x0B` | `DOUBLE` | no |
| `0x0C` | `STRING` | No |
| `0x0D` | `TYPE` | None |
| `0x0E` | `THREAD` | no |
| `0x0F` | `MUTEX` | no |
| `0x10` | `CONDITION` | false |
| `0x11` | `ATOMIC` | No |
| `0x20` | `ARRAY` | `element: type_ref`, `flags: u32` |
| `0x21` | `REFERENCE` | `pointee: type_ref`, `flags: u32` |
| `0x22` | `FUNCTION` | `flags: u32`, `argc: u16`, `reserved: u16`, `params: type_ref[argc]`, `result: type_ref` |
| `0x23` | `STRUCT` | `size: u64`, `alignment: u32`, `flags: u32`, `field_count: u32`, followed by fields |
| `0x24` | `ENUM` | `underlying: type_ref`, `member_count: u32`, followed by elements |
| `0x25` | `IMPORTED_TYPE` | `import_index: u32` |

The `STRUCT` field: `name: strref`, `offset: u64`, `type: type_ref` – 20 bytes.
The `ENUM` element: `name: strref`, `value: u64` – 16 bytes; for signed base types, the value is stored with an offset of up to two.

Flags:

- `TYPE_FLAG_MUTABLE = 0x1` for `ARRAY` indicates `mut T[]` and is only allowed
  directly within a fixed parameter type.
- The same bit in `REFERENCE` indicates `@mut T`.
- `TYPE_FLAG_PACKED = 0x1` for a `STRUCT` indicates packed `layout`.
- `TYPE_FLAG_VARIADIC = 0x1` for `FUNCTION` indicates a hidden package following fixed parameters.
- `TYPE_FLAG_NULLABLE = 0x2` marks `T?` for primitive reference-like types, `ARRAY`, `REFERENCE`, and `FUNCTION`. It participates in full type identity and is separate from the variadic flag in `FUNCTION`.
- All other flags are reserved and must be zero. MO 1/2 types without the nullable flag retain their legacy implicit-nullability interpretation.

The `FUNCTION` has `argc` and `params` fields, which only describe fixed parameters. The package does not have a separate `type_ref` and is not included in `argc`. Variability participates in the structural identity of the functional type.

`IMPORTED_TYPE` must refer to an import of the type `TYPE`. After linking, it represents the exported nominal type, not a copy of it. Structural equivalence is applied to primitives, arrays, references, and functions. Local identity for `STRUCT`/`ENUM` is determined by the pair `(module, type index)`. Imports and any number of export aliases preserve this identity, without creating a new type. Type aliases in MO are not present.

Recursion of types is only permitted through `REFERENCE`. The size and alignment of structures are checked by their fields; fields do not overlap, extend beyond the size, or be unsorted by offset. An empty structure is prohibited. The base type `ENUM` is one of eight integer types; the names and values of its elements are unique.

`VOID` is only permissible as a function result and as an internal value of `Type`;
it cannot be a field, an array element, content within a reference, a parameter, or
a global value.

## 7. Encoding Constants

The first byte of the `body` – is a tag, followed by the data. The scalar initializer must precisely match the expected type; for `ENUM`, it uses the tag of its base type.

| Tag | Constant | Data after tag |
|---:|---|---|
| `0x00` | `ZERO` | no |
| `0x01` | `NULL` | no |
| `0x02` | `BOOL` | `value: u8`, only 0 or 1 |
| `0x03` | `UBYTE` | `value: u8` |
| `0x04` | `UHALF` | `value: u16` |
| `0x05` | `UWORD` | `value: u32` |
| `0x06` | `ULONG` | `value: u64` |
| `0x07` | `SBYTE` | `value: i8` |
| `0x08` | `SHALF` | `value: i16` |
| `0x09` | `SWORD` | `value: i32` |
| `0x0A` | `SLONG` | `value: i64` |
| `0x0B` | `FLOAT` | `bits: u32` |
| `0x0C` | `DOUBLE` | `bits: u64` |
| `0x20` | `STRING` | `rodata_offset: u32`, `byte_length: u32` |
| `0x21` | `FUNCTION` | `space: u8`, `reserved: u8[3]`, `index: u32` |
| `0x22` | `TYPE_VALUE` | `type: type_ref` |
| `0x23` | `GLOBAL` | `space: u8`, `reserved: u8[3]`, `index: u32` |
| `0x24` | `ARRAY` | `array_type: type_ref`, `length: u32`, `items: constant_ref[length]` |
| `0x25` | `STRUCT` | `struct_type: type_ref`, `field_count: u32`, `fields: constant_ref[field_count]` |
| `0x26` | `OBJECT` | `reference_type: type_ref`, `value: constant_ref` |
| `0x27` | `ARRAY_N` | `array_type: type_ref`, `length: u32`, `item_count: u32`, `items: constant_ref[item_count]` |

For `FUNCTION` and `GLOBAL`: `space = 0` indicates a local table, `1` indicates `IMPORTS`; other values are prohibited. The import must have the corresponding format.

- `STRING` checks the range, null termination, and UTF-8 validity within `RODATA`.
- `GLOBAL` can only read immutable global values and preserves the identity of referenced values within an instance.
- `ARRAY` creates an array of a specified length; `length` represents the number of elements.
- `ARRAY_N` sets the initial length regardless of the number of elements;
  `item_count <= length`, with any remainder being filled with `ZERO`. This is an initialization template, not a separate array type.
- `STRUCT` contains exactly one value in a field, in the order specified by the field table.
- `OBJECT` creates a new managed object of the specified reference type.
- Every `constant_ref` within a constant refers to a record with a smaller index.

When creating an instance of a dependency, calculations are performed without executing the bytecode. The loop using `GLOBAL` is incorrect. Each template of `ARRAY`, `ARRAY_N`, and `OBJECT` creates a new object in every instance; subsequent reads via `GLOBAL` use already initialized values. Referencing values are maintained by the GC throughout the initialization process.

## 8. Runtime-kind

The operand `kind` for instructions uses numbers from the current `mvm_kind_t`.

| ID | Kind | ID | Kind |
|---:|---|---:|---|
| 0 | `VOID` | 10 | `SLONG` |
| 1 | `NULL` | 11 | `FLOAT` |
| 2 | `BOOL` | 12 | `DOUBLE` |
| 3 | `UBYTE` | 13 | `STRING` |
| 4 | `UHALF` | 14 | `ARRAY` |
| 5 | `UWORD` | 15 | `TYPE` |
| 6 | `ULONG` | 16 | `REFERENCE` |
| 7 | `SBYTE` | 17 | `FUNC` |
| 8 | `SHALF` | 18 | `THREAD` |
| 9 | `SWORD` | 19+ | Invalid |

This is a compact runtime category, not a full type from `TYPES`. Full types of functions, references, or arrays are verified through metadata where the ISA requires it.

## 9. Address Model

After combining, all `CODE` and `RODATA` reside in shared RAM with 32-bit addresses. Zero is a valid address. The headers and MO tables do not reside in RAM.

- `CALL`, `TAIL_CALL`, `LIT_FUNC` and jumps contain an absolute RAM address.
- `LIT_STRING` contains the absolute address of a null-terminated string.
- `GLOBAL_GET/SET`, `NATIVE` and typed operations use indices assigned to the instance.
- The final address space size does not exceed `UINT32_MAX + 1`, but
  the address `UINT32_MAX` is only permitted if the corresponding byte/operand is entirely
  located in RAM. In practice, the configurable RAM limit may be smaller.

Moving (relocation) is prohibited without using it. After loading and preparing the image, `CODE` ranges become immutable: bytecode `STORE` is redirected within them, and the host should not modify these bytes via RAM. This allows the VM to safely reuse prepared instructions. `RODATA` remains immutable by design.

## 10. ISA 2.2

The opcode byte comes first, followed by unaligned operands. The top of the stack column is on the right; `a b` indicates that `b` was popped last. All functions in the current ABI return a single slot; the `Void` result is represented internally by `NULL`. `RETURN` replaces arguments and local frame values with that slot.

| ID | Instruction | Operands | Stack / Action |
|---:|---|---|---|
| 0 | `RETURN` | — | Return the maximum value |
| 1 | `CALL` | `address: u32`, `argc: u8` | `args… → result` |
| 2 | `TAIL_CALL` | `address: u32`, `argc: u8` | `args… → result`, reuses frame |
| 3 | `LIT_FUNC` | `address: u32` | `→ FUNC` |
| 4 | `CALL_INDIRECT` | `argc: u8` | `args… FUNC → result` |
| 5 | `JUMP` | `address: u32` | unconditional jump |
| 6 | `JUMP_IF_FALSE` | `address: u32` | `BOOL →` |
| 7 | `JUMP_IF_TRUE` | `address: u32` | `BOOL →` |
| 8 | `LOCAL_GET` | `index: u16` | `→ value` |
| 9 | `LOCAL_SET` | `index: u16` | `value →` |
| 10 | `GLOBAL_GET` | `index: u32` | `→ value` |
| 11 | `GLOBAL_SET` | `index: u32` | `value →` |
| 12 | `LIT_UBYTE` | `value: u8` | `→ UBYTE` |
| 13 | `LIT_UHALF` | `value: u16` | `→ UHALF` |
| 14 | `LIT_UWORD` | `value: u32` | `→ UWORD` |
| 15 | `LIT_ULONG` | `value: u64` | `→ ULONG` |
| 16 | `LIT_SBYTE` | `value: i8` | `→ SBYTE` |
| 17 | `LIT_SHALF` | `value: i16` | `→ SHALF` |
| 18 | `LIT_SWORD` | `value: i32` | `→ SWORD` |
| 19 | `LIT_SLONG` | `value: i64` | `→ SLONG` |
| 20 | `LIT_FLOAT` | `bits: u32` | `→ FLOAT` |
| 21 | `LIT_DOUBLE` | `bits: u64` | `→ DOUBLE` |
| 22 | `LIT_NULL` | — | `→ NULL` |
| 23 | `LIT_FALSE` | — | `→ BOOL false` |
| 24 | `LIT_TRUE` | — | `→ BOOL true` |
| 25 | `LIT_STRING` | `address: u32` | `→ STRING` |
| 26 | `POP` | — | `a →` |
| 27 | `DUP` | — | `a → a a` |
| 28 | `SWAP` | — | `a b → b a` |
| 29 | `ADD` | — | `a b → a+b` |
| 30 | `SUB` | — | `a b → a-b` |
| 31 | `MUL` | — | `a b → a*b` |
| 32 | `DIV` | — | `a b → a/b` |
| 33 | `MOD` | — | `a b → a%b` |
| 34 | `NEG` | — | `a → -a` |
| 35 | `INC` | — | `a → a+1` |
| 36 | `DEC` | — | `a → a-1` |
| 37 | `EQ` | — | `a b → BOOL` |
| 38 | `NE` | — | `a b → BOOL` |
| 39 | `LT` | — | `a b → BOOL` |
| 40 | `LE` | — | `a b → BOOL` |
| 41 | `GT` | — | `a b → BOOL` |
| 42 | `GE` | — | `a b → BOOL` |
| 43 | `NOT` | — | `BOOL → BOOL` |
| 44 | `BAND` | — | `a b → a&b` |
| 45 | `BOR` | — | `a b → a|b` |
| 46 | `BXOR` | — | `a b → a^b` |
| 47 | `BNOT` | — | `a → ~a` |
| 48 | `SHL` | — | `a b → a<<b` |
| 49 | `SHR` | — | `a b → a>>b` |
| 50 | `CAST` | `kind: u8` | `a → a'`, runtime cast |
| 51 | `TYPEOF` | — | `a → TYPE`, legacy runtime-kind |
| 52 | `IS` | `kind: u8` | `a → BOOL` |
| 53 | `LIT_TYPE` | `kind: u8` | `→ TYPE` |
| 54 | `CONCAT` | — | `STRING STRING → STRING` |
| 55 | `STRING_LEN` | — | `STRING → UWORD` |
| 56 | `STRING_GET` | — | `STRING index → UWORD code point` |
| 57 | `NATIVE` | `id: u16`, `argc: u8` | `args… → result` |
| 58 | `LOAD` | `kind: u8` | `byte_offset → scalar` |
| 59 | `STORE` | `kind: u8` | `byte_offset value →` |
| 60 | `ALLOC` | — | `byte_size → REFERENCE` |
| 61 | `ARRAY_NEW` | `kind: u8` | `length → ARRAY` |
| 62 | `ARRAY_GET` | — | `array index → value` |
| 63 | `ARRAY_SET` | — | `array index value →` |
| 64 | `ARRAY_LEN` | — | `array → UWORD` |
| 65 | `FIELD_GET` | `index: u16` | `object → value` |
| 66 | `FIELD_SET` | `index: u16` | `object value →` |
| 67 | `FIELD_ADDR` | `index: u16` | `typed object → REFERENCE` |
| 68 | `TYPED_ALLOC` | `type_id: u32` | `→ REFERENCE` |
| 69 | `VALUE_COPY` | `type_id: u32` | `destination source →` |
| 70 | `VALUE_ZERO` | `type_id: u32` | `destination →` |
| 71 | `ARRAY_CAP` | — | `array → UWORD` |
| 72 | `ARRAY_RESERVE` | — | `array capacity →` |
| 73 | `ARRAY_RESIZE` | — | `array length →` |
| 74 | `ARRAY_PUSH` | — | `array value →` |
| 75 | `ARRAY_POP` | — | `array → removed value` |
| 76 | `ARRAY_INSERT` | — | `array index value →` |
| 77 | `ARRAY_REMOVE` | — | `array index → removed value` |
| 78 | `VA_PACK` | `count: u32`, `type_id: u32[count]` | `extra[count]… → packet` |
| 79 | `VA_FORWARD` | — | `→ packet` of the current frame |
| 80 | `VA_COUNT` | — | `→ ULONG` |
| 81 | `VA_ARG` | `type_id: u32` | Extracting `index → value` from the current frame's packet |
| 82 | `REF_READ` | `ref_type_id: u32` | `reference → value` |
| 83 | `REF_WRITE` | `ref_type_id: u32` | `reference value →` |
| 84 | `TYPED_INIT` | `pointee_type_id: u32` | `value → REFERENCE` |
| 85 | `LIT_TYPE_ID` | `type_id: u32` | `→ TYPE` |

ID 86–255 are invalid in version 1.1. New instructions are only added to the end, with an increase in the compatible ISA version; existing numbers are not reused.

`ARRAY_NEW VOID` is invalid. `LOAD/STORE` only accept scalar kinds with a non-zero width (like `BOOL` and numeric types); other kinds are checked according to the specific instruction rules.

### 10.1 Variable Call ABI

Package – an internal, immutable value within a stack slot. It contains 8-byte representations of additional arguments, a complete `type_id` for each value, and GC roots information. The package is not a M value: it cannot be stored in a local or global slot, field, or array, returned via `RETURN`, or passed to a regular non-variadic function. It does not have a `Type` descriptor and is not a C `va_list`.

`VA_PACK` extracts `count` additional values and creates a package. Each type in `type_id[count]` corresponds to a value in the order of calling. Integer, `Bool`, `Float`, `Double`, `String`, `Type`, enum, structs, arrays, unions, `@T`/`@mut T`, functions, and handles `Thread`, `Mutex`, `ConditionVariable`, and `Atomic` are allowed. `Void` is not permitted. A `count` of 0 creates an empty package. The number of additional arguments is limited to `UINT32_MAX`, memory, and `max_stack`, but not by `CALL.argc`.

When calling a variable function, fixed arguments occupy the first `fixed_argc` slots, the next slot contains the package. Therefore, `CALL`, `TAIL_CALL`, `CALL_INDIRECT`, and `NATIVE` have `argc = fixed_argc + 1`. For non-variable calls, `argc = fixed_argc`. The hidden package is not accessible via `LOCAL_GET`/`LOCAL_SET` and is accounted for in `max_stack`.

`VA_FORWARD` passes the current frame's data without copying, preserving the original values and types.

It is only valid within a variadic function. In a standard call, the original frame retains the data packet; in a tail VM, ownership or the packet itself is transferred via a reference counter. The packet remains valid until the original call returns, and it holds all referenced arguments.  `VA_FORWARD` is the only additional argument to the call.

`VA_COUNT` returns the number of elements in the current frame's packet. `VA_ARG`
accepts an unsigned index and checks bounds and the expected complete type.
Integers allow signedness changes and truncation to the low bits, but not widening.
Enums require the same nominal identity. `Float`, `Double`, `Bool`, `String`, and
`Type` require an exact type. Functions are checked against their full signature
and variadic status. An `@T` can be read from `@mut T`; an `@mut T` cannot be
obtained from `@T`. `VA_FORWARD`, `VA_COUNT`, and `VA_ARG` are invalid outside a variadic function.

The `NATIVE` variant utilizes the same hidden package. The VM employs a native adapter, which maintains GC roots and provides a validated package representation for the executor. Using `...`, `va_list`, and the `NATIVE` variant of libffi, this ABI is not replaceable.

### 10.2 Typed References

`REF_READ` accepts a `ref_type_id` of type `@T` or `@mut T`. `REF_WRITE`—only
`@mut T`. `TYPED_INIT` accepts a `pointee_type_id` of the content type `T`, where
`Void` is prohibited. All three identifiers receive values through `LOCAL_TYPE_ID` or `IMPORT_TYPE_ID`.

The VM's referenced value contains the full type and access rights, including internal references. Reading and writing check for `null`, object membership to the current instance, the complete type and content of the reference; writing additionally requires `@mut`. For internal references, the value range must be entirely within the original object. Type or access rights mismatch – a runtime error, not an unchecked pointer cast.

`TYPED_INIT` receives a pre-calculated source, retains both the source and the new object as GC roots, and returns `@mut T` only after complete initialization. Structures are copied by value, without deep copying of nested objects. Scalars, strings, arrays, functions, `Type`, and embedded references are initialized to their representation. If a partially initialized reference encounters an error, it is not returned.

### 10.3 Complete Type Descriptors

`LIT_TYPE_ID` generates a complete `Type` descriptor based on a movable `type_id`. Aliases are resolved to a single canonical type. Structural types are deduplicated, and `struct`/`enum` maintain their `(module, type index)` identity, while importing a nominal type preserves the identity of the imported type. An array stores the element type, a reference – the type of its content, and permissions, a function – its parameters, result, and variability. `EQ`/`NE` compare canonical full `type_id`s.

`LIT_TYPE` and `TYPEOF` are maintained as legacy runtime-kind operations; `IS` remains a runtime-kind check. The M compiler does not use them for encoding `typeof`: it determines the static type of the operand and generates only `LIT_TYPE_ID`, without creating code for the operand. Therefore, `typeof` does not perform calls, allocations, reading `null`, or boundary checks.

The semantics of operations, Unicode strings, arrays, GC, and execution errors are determined by the mvm/M contract, not by the MO container.

## 11. Mandatory bytecode verification

The loader verifies the code of each function independently and does not execute partially verified modules.

1. Decoding from `code_offset` to `code_offset + code_size` proceeds using integer instructions and ends precisely on the boundary of a function. An unknown opcode, a truncated operand, overflow of the `VA_PACK` size, or an invalid `kind` – all result in an error.
2. All relocations are applied to the correct operands. The purpose of `CALL`, `TAIL_CALL`, and `LIT_FUNC` is the beginning of a function. `CALL`, `TAIL_CALL`, and `NATIVE` have `argc = fixed_argc` for a non-variable function and `argc = fixed_argc + 1` for a variable function. Indirect calls verify the complete signature during execution.
3. Every jump must target the start of an instruction within **the same function**.
   Jumps into another function, the middle of an instruction, or padding are forbidden.
4. `LIT_STRING` points to a verified, null-terminated UTF-8 string in `RODATA` of one of the modules, not in `CODE` or padding.
5. The local slot index exists and does not point to a hidden package. Indexes for global values, types, and native exist; write permissions and object view are correct.
6. The `VA_PACK` and `VA_ARG` types are permissible for additional arguments.
   `REF_WRITE` receives `@mut T`, `REF_READ` receives `@T` or `@mut T`, and `TYPED_INIT` receives a type different from `Void`. `VA_FORWARD`, `VA_COUNT`, and `VA_ARG` can only be used in variadic functions.
7. Control flow analysis confirms the absence of underflow, exceeding `max_stack`, consistent depth at merges, and the presence of a result at `RETURN`. `VA_PACK` accounts for the immediate `count`. Every instruction must be reachable from a single entry point of the function; unreachable code is prohibited.
8. After `RETURN`, `JUMP`, or `TAIL_CALL`, control does not fall to the subsequent bytes.
   The last instruction of a function cannot be a regular instruction with fallthrough.
9. Verification does not rely on names, debugging information, or the compiler's assertion of correctness. All ranges and limits are checked before allocation.

A full check of dynamic types for each stack value is not included in the ISA 1.1 structural verifier. The VM maintains runtime checks for operations, bounds, indirect call signatures, types, and object references and membership to instances. The package is only accepted as a hidden argument for a variadic call; operations for storage and `RETURN` reject it, preventing it from leaving the call ABI, even with faulty bytecode.

## 12. Canonical MO and Compatibility

Canonical Compiler:

- Sorts the directory by section ID and places sections without spaces after the header and directory.
- does not create duplicate `STRINGS` entries or duplicate structural types;
- Sorts `FUNCTIONS` by code offset, and `RELOC` by `code_offset`.
- Initializes reserved and movable operands to zero.
- It preserves the order of declarations for the MIR format for global variables, types, and constants.
- It utilizes the shortest, yet correct, encoding for each entry.

Canonicalization is not a requirement for loading. The reader accepts any file that meets regulatory constraints, and never uses the file's byte order as a substitute for character resolution by name.


## MO/ISA 2.0 Extension: union

The overall structure of sections is preserved. A new type tag `0x26` contains `u8 tag`, `u32 variant_count`, and then `u32 variant_type_index[variant_count]`.
The count is 2–4096; variants are distinct and not unions themselves.
The loader canonicalizes complete structural types between modules before relocation and constant creation. Recursion through unions is prohibited.

New constant tags: `0x28` – wrapping and `0x29` – union expansion. Following the tag are three `u32`: the index of the target union type, the index of the payload type, and the index of a previously declared payload constant. For `0x28`, the payload type is a variant; for `0x29`, it is the original union with a subset of variants.

| Opcode | Name | Operands Bytes |
|---:|---|---|
| 86 | `UNION_NEW` | two `u32` type IDs |
| 87 | `UNION_VARIANTOF` | 0 |
| 88 | `UNION_GET` | `u32` type ID |
| 89 | `UNION_WIDEN` | `u32` type ID |
| 90 | `TYPED_CONSTRUCT` | `u32` type ID |
| 91 | `ARRAY_NEW_TYPED` | `u32` array type ID |

Opcode numbers 0–85 are preserved. Typical operands use previous `local/import` relocations. New instructions are prohibited in ISA 1 images. The loader checks the width of each instruction within its function and boundary of jump targets; the runtime repeatedly verifies the object's type, full type, and active payload. Old MO/ISA 1.0–1.1 continue to be read through a separate check of the header version.

## ISA 2.1 Extension: Local Superinstructions

| Opcode | Name | Operands Bytes | Action |
|---:|---|---|---|
| 92 | `INC_LOCAL` | `u16` local index | Increments the numeric local by 1 |
| 93 | `DEC_LOCAL` | `u16` local index | Decrements the numeric local index by 1 |

The instructions do not modify the operand stack depth and preserve the `INC`/`DEC` rules for all numerical types, including wrapping overflow. The index is checked in the same way as for `LOCAL_GET`/`LOCAL_SET`. The hidden variadic packet is unavailable. In images with ISA below 2.1, these opcodes are prohibited. The existing instructions’ numbers are preserved.

## ISA 2.2 Extension: BREAKPOINT

| Opcode | Name | Operands Bytes | Action |
|---:|---|---|---|
| 94 | `BREAKPOINT` | 0 | Pauses execution with `MVM_RESULT_BREAKPOINT` |

The instruction does not modify the operand stack, frames, or `working`. The PC points to the next instruction, so the executor can continue execution by using `mvm_run`/`mvm_update`. The presence of `DEBUG_LINES` is not required. In ISA images below opcode 94, instruction 94 is prohibited; existing instructions are preserved.

## ISA 2.4 Extension: JUMP_IF_LOCAL

| Opcode | Name | Operands Bytes | Action |
|---:|---|---|---|
| 95 | `JUMP_IF_LOCAL` | `left:u16, right:u16, comparison:u8, when_true:u8, address:u32` | Compares two local values and conditionally jumps. |

`comparison` contains the numeric value of an opcode from `EQ`–`GE` (37–42);
`when_true` is 0 or 1. The branch is taken when the comparison result equals
`when_true != 0`. The instruction leaves the operand stack unchanged and preserves
the semantics of the corresponding comparison + `JUMP_IF_TRUE`/`JUMP_IF_FALSE`
pair, including NaN behavior and value-validation errors.
Both indices are checked as for `LOCAL_GET`; the hidden variadic packet is inaccessible.
The target must be the start of an instruction in the same function. Opcode 95 is
forbidden in images with ISA versions below 2.4; existing instruction numbers are preserved.

## MO 3.0 / ISA 2.5 Extension: Explicit Nullability

MO 3 type descriptors use `TYPE_FLAG_NULLABLE = 0x2` for `T?`. Nullability is part
of canonical type identity, generic substitution, signatures, constants, and
native/host boundary validation. MO 1 and MO 2 descriptors keep legacy implicit
nullable semantics for reference-like types.

| Opcode | Name | Operands Bytes | Action |
|---:|---|---|---|
| 96 | `NULL_CHECK` | 0 | Traps if the top value is null; otherwise preserves it. |

`NULL_CHECK` implements checked conversion from `T?` to `T`. It does not change
stack depth or the representation of a present value. Opcode 96 is forbidden in
images with ISA versions below 2.5; existing instruction numbers are preserved.
