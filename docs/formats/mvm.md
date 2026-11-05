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

# mvm – Program ZIP Container

[Format Overview](README.md) · [MIR](mir.md) · [MO](mo.md)

Status: mvm Container 1 draft. `.mvm` is an executable program image, rather than a separate module format or a source archive.

## 1. Composition

The file `.mvm` is a standard ZIP archive. Required entries:

```text
manifest.toml
modules/main.mo
modules/lib.mo
...
```

Physical paths `.mo` are not module names. A logical name is stored in the `MODULE` section of each MO, and it is this name that participates in importing. The recommended scheme `modules/*.mo` does not restrict the linker: any safe ZIP paths with the `.mo` suffix are permitted.

The archive is self-contained: all inter-module imports are resolved within it.
External `.mo` files, source code lookup, and program directory reading during loading are not used. The executor provides native functions; shareable libraries are not placed within `.mvm` version 1, and are not automatically loaded.

## 2. ZIP Limitations

Version 1 loader accepts:

- Compression methods `0` (Stored) and `8` (Deflate);
- A standard ZIP archive with a single central directory.
- ZIP64, if the implementation supports it and the resulting sizes meet the limits;
- Directories and data descriptors, if precise sizes and CRC values are available from the central directory.

Loader rejects:

- Encryption, multi-volume archives, and unsupported compression methods.
- Duplicate full record names, including duplicate manifest;
- Absolute paths, `\`, NUL, empty components, components `.` and `..`, Windows drive/UNC paths;
- Symbolic links and special files based on central directory attributes.
- Mismatch between actual size/CRC and the central directory.
- truncated archives or data outside the declared ranges;
- More than one logical name for a module, even if the physical paths are different.

The name of each entry must be a valid UTF-8 string, regardless of the ZIP language flag. Names are compared byte-by-byte and are case-sensitive. The implementation is not required to perform Unicode normalization, but the archiver is recommended to use NFC.

The loader must have configurable limits on the number of records, manifest size, size of each unpacked MO, and the total unpacked size. Default limits for version 1:

| Resource | Limit |
|---|---:|
| ZIP records | 4096 |
| `manifest.toml` | 64 KiB |
| One unpacked `.mo` file | 256 MiB |
| All unpacked `.mo` files | 1 GiB |

Overflowing the limit or arithmetic size overflow – this is a loading error.
It is not possible to allocate memory based on an unverified size from the ZIP header. CRC is not a protection against malicious archives.

Any safe non-directory entry except `manifest.toml` and `.mo` modules can be a
resource. An entry `path/to/name` is available to M as `mvm://path/to/name`
through the standard `file*` API. Lookup uses the complete path exactly; no
`resources/` prefix is added or removed. Resource handles support reading, line
reading, seek/tell, and existence checks; mutation is rejected as read-only.
Resource paths use the same safety rules as other ZIP entries. Per-resource and
total unpacked limits are 256 MiB and 1 GiB, with at most 4096 ZIP records in
total.

## 3. Manifest

`manifest.toml` is a TOML 1.0 document in UTF-8 without BOM, with a maximum size of 64 KiB.
It resides directly in the root of the archive. Only root keys are permitted; tables, arrays of tables, and dotted keys are not used in version 1.
Comments and blank lines are handled according to TOML rules. Duplicate keys and invalid TOML are considered errors.

Required keys have string values:

| Key | Value |
|---|---|
| `MVM-Format-Version` | `"1.0"` |
| `MVM-ISA-Version` | `"2.5"` |
| `Entry-Module` | Logical name of the root module |
| `Main-Export` | External name of the entry point, typically `"main"` |

Example:

```toml
MVM-Format-Version = "1.0"
MVM-ISA-Version = "2.5"
Entry-Module = "app/main.m"
Main-Export = "main"
Created-By = "mvm 0.1"
```

`Created-By` is optional, has a string value, and does not affect execution.
Unknown root keys with the `X-` prefix are ignored after TOML validation.
All other unknown keys are rejected, so a new mandatory setting cannot be
silently ignored by an older VM.

Major/minor versions are compared numerically, not as strings. An unsupported major version of a container or ISA is an error. Minor above the supported limit is only permissible if there are no unknown required data.

## 4. Root Module and Exports

Exactly one MO must have the `ROOT` flag, and its logical name must match `Entry-Module`. The `Main-Export` export must exist in the root module, be a function, and have the `HOST` flag.

The root module's `HOST` exports form the image API for the executor. Ordinary
exports from other modules are available only for internal linking. Setting
`HOST` in a non-root module is forbidden.

`mvm run` accepts an entry point from one of the following signatures:

```text
(): Word
(args: String[]): Word
```

Additional integer result types may be accepted by the embedding API,
but the CLI contract version 1 uses `Word`. The result becomes the exit code of the process according to the rules of the execution environment. Through the C API, it is possible to call other `HOST`-functions with valid boundary types.

## 5. Loading and internal linking

The loader performs the steps atomically relative to the instance: if any error occurs, the partially created instance is released, and the M code does not start.

1. **ZIP.** Read the central directory, verify paths, limits, sizes, and CRC.
2. **Manifest.** Verify versions, required keys, and absence of duplicates.
3. **MO.** Unpack each `.mo` file with limits and thoroughly check its container, sections, ISA, types, and tables. Logical module names must be unique.
4. **Module graph.** Resolve each import by the pair `(module, export name)`.
   The kind and complete type must match. Cyclic imports are allowed. Self-import
   does not create a new module instance.
5. **Native Imports.** Collect them by name. Identical names must have a single signature throughout the entire archive. The executor either binds each function or the loading fails.  Unnecessary functions are not an error.
6. **Index assignment.** Assign each local and imported global value a unique
   instance index, each type a type ID, and each native function a native ID.
   An import is an alias of the exported object, not a copy.
7. **Memory Layout.** Place the `CODE` and `RODATA` sections of all modules within a single 32-bit address space. The base address of each module’s code section has an alignment of 1, while the base address of the `rodata` section is 1. Additional alignment within sections is already reflected in the offsets. Gaps are filled with zeros, but they are not valid targets for calls or jumps.
8. **Relocations.** Apply `RELOC` entries, writing little-endian addresses and indices to operands. Check for overflow in `uint32_t`/`uint16_t`, RAM boundaries, and configurable memory limits.
9. **ISA Verification.** After relocations, verify function and instruction boundaries, target addresses of jumps and calls, signatures of direct calls, string indices, and stack depth analysis according to MO rules.
10. **Initialization.** Create global values and constant graph objects without executing M. Each instance receives its own mutable slots and its own template objects. The immutable MO/mvm image remains unchanged.
11. **Execution.** Set the initial PC to address `Main-Export` or prepare the call via the embedding API. Only after this is allowed to execute bytecode.

The physical placement of modules should not alter the language behavior.

For reproducible addresses, the canonical loader orders modules by logical name, placing the root module first, and maintaining the order of MO tables within modules.

## 6. Resolving Imports

For an MO import of a function, global value, or type:

- The `module` must precisely match the logical name of a single MO.
- `name` should be present among its usual or `HOST`-exports.
- The object types must match.
- Full types and signatures must be equivalent after resolving references to imported types.
- The global value maintains a mutability flag: imports cannot expand write access.
- Exporting through a chain of re-exports in MO is not supported. Multiple aliases can point to a single object, but each export explicitly specifies a local index.

Native import has the form `NATIVE_FUNCTION` and an empty module name. It is not searched for in ZIP and cannot be called by numeric ID until linked. If the implementation has not yet been registered, the mvm creates a deferred binding: it can be resolved via `libraryLoad`, but calling it before resolution results in a runtime error.

A missing module or name, a type mismatch, or an ambiguous export is a linking
error. mvm does not choose the first matching candidate.

## 7. Instances and Threads

One `.mvm` file can be loaded once and used to create independent instances.

The code, `rodata`, type tables, and related function descriptors can only be shared as immutable data. Global values, the stack, GC-objects, and execution state are all specific to each instance.

The worker from `std/thread.m` receives a new instance of the same image. The address `FUNC` is valid only within the address space of the corresponding image; references, arrays, strings, and handles between instances are not relocated.

## 8. Loading Errors

Diagnosis should differentiate, at a minimum:

- `container`: corrupted/unsupported ZIP file or manifest.
- `format`: structural violation of MO.
- `version`: Unsupported major version of the container or ISA.
- `link`: unresolved/incompatible import, duplicate module or export.
- `verify`: invalid bytecode, relocation, address, or stack.
- `native`: an already registered function has a different signature, or a deferred binding could not be created;
- `resource`: The limit for ZIP, RAM, types, global variables, or allocation has been exceeded.
- `entry`: Invalid root module or entry point.

The error includes the physical path of the ZIP archive, the logical module, and, if possible, a section/function/offset. After the error, the VM does not execute even already checked modules within the archive.

## 9. Compatibility and Reproducibility

A new implementation should read all supported major versions regardless of the order of ZIP records. It does not rely on the timestamp, archive comment, extra fields, order of the central directory, or a specific Deflate stream.

For reproducible builds, the packer is recommended to:

- Sort records by physical path.
- Set the timestamp to `1980-01-01 00:00:00` and identical attributes.
- Remove comments and non-essential extra fields.
- generate canonical MOs
- Use the same compressor and parameters if a byte-by-byte equality of archives is required.

Digital signatures and automatic loading of dynamic modules or native libraries do not belong to mvm Container 1 and require a separate specification extension. Read-only resource entries at arbitrary safe paths are supported as described above; native `.mvm` resources are not automatically unpacked or loaded as dynamic libraries.

The current loader supports ISA 1.0–1.1 and 2.0–2.5. The Union requires ISA 2.0;
`INC_LOCAL`/`DEC_LOCAL` require ISA 2.1, `BREAKPOINT` – ISA 2.2, and the types
`MUTEX`, `CONDITION`, and `ATOMIC` – ISA 2.3, `JUMP_IF_LOCAL` – ISA 2.4, and explicit nullability plus `NULL_CHECK` – ISA 2.5. Images before ISA 2.5 retain legacy implicit-nullability semantics.
The ZIP container version remains 1.0.
