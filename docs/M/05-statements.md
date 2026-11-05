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

# 5. Statements

[← Expressions](04-expressions.md) · [Table of Contents](README.md) · [Modules →](06-modules.md)

## 5.1 Operators and Their Limits

The function body is a sequence of operators:

| Operator | Example |
|---|---|
| Variable declaration | `let mut i: UWord = 0` |
| Function declaration | `let twice(x: Word): Word { ... }`, `let mut f(): Void { ... }` ([3.8](03-declarations.md#38-nested-functions-and-literals)) |
| Assignment | `x = 1`, `r = 0`, `a[i] = v` |
| Compound Assignment | `i += 2`, `flags \|= MASK` |
| `++`, `--` | `i++` |
| Call | `puts("hi")` |
| block | `{ ... }` |
| `if` / `else` | 5.5 |
| `while` | 5.6 |
| `for` | 5.7 |
| `switch` | 5.9 |
| `break`, `continue` | 5.8 |
| `return` | 5.10 |

Semicolons are not used, and newlines have no meaning. The operator ends where its grammar can no longer continue.

Therefore, `x = a` and the following line `-b` is equivalent to `x = a - b`. The operator, which begins with `-`, `(`, or `[`, requires an expression without parentheses to be separated by a block or parentheses (5.5).

Other expressions used as operators – an error: `x + 1` or `a == b` on their own lines do nothing.

## 5.2 Block

`{ statements }` – a new scope.
([3.7](03-declarations.md#37-scope-and-order)). An empty block `{}` is allowed.

## 5.3 Assignment

```
location = expression
target op= expression     // op: + - * / % & | ^ << >>
location++
location--
```

- **These are statements, not expressions:** they have no value. `a = b = 0`,
  `let j: UWord = i++`, `buf[i++] = 0`, and `if x = 0` are errors.
- `target` is a mutable variable, its field, or an element; or a value,
  field, or element accessible through `@mut T`
  ([4.4](04-expressions.md#44-postfix-operators)).
- The right type matches the target type, considering allowed transformations.

For a reference, compatibility with the reference itself is checked, without implicit evaluation of the right-hand side: this is a reassignment requiring mutable storage for the reference. Otherwise, direct compatibility with its content `T` is checked, which is writing to an object via `@mut T`.

For a variable, reassignment requires `let mut`; this prohibition does not include additional evaluation of the right-hand side and does not change the selected case.

For example, `r = @10` replaces the reference, while `r = 10` changes the number (2.4).
- `x op= y` is equivalent to `x = x op y`, but the `x` is calculated only once. For `<<=` and `>>=`, the right-hand side is any unsigned type, as with shifts.
- `x++`, `x--` mean `x += 1`, `x -= 1`, with wrapping. Only the postfix form
  exists. They are available through `@mut T` when `T` is an integer type.
  They are unavailable for `Float` and `enum`; the reference descriptor itself is unchanged.
- The structure can be assigned entirely: `dst = src as Color`, `c = {}`.
  Here, `dst` and `src` are references to `Color`; explicit reading `src as Color` selects the value, preserving the reference `dst`. Simply `dst = src` reassigns the reference and requires a mutable binding for `dst`. Assigning an array copies the reference. `a = []` creates a new empty array, while `arrayResize(a, 0)` clears the existing object for all its aliases.

## 5.4 Calling as an Operator

`f(args)` on its own line. If the function returns something, and that result is lost – **warning**: there’s no point in returning unnecessary results. There’s no specific mechanism to output the result.

```
extern let write(text: String): Long
let written: Long = write("hi\n")
if written < 0 return written
```

The rule applies to built-in functions as well: `vaCount(args)` without using the result—a warning. The same rule applies to native functions as it does to functions on M.

## 5.5 `if` / `else`

```
if condition body
if condition body else body
```

- **Condition — only `Bool`:** `if p != null`, `if n != 0`. `if p` and `if n` are errors.
- **The body is a block `{ }` or exactly one statement.** `else if` is equivalent to `else`,
  where the body becomes another `if`.

  ```
  if n < 0 return 0
  if n < 0 puts("-") else puts("+")
  if a {
      ...
  } else if b {
      ...
  } else {
      ...
  }
  ```

- **Body-declaration** (`if c let x: UWord = 1`,
  `if c let f(): Void { }`) – error: the name would immediately go out of scope.

### Where Does the Condition End?

1. **If `if` is immediately followed by `(`**, the condition is exactly the
   expression inside those parentheses, as in C. The body starts after `)`:

   ```
   if (p != null) *p = 0
   ```

   Therefore, a condition that only **starts** with a parenthesis must be
   parenthesized as a whole: `if (a + b) * c > 0 { }` is an error (condition:
   `(a + b)`, body: `* c > 0`). Use `if ((a + b) * c > 0) { }` or
   `if c * (a + b) > 0 { }`. The compiler explains this in its error message.
2. **Otherwise, the condition is the longest expression** following `if`. It ends at the first token that the expression cannot continue from; the body begins at this token. `if x -y` is equivalent to `if (x - y)`, not `if x { -y }`.

The body, which begins with `-`, `*`, `&`, `(`, or `[`, must be written within a block or enclosed in parentheses (Rule 1):

```
if p != null *p = 0        // Error: p != (null * p) = 0
if (p != null) *p = 0      // correct
if p != null { *p = 0 }    // or this
```

### Dangling `else`

`else` refers to the nearest `if` without an `else`. To attach it to an outer `if`, the inner one is taken into scope.

```
if a
    if b f() else g()       // else — from if b

if a {
    if b f()
} else {
    g()                     // else — as opposed to if a
}
```

The compiler issues a warning if the body of an `if` statement is on a separate line with the same indentation as the `if` itself – this resembles an error.

## 5.6 `while`

```
while condition body
```

- The condition is `Bool`, and the boundary rules are similar to those of `if` (5.5).
- The body is a block or a single statement: `while s[n] != 0 n++`.
- **Infinite loop — `while true`.** There isn't a separate `loop` keyword. `while true`
  without a `break` inside, the compiler considers it infinite: it doesn't require a `return` after it, and the code after it is unreachable (warning).

## 5.7 `for`

```
for name: T in start..end body
for name: T in start...end body
for name: T in start..end by step body
for name: T in array_or_string body
for name: T, index: ULong in array_or_string body
for mut name: T in ... body
```

- **The variable type is always specified.** The variable is only visible within the body.
- `a..b` — **semi-open** range: `b` is not included. `a...b` — **closed**: `b` is included. The ranges differ by a single dot, therefore the primary one is semi-open.
- `start` and `end` are expressions of type `T` (literals receive type `T`).
  They are **evaluated once**, before the first iteration, from left to right.
- **Step** `by k` — a constant integer, not `0`, where `|k|` fits within `T`. Without `by`, the step is `1`. The sign determines the direction, so `by -1` works for unsigned `T`.
- Within the range **`T`**, an integer type is used. When iterating through an array `T`, the type of its element is used; when iterating through a string, `UWord` is used.
- Without the `mut` keyword, the variable is immutable within the scope. However, within a range with `mut`, it can be modified, and this **affects the loop execution**, similar to C.
- The body – this is either a block or a single statement. The boundary of the range or source *before* the body – according to rule 5.5; the parentheses rule applies *after* `in`.

### Range Semantics

**The loop neither crosses its bound nor overflows the variable.** For a step
`k > 0` and a half-open range

```
for mut i: T in a..b by k { body }
```

Works as

```
let mut i: T = a
let end: T = b
if i < end {
    while true {
        body
        if i >= end break          // The body (for mut) jumped to the end.
        if end - i <= k break      // The next step would reach `end`.
        i = i + k
    }
}
```

- For the closed range `a...b`: in the first `if`, `i <= end`; in the second, `end - i < k`.
- For moving down (`k < 0`): comparisons are mirrored (`i > end`, `i <= end`), the difference is `i - end`, and the step is `i = i - |k|`.
- The difference is always non-negative and is treated as an unsigned integer of the same width, therefore it does not overflow and is handled as such for signed `T`.
- The `continue` within the body proceeds to checks after the body itself.

Consequences:

| Loop | Iterations |
|---|---|
| `for i: UWord in 0..argc` when `argc = 0` | results in no iteration |
| `for i: UByte in 0...0xFF` | 256, without looping |
| `for i: UWord in 0...9 by 2` | 0, 2, 4, 6, 8 |
| `for i: UWord in 10...0 by -1` | 10, 9, …, 0 |
| `for i: UByte in 0xFF...0 by -3` | 0xFF, 0xFC, …, 0x00, without going beyond 0xFD |
| `for i: UWord in argc..0 by -1` | `argc`, …, 1; index – `i - 1` |
| `for i: UWord in 5..5` | There are none |

- **Empty range** – zero iterations, not an error.
- If the `for mut` body **overflows** a variable (`i++` with `UByte` from `0xFF` results in `0`), it becomes smaller than `end`, and the loop may not terminate. This is the responsibility of the programmer.
- The compiler can check one condition instead of two, if it detects that the variable is not modified within the body.

### Iterating over Arrays and Strings

```
for item: String in names println(item)
for item: Word, index: ULong in values {
    // item == values[index], index == 0, 1, ...
}
for character: UWord in text println(toString(character as ULong))
```

- The source and its length are calculated **once** before the first iteration. An empty array or string results in zero iterations.
- For `T[]`, the variable must have the type `T`. During each iteration, the current element is copied into it. For `String`, the variable has the type `UWord` and receives a Unicode code point, not a byte or a single-character string.
- An optional index has a type of `ULong`, starts at zero, and is only visible within the implementation. It is immutable for the program; `mut` applies only to the element itself.
- `for mut item: T in values` allows modification of a local copy of the element, but does not modify the underlying array.
- Elements added during iteration are not accessed because the length has already been saved. Reducing the array can lead to a common out-of-bounds error when reading an element.
- `continue` increments the position and proceeds to the next element; `break` terminates the loop.

## 5.8 `break` and `continue`

- `break` exits the nearest `while`, `for`, or `switch` loop.
- `continue` proceeds to the next iteration of the nearest `while` or `for` loop (`switch` skips it).
- **No method exists.** It’s not possible to exit two loops simultaneously – either through a flag or using `return`.
- Using `break` or `continue` outside of a loop – is an error.  The function boundary is also a boundary: using `break` within a nested function or literal does not exit the outer function’s loop.

## 5.9 `switch`

```
switch expression {
    case constant:
        operators
    case constant:
    case constant:
        operators
    default:
        operators
}
```

- The expression is an integer, `enum`, or `Type`, without parentheses, as in `if`. The body is always a block `{ }`.
- The `case` label is a constant expression of the same type. Duplicate labels are an error. There should only be one `default` in any place.
- **Execution falls through** to the next `case` until a `break`, `return`,
  or `continue` is encountered, as in C.
- **`break` within a `switch` statement exits the `switch`**, not the loop surrounding it.
  `continue` refers to the loop itself.
- Completeness is not checked: if no label matches and `default` is not present, nothing happens.
- **`let` directly within `case` – an error**, including nested functions: in C

  This can lead to skipping initialization. A variable is declared within a block:

  ```
  case 0: {
      let half: UWord = n / 2
      return half
  }
  ```

## 5.10 `return`

- `return expression` – in a function with a result; the type of the expression is the type of the result.
- `return` is used in `Void`-functions. It is not needed at the end of a `Void`-function.
  Additionally, `return expression` is allowed if the expression's type is `Void`; this is necessary for generic wrappers like `join<R>` when `R = Void`.
- If the following token initiates an expression, `return` always takes it,
  regardless of the function's result type and newline. Therefore,
  `return` and the subsequent `finish()` line constitute `return finish()`.
  To exit early before another operator, use a block:
  `if done { return }`, followed by `finish()`. A blank `return` in a function
  with a non-`Void` result is an error.
- **A function that doesn't reach a closing `}` cannot be compiled.** The compiler checks for this: the body must contain an operator that prevents execution from continuing beyond it – such as `return`, `while true` without `break`, `if`/`else` statements with both branches being the same, or a block containing such an operator. Otherwise, an error occurs. This usually happens when this is the last operator; if there's more code after it, that code is unreachable (a warning is issued, but no error occurs).
- Code following `return`, `break`, `continue`, and `while true` without a `break` –
is a warning.


### Analyzing a `union` using `Type`

`switch variantof(value)` compares the type of the active variant with constant labels `case typeof(T):`. Full types are compared, including the type of array elements, the content of the reference, its permissions, and the function signature. Aliases and variant reordering do not create different labels.

The `break`, `continue`, and `default` statements are preserved. The type of the variable inside the loop is not automatically narrowed; the value is obtained through `value as T`. If the value changes after the initial check, the extraction will re-verify the option. For a stable snapshot, use `let current: U = value` and refer to `current` in the check and extraction. See [2.16](02-types.md#216-union-types) for an example.
