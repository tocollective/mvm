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

# 9. Errors and Warnings

[← Interaction with mvm](08-interop.md) · [Table of Contents](README.md) · [Grammar →](10-grammar.md)

Here's a summary of everything the compiler must reject (**error**) or about which it must report while continuing compilation (**warning**), and of what the mvm rejects when creating an instance. What stops a program during execution – [traps, 7.3](07-runtime.md#73-traps). Details – in sections on references.

## 9.1 Warnings

| What | Section |
|---|---|
| Local variable has never been read | [3.2](03-declarations.md#32-variables) |
| The inner function has never been used | [3.8](03-declarations.md#38-nested-functions-and-literals) |
| `let mut`, which has not been assigned to after declaration – would suffice with just `let` (including the function variable `let mut f(...)`) | [3.2](03-declarations.md#32-variables), [3.8](03-declarations.md#38-nested-functions-and-literals) |
| Reading `let mut x: T` without `=` until the first assignment | [3.2](03-declarations.md#32-variables) |
| A function not exported, a global variable, or a type or `enum` that is not used anywhere (prelude names – without warning) | [6.2](06-modules.md#62-export), [8.6](08-interop.md#86-prelude) |
| Imported name is not used | [6.3](06-modules.md#63-import) |
| Function result lost (including native or built-in) | [5.4](05-statements.md#54-calling-as-an-operator) |
| Unreachable code: after `return`, `break`, `continue`, `while true` without `break` | [5.10](05-statements.md#510-return) |
| `if`/`while`/`for` body without parentheses on a separate line with the same indentation as the statement itself | [5.5](05-statements.md#55-if--else) |
| `///` not before declaration | [1.2](01-lexical.md#12-comments) |
| A local variable, parameter, or nested function shadows a local name or parameter (not a top-level name | [3.7](03-declarations.md#37-scope-and-order) |

The compiler does not issue warnings for unused **parameters**: when using callbacks and handlers, the signature is defined externally.

## 9.2 Errors

### Lexicon

- Unclosed `/*`, character or string literal;
- Not a single Unicode code within a character literal; tabs or newline within the literal; unknown escaping; `\x` without two digits.
- Decimal number with a leading zero (`010`); `_` is not allowed between digits.
- The identifier matches a reserved word.
- A string literal after escaping contains an invalid UTF-8 sequence or `U+0000`.

### Types and Casts

- Incompatible operand types, considering the context of an integer literal, disabling write access and enabling transparent read of the reference (2.10);
- A literal or the result of a constant expression does not fit within the type.
- A literal without context specifying its type.
- The condition or logical operator is not `Bool`.
- signed index or signed shift value
- Ordering references, strings, arrays, functions, and `Type`; comparing structs.
- `null` when used in the context of `Type` or comparing `Type` with `null`.
- Using the value of `Type` instead of a syntax type or type name as a value without `typeof` (2.14);
- Converting a descriptor (link, array, string, function, `Thread`, or `Type`) into a number and back; the numerical content of the descriptor is read according to standard rules.
- Changing the type of the content within a link, or obtaining `@mut T` from `@T`.
- `as`, which is not defined in rule 4.6.
- `%` and bitwise operations on `Float` and `Double`.
- `Void` outside of the result or argument type `typeof(Void)`;
  `@Void`, `@mut Void`, an array of `Void` elements;
- `@expression` with an operand of `Void` or an undefined type;
- Indexing a reference whose content is not an array or a string.
- `mut T[]` outside of the parameter list, or `mut` preceding the parameter of another type.
- Non-empty `[n]` within the type of a field, function parameter, or result, in a pseudonym,
  within `typeof`/`sizeof`/`alignof`/`as`, or not in the last suffix of the declaration.
- Declaration of `T[n]` without `=` with an array literal; signed initial length.
- A non-empty suffix following an ungrouped function type in a function array declaration.
- A constant initial length of less than `0..4_294_967_295` inclusive, or less than the number of literal elements;
- A structure with no fields, containing repeating fields, or containing itself by reference; a pseudonym referring to itself; `packed` for the pseudonym.
- Invalid value, repetition, or unspecified element name within an `enum` (2.7);
- `null` without an explicit type among additional variable arguments.
- A generic type or function without required type arguments, incorrect number of arguments, a value argument instead of a syntax type.
- Unsupported generic declarations (`enum`, `extern`, global, `let mut`-function, nested function, or `main`).
- Operation not allowed after substituting specific types.
- Recursive processing of generic structures by value, or the generation of new generic instances without completion (2.15).

### Mutability

- Writing to a `let`-variable, its field or element; parameter replacement.
- Changing the content via `@T` instead of `@mut T`.
- Modification of array elements or length via an immutable path.
- Passing an immutable path to the `mut T[]` parameter.
- writing to a string element
- Using the unary `&` or unary `*`: The operators `&` and `*` are exclusively binary operators. A new object is created as `@value`.
- Replacing the underlying link within an immutable storage location.
- Accessing a non-existent field in a struct, including through a reference.

`@local`, `@record.field` and `@items[i]` are valid: they create objects with copies of values, without modifying the original locations.

### Declarations

- `let x = ...` without a type.
- `let x: T` without `=` and without `mut`.
- Global variable with a non-constant initializer (function call,
reference to `let mut`).
- Loop within global variable initializers.
- Two identical top-level names within a file (considering imports).
- Two identical names within the same block; a local variable with the same name as its parameter within the function’s body.
- A function with a result can reach `}` without a `return`.
- `return` with an expression that is not `Void` in a `Void`-function, or without an expression in a function with a non-`Void` return value.
- Declaring a variable or function – using a `if`/`while`/`for` block body without a block.
- `let` (variable or function) directly within a `case` without a block.
- Accessing a local variable or parameter of an outer function, including a function variable, from within a nested function or literal.
  `let mut` from within its own body ([3.8](03-declarations.md#38-nested-functions-and-literals).
- Calling a nested function before its declaration.
- Assignment of an inner function (without `mut`).
- `extern` is not for functions (`extern let x: T`, `extern let mut ...`).
- Structures by value in parameters or results
  - `extern` functions or functions exported by the root file
- Two `extern` declarations with the same name but different types within a single program.

### Operators

- An expression that is not a call, such as an operator (`x + 1`).
- Assignment or `++` within an expression (`a = b = 0`, `buf[i++]`);
- `break`/`continue` outside of a loop (`continue` in a `switch` outside of a loop).
- `for`: the step is not a constant, or is `0` or does not fit within the type.
- `for in`: the source is neither an array nor a string, the type of the element does not match the type of the variable, or the index has a type that is not `ULong`.
- `switch`: label is not a constant, repeating label, two `default`.
- comparison chain `a < b < c`
- A literal function without parentheses in an `if`/`while`/`for` condition or in a `switch` expression.

### Modules

- The file in `from` was not found.
- Import a name that the file does not export.
- Exporting a name that is not present in the imported file, or in a nested function;
- Two exports with a single external name.

### Built-in Functions

- The argument is not the correct type (`length` is not a string or an array, `vaCount` is not with a variable argument package, `vaArg` with a signed index).
- `sizeof`/`alignof` with `Void`, using untyped or untyped arguments: `args: ...`
- `typeof` without arguments, with multiple arguments, with an operand of an undefined type (`1`, `null`, `[]`, a literal struct without context), or with the `args: ...` package; typical name and type errors within an unevaluated operand remain errors (7.4).
- The `length` of a value that is not a string/array or a reference to a string/array.
- `arrayCapacity`, `arrayReserve`, `arrayResize`, `arrayPush`, `arrayPop`,
  `arrayInsert`, `arrayRemove` with a value that is not an array.
- Signed length/capacity/index operations on arrays,
  where the added value is not compatible with the type `T`.
- Using the `length(a)` of an array as a constant.

- The value of the built-in function (`let f: ... = vaCount`); is declared with a reserved name.

Calls to library functions like `clz`, `bswap`, and other native functions are checked using their standard `extern` declarations, without any special compiler rules.

### Image Building

- `mvm run`: There is no `main` function in the root file, or it has a different signature.

## 9.3 Instance Creation Errors

mvm reports this, not the compiler: the image has been built, but the executor cannot run it ([8.1](08-interop.md#81-from-source-to-call)).

- For the `extern`-function, the executor did not provide a function with that name.
- There is a native function, but the types of parameters, results, or variability, as well as access rights, do not match ([8.2](08-interop.md#82-types-at-the-boundary)).
- Segments, the data stack, and global literals require more memory than the limit allows, or their allocation failed.
- The image did not pass validation or was compiled into an incompatible mvm build.
  ([7.1](07-runtime.md#71-image-and-instance).

## 9.4 Condition in Parentheses

Compiler messages – in English. The most subtle error –
a condition that starts with a parenthesis
([5.5](05-statements.md#55-if--else)). The message should suggest a fix:

```
if.m:12: error: the 'if' condition is '(a + b)', and '* c > 0 ...' is not a statement
    if the whole expression is the condition, put it in parentheses:
        if ((a + b) * c > 0) ...
```


Union errors include ambiguous selection, incompatible values,
extracting a type outside of the available options, narrowing a union, a missing initializer, duplicate descriptors in `case`, recursive layout, and exiting a union through native/host ABI. An error after generic substitution results in a chain of instantiations. Extracting a valid, but inactive, variant results in a runtime error; however, the original value is preserved.
