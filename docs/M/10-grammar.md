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

# 10. Grammar

[← Diagnostics](09-diagnostics.md) · [Table of Contents](README.md)

Syntax Summary in EBNF. `{ x }` represents zero or more occurrences, `[ x ]` represents an optional occurrence, `|` represents a choice, `"..."` represents a token. Lexical analysis – [section 1](01-lexical.md), precedence – [4.1](04-expressions.md#41-priorities). Where the grammar is ambiguous, rules from the text apply; these are marked `(* ... *)`.

## File

```ebnf
file        = { top_decl } ;

top_decl    = import_decl
            | export_decl
            | struct_decl
            | alias_decl
            | enum_decl
            | var_decl
            | func_decl
            | extern_decl ;

import_decl = "import" "{" name_as { "," name_as } [ "," ] "}"
              "from" string_literal ;
export_decl = "export" "{" name_as { "," name_as } [ "," ] "}" ;
name_as     = identifier [ "as" identifier ] ;
```

## Types

```ebnf
struct_decl = [ "packed" ] "type" identifier [ generic_params ]
              "{" field { "," field } [ "," ] "}" ;
field       = identifier ":" type ;

alias_decl  = "type" identifier [ generic_params ] "=" type ;
              (* Following `'type identifier [generic_params]': '{' — struct,
                 '=' — alias *)
generic_params = "<" identifier { "," identifier } [ "," ] ">" ;
type_args      = "<" type { "," type } [ "," ] ">" ;

enum_decl   = "enum" identifier ":" type
              "{" enum_item { "," enum_item } [ "," ] "}" ;
enum_item   = identifier [ "=" const_expr ] ;

type        = type_term { "|" type_term } ;
type_term   = prefix_type { "[" "]" } ;
              (* '[]' — array; the suffix refers to the entire type to the left.;
                 The length is not part of the type. *)
prefix_type = "@" [ "mut" ] prefix_type
            | func_type
            | "(" type ")"
            | type_name ;
func_type   = "(" [ param { "," param } [ "," ] ] ")" ":" type ;
              (* '(' And then ‘)’ or ‘identifier:’ – function type,
                 Otherwise – parentheses around the type. *)
type_name   = "Byte" | "UByte" | "Half" | "UHalf" | "Word" | "UWord"
            | "Long" | "ULong" | "Bool" | "Float" | "Double" | "String"
            | "Thread" | "Type" | "Void" | identifier [ type_args ] ;

param_type  = [ "mut" ] type              (* mut Only for `T[]` in the parameter *)
            | "..." ;                    (* Only the last parameter *)
```

## Declarations

```ebnf
var_decl    = "let" [ "mut" ] identifier ":"
              ( type [ "=" expr ]
              | type "[" expr "]" "=" array_lit ) ;
              (* The second form – creating an array with an initial length.;
                 type Sets the element type and the type of the variable. — type[];
                 The expression of size is only in the last suffix.;
                 The type of function element is grouped within parentheses. (3.2) *)
func_decl   = "let" [ "mut" ] identifier [ generic_params ]
              "(" [ param { "," param } [ "," ] ] ")" ":" type block ;
              (* After `let [mut] identifier [generic_params]`: ':' — variable,
                 '(' — function; `generic_params` is only available for regular functions
                 Top-level without mut *)
param       = identifier ":" param_type ;
extern_decl = "extern" "let" identifier
              "(" [ param { "," param } [ "," ] ] ")" ":" type ;
              (* Only the native function (3.5) *)
```

## Operators

```ebnf
block       = "{" { statement } "}" ;

statement   = var_decl
            | func_decl
            | block
            | if_stmt
            | while_stmt
            | for_stmt
            | switch_stmt
            | "break"
            | "continue"
            | "return" [ expr ]
            | simple_stmt ;

simple_stmt = place assign_op expr
            | place ( "++" | "--" )
            | call ;
assign_op   = "=" | "+=" | "-=" | "*=" | "/=" | "%="
            | "&=" | "|=" | "^=" | "<<=" | ">>=" ;
place       = postfix_expr ;

body        = block | statement ;
              (* not `var_decl` and not func_decl *)
condition   = "(" expr ")"
            | expr ;
              (* If the first token is `(`— this is only within these brackets.;
                 Otherwise – the longest expression. *)

if_stmt     = "if" condition body [ "else" body ] ;
              (* else — to the nearest if without else *)
while_stmt  = "while" condition body ;
for_stmt    = "for" [ "mut" ] identifier ":" type "in"
              expr ( ".." | "..." ) condition_end
              [ "by" const_expr ] body
            | "for" [ "mut" ] identifier ":" type
              [ "," identifier ":" "ULong" ] "in" expr body ;
condition_end = expr ;
              (* following the same rules as condition *)

switch_stmt = "switch" expr "{" { switch_case } "}" ;
switch_case = ( "case" const_expr | "default" ) ":" { case_stmt } ;
case_stmt   = statement ;
              (* Aside from `var_decl` and `func_decl`: declarations – within a block. *)
```

The `return` boundary is independent of the function's result type. If the next token initiates an expression, `return` will take that expression, including across newline characters. Otherwise, it’s an empty `return`, valid only when the result is `Void`. To implement early exits before the next call or assignment, use a block: `if done { return }`. Returning an expression of type `Void` from a `Void`-function is permitted; the expression is evaluated before the return.

In an `if`, `while`, or `for` condition, or a `switch` expression, a top-level
struct literal `{ ... }` or function literal is not allowed: `{` starts the body
there. Inside parentheses, these literals are allowed.

## Expressions

```ebnf
expr        = or_expr [ "?" expr ":" expr ] ;      (* associativity (right-to-left) *)
or_expr     = and_expr { "||" and_expr } ;
and_expr    = cmp_expr { "&&" cmp_expr } ;
cmp_expr    = bor_expr [ cmp_op bor_expr ] ;          (* Without chains *)
cmp_op      = "==" | "!=" | "<" | "<=" | ">" | ">=" ;
bor_expr    = bxor_expr { "|" bxor_expr } ;
bxor_expr   = band_expr { "^" band_expr } ;
band_expr   = shift_expr { "&" shift_expr } ;
shift_expr  = add_expr { ( "<<" | ">>" ) add_expr } ;
add_expr    = mul_expr { ( "+" | "-" ) mul_expr } ;
mul_expr    = cast_expr { ( "*" | "/" | "%" ) cast_expr } ;
cast_expr   = unary_expr { "as" type_term } ;
              (* The purpose of the union is to group.: as (A | B). *)
unary_expr  = ( "-" | "!" | "~" | "@" ) unary_expr
            | postfix_expr ;
postfix_expr = primary { call_suffix | "[" expr "]" | "." identifier } ;
call_suffix = "(" [ expr { "," expr } [ "," ] ] ")" ;
call        = postfix_expr call_suffix ;

primary     = int_literal | float_literal | char_literal | string_literal
            | "true" | "false" | "null"
            | identifier
            | identifier "::" type_args         (* specific generic function *)
            | identifier "." identifier          (* element enum *)
            | "(" expr ")"
            | struct_lit
            | func_lit
            | array_lit
            | builtin_call ;

func_lit    = "(" [ param { "," param } [ "," ] ] ")" ":" type block ;
              (* '(' And then ‘)’ or ‘identifier:’ – this is a function literal.,
                 Otherwise — an expression within parentheses *)
struct_lit  = "{" [ field_init { "," field_init } [ "," ] ] "}" ;
field_init  = "." identifier "=" expr ;
array_lit   = "[" [ expr { "," expr } [ "," ] ] "]" ;

builtin_call = ( "sizeof" | "alignof" ) "(" ( type | expr ) ")"
              (* variable name – value, type – type (7.4) *)
             | "variantof" "(" expr ")"
             | "typeof" "(" ( type | expr ) ")"
              (* The same rules for name resolution; the result – a value Type,
                 The expression is not evaluated; `typeof` is not a type literal. *)
             | "offsetof" "(" type "," identifier ")"
             | "vaArg" "(" expr "," expr "," type ")"
             | "breakpoint" "(" ")"
             | builtin_name call_suffix ;
builtin_name = "length" | "arrayCapacity" | "arrayReserve" | "arrayResize"
             | "arrayPush" | "arrayPop" | "arrayInsert" | "arrayRemove" | "vaCount" ;

const_expr  = expr ;                        (* The value is known during compilation. *)
```

- `{` at the beginning of an expression – always represents a struct literal: blocks are never expressions.
- `[` at the beginning of an expression – represents a literal array, followed by an expression which specifies the index.
- If the expression starts with `(` followed by `)` or `identifier ":"`, it represents a literal function, otherwise it’s an expression within parentheses.
- `identifier "." identifier` – an element of `enum`, if the left-hand side is the name of the `enum`, otherwise – a field.
