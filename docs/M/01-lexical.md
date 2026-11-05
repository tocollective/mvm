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

# 1. Lexical Analysis

[← Table of Contents](README.md) · [Types →](02-types.md)

## 1.1 Original Text

- The program file on M – text in UTF-8 with the `.m` extension.
- Characters outside of ASCII are permitted in comments, string, and character literals (1.6). In identifiers, only ASCII is allowed.
- Whitespace characters: space, tab, `CR`, `LF`. They are only used to separate tokens. Indentation and newline characters are meaningless: any construct can be moved to a different line, and tabs and spaces can be mixed.
- There are no semicolons. The end of an operator defines the grammar
  ([5.1](05-statements.md#51-operators-and-their-limits)).

## 1.2 Comments

| Record | What is this? |
|---|---|
| `// ...` | to the end of the line |
| `/* ... */` | Block-style; **nested**: `/* a /* b */ c */` – a single comment |
| `/// ...` | Doc-comment until the end of the line |

- Block comments can be placed anywhere a space is allowed, including within an expression: `greet(/* name */ "M")`. An unclosed `/*` is an error.
- The documentation comment describes a declaration that immediately follows it:

*   a function, `type`, `enum`, struct field, `enum` element, a global variable, `extern`. The compiler saves the documentation comments for exported declarations from the root file within the image; the executor can display them to the user ([8.4](08-interop.md#84-calling-m-functions)). Multiple consecutive `///` lines constitute a single comment. A `///` line elsewhere indicates a warning.
- Multiple `////` characters represent a standard comment.

## 1.3 Identifiers

```
identifier = (letter | "_") { letter | digit | "_" }
letter     = "A"…"Z" | "a"…"z"
digit      = "0"…"9"
```

- The register differs: `color` and `Color` are different names.
- The identifier cannot match a keyword, an identifier of an embedded type, or an embedded function, or an identifier matching a keyword. (1.4), an identifier of an embedded type (1.4), or an embedded function ([7.4](07-runtime.md#74-built-in-functions)).
- Conventions (not rules of the language): types — `PascalCase`, functions and variables — `camelCase`, constants — `UPPER_CASE`.

## 1.4 Reserved Words

Keywords:

```
as       break    by       case     continue default  else
enum     export   extern   false    for      from     if       import
in       let      mut      null     packed   return   switch   true
type     while
```

Built-in type names (also reserved):

```
Byte  UByte  Half  UHalf  Word  UWord  Long  ULong
Bool  Float  Double  String  Thread  Type  Void
```

Built-in function names: `typeof`, `variantof`, `sizeof`, `alignof`, `offsetof`, `length`,
`arrayCapacity`, `arrayReserve`, `arrayResize`, `arrayPush`, `arrayPop`, `arrayInsert`,
`arrayRemove`, `vaCount`, `vaArg`, `breakpoint`.

Ordinary function names, including functions from the
[native library](../native-library.md), follow the identifier rules in 1.3.

## 1.5 Numerical Literals

### Integers

| Record | Example |
|---|---|
| Decimal | `0`, `42`, `1_000_000` |
| hexadecimal | `0xFF`, `0xfd00_2000` |
| binary | `0b1010_1010` |

- Prefixes are written in lowercase: `0x`, `0b`. Hexadecimal digits – in any case.
- The underscore `_` can only be used **between digits**: `0xFD00_2000` – yes, `0x_FF`, `1_`, `1__0` – error.
- There are no octal literals. A decimal number with a leading zero (`010`) is an error, to prevent it from being interpreted as an octal number from C. However, `0` itself is allowed.
- There is no sign on the literal: `-1` is a unary minus and the literal `1`
  ([4.5](04-expressions.md#45-unary-operators)).
- The context determines an integer literal's type
  ([4.2](04-expressions.md#42-literals-and-constant-expressions)).

### With Floating-Point

```
float_literal = digits "." digits [ "f" ]
```

- Digits are required **on both sides** of the decimal point: `1.5`, `0.5f`, `2.0`.
  `1.`, `.5`, `1e10`, and `1.f` are not literals.
- The suffix `f` is written in lowercase and specifies the `Float` type: `1.5f`. A literal without the suffix has the `Double` type: `1.5`. This type does not change within the context ([4.2](04-expressions.md#42-literals-and-constant-expressions)).

A decimal point following a number indicates a fractional part, only if followed by a digit. Therefore, `0..argc` is `0`, `..`, and `argc`, not `0.` and `.argc`.

## 1.6 Symbolic and String Literals

- **Symbol** – exactly one Unicode code. ASCII and escaped bytes, such as `'a'` and `'\xE9'`, have the type `UByte`. Symbols outside of ASCII, written directly, have the type `UWord`: `'д'` – `0x0434`, `'é'` – `0x00E9`, `'😀'` – `0x1F600`. The value is the character code, not UTF-8 bytes.
- **String** `"..."` has its own type `String`.
  ([2.12](02-types.md#212-strings)). Its content is immutable text in UTF-8; assignment copies the string reference.
- The length and index of a string are measured in Unicode code points:
  For `"café"`, the length is 4, and the element at index 3 is `0xE9` of type `UWord`.
  The representation of this text in UTF-8 takes 5 bytes.
- A string literal cannot contain multiple Unicode code points:
  `'ab'`, `'é'` (letter and combining accent), `'👩‍💻'` – these are errors.
- `\` and the quote that closes a literal must be escaped. Tabs and ASCII
  control characters inside a literal are errors; use escape sequences.
- A string can span multiple lines in the source code: each translation
  of a string within quotes (`LF` or `CRLF`) becomes one byte `0x0A`.
  Indentation of subsequent lines is included as is. A newline character in a literal string is an error.

  ```m
  let text: String = "1
  2
  3
  "   // "1\n2\n3\n"
  ```
- `\xNN` within a string inserts a UTF-8 byte. After processing any escapes, the entire text must be valid UTF-8, without `U+0000`. Otherwise, an error occurs. For example, `"\xC3\xA9"` is the string `"é"`, and `"\xFF"` is an error. For arbitrary bytes, use `UByte` arrays.
- In the symbolic literal `\xNN`, a value of `UByte` is specified, so `'\xFF'` is valid. `\0` and `\x00` are only valid in symbolic literals. Import paths follow string literal rules.

| Encoding | Byte |
|---|---|
| `\n` | `0x0A` |
| `\r` | `0x0D` |
| `\t` | `0x09` |
| `\0` | `0x00` |
| `\\` | `0x5C` |
| `\'` | `0x27` |
| `\"` | `0x22` |
| `\xNN` | `0xNN`, exactly two hexadecimal digits |

Other sequences (`\a`, `\u…`, `\x1`) are an error.

## 1.7 Miscellaneous Literals

- `true`, `false` are values of type `Bool`.
- `null` – absence of a value in an explicitly nullable type such as `@T?`, `@mut T?`, `T[]?`, `String?`, or a nullable function type; the specific type determines the context. Ordinary reference-like types are non-null by default.

## 1.8 Operators and Separators

```
+   -   *   /   %
&   |   ^   ~   <<  >>
==  !=  <   <=  >   >=
&&  ||  !  ?
=   +=  -=  *=  /=  %=  &=  |=  ^=  <<=  >>=
++  --  @
.   ..  ...
,   :   ::  (   )   [   ]   {   }
```

- The lexer takes the longest suitable token: `...` before `..`, `..` before `.`, `<<=` before `<<`.
- The `@mut` within a reference type consists of two tokens, `@` and `mut`. In the expression `@value`, a new object is created; `&` is used only as a binary bitwise operator.
