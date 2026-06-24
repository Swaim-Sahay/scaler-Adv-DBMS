# Lab 7 — Dijkstra's Shunting-Yard

`main.cpp` takes a SQL `WHERE` clause written in normal infix notation,
converts it to postfix (Reverse Polish Notation) using Dijkstra's
Shunting-Yard algorithm, then walks the postfix once per row to decide
which rows pass the filter.

---

## Why bother converting to postfix

Reading infix needs you to track precedence and parentheses at every step.
`marks >= 80 AND id < 3 OR id > 5` is ambiguous without knowing that `AND`
binds tighter than `OR` and that comparisons bind tighter than both.
Postfix removes that problem entirely. The operator order in the output
already encodes what to do first, so the evaluator is just a single
left-to-right scan with one stack. No recursion, no precedence table at
eval time.

Simple arithmetic example:

```
Infix   : 2 + 3 * 4
Postfix : 2 3 4 * +

Evaluation:
  2   -> stack: [2]
  3   -> stack: [2, 3]
  4   -> stack: [2, 3, 4]
  *   -> pop 4 and 3, push 12   -> stack: [2, 12]
  +   -> pop 12 and 2, push 14  -> stack: [14]
  result: 14  (correct — * bound tighter than +)
```

---

## Precedence table

| Operator | Precedence |
|----------|-----------|
| `>` `<` `>=` `<=` `=` | 3 |
| `AND` | 2 |
| `OR`  | 1 |

Higher number = tighter binding. So `marks >= 80 AND id < 3` is read as
`(marks >= 80) AND (id < 3)`.

---

## The algorithm

Two containers: an output queue and an operator stack. Walk the tokens
left to right:

- **operand** (column name or number) → push straight to output
- **operator `o`** → while the stack top is an operator with precedence `>= o`,
  pop it to output; then push `o`
- **`(`** → push to stack
- **`)`** → pop to output until `(`, discard the `(`
- **end of input** → pop everything left on the stack to output

All operators here are left-associative, so the pop condition is
`prec(top) >= prec(incoming)`.

---

## Worked example

Input: `marks >= 80 AND (id < 3 OR id > 4)`

| Token | Output so far | Operator stack |
|-------|--------------|----------------|
| marks | marks | |
| >= | marks | >= |
| 80 | marks 80 | >= |
| AND | marks 80 >= | AND |
| ( | marks 80 >= | AND ( |
| id | marks 80 >= id | AND ( |
| < | marks 80 >= id | AND ( < |
| 3 | marks 80 >= id 3 | AND ( < |
| OR | marks 80 >= id 3 < | AND ( OR |
| id | marks 80 >= id 3 < id | AND ( OR |
| > | marks 80 >= id 3 < id | AND ( OR > |
| 4 | marks 80 >= id 3 < id 4 | AND ( OR > |
| ) | marks 80 >= id 3 < id 4 > OR | AND |
| end | marks 80 >= id 3 < id 4 > OR AND | |

Final RPN: `marks 80 >= id 3 < id 4 > OR AND`

---

## Evaluation

Walk the postfix with one int stack. Numbers and column values get pushed.
Operators pop two operands and push the result (0 or 1 for comparisons,
also 0 or 1 for AND/OR). The last value on the stack is the boolean answer
for that row.

---

## Build & run

```bash
g++ -std=c++17 -Wall -Wextra main.cpp -o main && ./main
```

Expected output:

```
Infix WHERE : marks >= 80 AND (id < 3 OR id > 4)
Postfix     : marks 80 >= id 3 < id 4 > OR AND

Rows matching WHERE clause:
  Riya (id=1, marks=92)
  Diya (id=3, marks=88)
  Meera (id=5, marks=81)
```

---

## How this compares to the parser in `../queryParsing`

Both folders solve the same problem — respecting operator precedence inside
a WHERE clause — in two different ways.

| | `queryParsing/` | `dsy/` (this folder) |
|---|---|---|
| Output of front-end | AST (tree of Expr nodes) | flat postfix token list |
| Precedence handling | encoded in grammar / function call depth | explicit precedence table + operator stack |
| Evaluator shape | recursive tree walk | one-pass stack |
| Error recovery | easy — you're at a known node | harder — stack state is opaque |

Real query planners use a tree because it carries metadata (types, row-count
estimates, index hints) that a flat list can't hold. Shunting-yard is still
useful as a building block — think expression parsing inside a CASE statement
or a stored procedure body.
