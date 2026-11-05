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

# 6. Modules

[← Operators](05-statements.md) · [Table of Contents](README.md) · [Runtime Environment →](07-runtime.md)

## 6.1 File – Module

- Each `.m` file is a separate module with its own namespace.
- **Everything declared in a file is local by default.** Only names listed
  in `export` are visible outside it.
- There are no packages, namespaces, or nested modules.
- **The program** is the root file (the one passed to the compiler) and all files it imports, either directly or indirectly. The compiler compiles the entire program into a single mvm **image** ([7.1](07-runtime.md#71-image-and-instance)). There is no separate linking process: everything found by the linker is found by the compiler.

## 6.2 `export`

```
export { colorRed, colorGreen }
export { Color }
export { colorMix as mix }
```

- **Only explicit exports, by name only.** There are no “all at once” exports.
- `export { }` can be written **anywhere in the file and as many times as needed**: at the beginning as a table of contents, next to a declaration, or at the end.
- Functions, global variables (including `let mut f(...)` variables), structs, aliases, `enum`s, and `extern` declarations can be exported. Local variables and nested functions cannot.
- **`as`** exports under a different name: importing sees `mix`, not `colorMix`. One name can be exported under multiple names.
- Errors: exporting a name that is not present in the file; two exports with the same external name; exporting an imported name (no re-exporting).

### Who can see the export

| File | Visibility |
|---|---|
| root | The executor is a program API ([8.4](08-interop.md#84-calling-m-functions)); other files – via `import`, as is customary. |
| remaining | only accessible through `import` for program files; the executor does not see them |

- **The root file's `main` is exported automatically**; `export { main }` is unnecessary (but is not an error).
- The function exported by the root file – a function at the boundary with the executor – has parameters and results that are only types from [8.2](08-interop.md#82-types-at-the-boundary). Structures by value – an error.
- Exporting types, aliases, and `enum`s from the root file is enabled, but it does not create named exports for the executor. Metadata for `typeof` is included in the image; to pass an executor descriptor, either the `Type` variable or a function with a `Type` return type is exported ([2.14](02-types.md#214-type-descriptors)).
- Generic structs, aliases, and functions are exported as templates for import by other M-modules. An open template does not become a host export: the executor provides a regular, specific wrapper function. Specific instances, shared across multiple modules, are linked using MIR import/export mechanisms and maintain a unique identity (2.15).

### `extern` and Export

```
// native.m
extern let log(msg: String): Void
extern let now(): ULong
export { log, now }
```

The `extern`-declaration exports a **declaration**: the file that imports `log` can call it, while the implementation is provided by the executor. This means native function declarations are gathered in a single file. This does not make `log` an export of the image: the executor cannot call its own function through the program.

## 6.3 `import`

```
import { log } from "native.m"
import { Color, colorRed, mix } from "color.m"
import { colorGreen as green } from "color.m"
```

- **Import only by name**, without `import *`.
- The same file can be imported multiple times.
- **`as`** changes the name only within this file. The name of the `extern`-function, as well as the export name of the root file, remain unchanged.
- An imported name becomes a top-level name in the file. A collision with
  another top-level name (declared here or imported) is an error.
  A name from the prelude is hidden without an error
  ([8.6](08-interop.md#86-prelude)).
- `import` statements can be written anywhere at the top level, but are typically placed at the beginning.
- Errors: File not found; name not exported from this file.
- Unused imported name – a warning.
- Cyclic import (`a.m` imports `b.m`, `b.m` imports `a.m`)
  is allowed: the order of declarations does not matter.
  ([3.7](03-declarations.md#37-scope-and-order)).

### Where to Search for the File

1. Relative to the directory of the **current** file: `"color.m"`,
   `"../native.m"`, `"lib/text.m"`.
2. Then, sequentially through the directories passed to the compiler via the `-I` flag, such as `-I` for `#include` in `cc`.

   ```
   mvm build -I /opt/m/lib main.m -o main.mvm
   ```

   ```
   import { popcount } from "std/bits.m"   // It will be found at /opt/m/lib/std/bits.m
   ```

The first file found is taken.

The executor, which compiles the program itself, can replace file system searching with its loader: in this case, “directory” and “file” refer to what the loader understands, such as entries in an archive or strings in memory ([8.1](08-interop.md#81-from-source-to-call)). Rules 1 and 2 are maintained: a relative path is considered relative to the path of the current file.

## 6.4 Program and Image

What does each declaration receive within an image?

| Declaration | In the image |
|---|---|
| Root file export: function | Export image under an external name; the executor calls it by name |
| Export of the root file: global variable | Export of the image: the executor receives its name, type, and write permissions ([8.5](08-interop.md#85-accessing-objects-and-values)) |
| `main` of the root file | Export `main` |
| Exporting remaining files, non-exported functions, and variables | Internal: The executor has no access to them by name |
|nested function, function literal, body of a variable function | internal |
| `extern`-declaration | Import image: name and parameters/result types; the executor must provide it |
| type, alias, `enum` | nothing |

- The names of exported images are unique: they are derived from a single root file, which contains two exports with the same external name – an error (6.2).
- Internal functions and variables with the same names in different files do not conflict.
- The names of internal functions remain in the debug information of the image:
  The mvm displays them in the trap tracing
  ([7.3](07-runtime.md#73-traps)).
