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

# mvm formats

The current compiler generates **MIR 4, MO 3.0, and ISA 2.5**. The ZIP container
remains mvm 1.0. The loader also reads MO/ISA 1.0–1.1 and ISA 2.0–2.4 with legacy
implicit-nullability semantics; the assembler accepts MIR 2 and MIR 3 for the earlier
instruction and type subsets. Union extensions are described in
[UNION.md](../../.hidden/UNION.md). Other sections include target language contracts.

## Pipeline

```text
main.m ──► main.mir ──► main.mo ──┐
lib.m  ──► lib.mir  ──► lib.mo  ──┼──► program.mvm ──► mvm
                                  │       ZIP             loading,
                           manifest.toml                   linking,
                                                          execution
```

| Format | Purpose | Specification |
|---|---|---|
| `.mir` | textual intermediate representation of one source module | [MIR](mir.md) |
| `.mo` | portable binary object with code, metadata, and relocations | [MO](mo.md) |
| `.mvm` | ZIP container for a complete program consisting of one or more `.mo` files | [mvm](mvm.md) |

One `.m` source file produces one logical module. The intermediate `.mir`
can be read, saved, and compiled separately; `.mo` cannot run
by itself because its addresses and indices have not yet been linked. `.mvm`
contains the entire program and requires neither source files nor an external linker.

When loading `.mvm`, mvm performs internal static linking:
it resolves module imports, assigns addresses and indices, applies relocations,
validates the resulting bytecode, and binds `extern` functions to the executor.
This is not a separate user-facing linking stage or dynamic loading
of modules at runtime.

## Normative terms

The words **must**, **must not**, and **shall** specify mandatory requirements.
**May** denotes a permitted option. **Recommended** does not affect
compatibility. If an implementation cannot accept a document within its configured
resource limit, it reports a loading error rather than executing part of the image.

## General versioning rules

- Textual MIR has an independent representation-language version.
- MO has a container version and an instruction-set version (**ISA**).
- mvm has a container version; the required ISA version is specified in the manifest.
- An incompatible change increments the major version. An implementation rejects
  unsupported major versions.
- A minor version may add only optional data. An unknown
  required section, opcode, type kind, or relocation kind is always rejected.
- Numbers in binary formats use little-endian encoding. Numeric types have
  fixed widths regardless of platform.
- Module, export, and import names are UTF-8 strings. Comparison is
  bytewise, without implicit Unicode normalization and without case sensitivity.

## Responsibilities

The M frontend is responsible for language type checking and produces valid MIR. The MIR assembler
is responsible for MO structure, instruction encoding, metadata, and calculation of
stack depth. The mvm loader must revalidate all data from the file:
a correct compiler is not a security prerequisite.

ZIP CRC protects only against accidental corruption. Neither `.mo` nor `.mvm` in
version 1 contains a digital signature or proves the origin of the code.

## License

Copyright 2026 tocollective contributors.

Licensed under the Apache License, Version 2.0. See [LICENSE](../../LICENSE) for the full license text.

Unless required by applicable law or agreed to in writing, software distributed
under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied. See the License for the
specific language governing permissions and limitations under the License.
