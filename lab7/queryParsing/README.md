# Lab 7 — Query Parsing

`main.cpp` is a minimal SQL front-end. It takes a string like

```
SELECT name FROM students WHERE marks >= 80 AND id < 6
```

lexes it into a token stream, builds an AST with a recursive-descent parser,
then walks the tree once per row to print matching values.

---

## Pipeline

```
SQL string
    │
    ▼
  lex()                 ->  vector<Token>
    │
    ▼
  Parser.parse()        ->  SelectStmt { col, table, WHERE expr-tree }
    │
    ▼
  run() / matches()     ->  filtered rows printed
```

Same three-phase shape as a real database engine, just scaled down to what
the lab needs.

---

## 1. Lexing

The lexer scans left-to-right and emits a flat `vector<Token>`. Each token
is the smallest meaningful unit: a keyword, an identifier, a number, an
operator, or a parenthesis. Whitespace gets dropped.

Having a separate lex step means the parser never has to think about raw
characters. It just sees `TokKind::Where` instead of "five characters that
spell W-H-E-R-E". Two-character operators like `>=` are also cleaner to
handle in the lexer than in the parser.

```
SELECT name FROM students WHERE marks >= 80
   ↓
[SELECT][name][FROM][students][WHERE][marks][>=][80][END]
```

---

## 2. Parsing — recursive descent

One function per grammar rule. The call graph is the precedence hierarchy:
the deepest function handles the tightest-binding operators.

```
select := SELECT IDENT FROM IDENT WHERE expr
expr   := term  ( OR  term  )*       -- OR binds loosest
term   := factor ( AND factor )*     -- AND next
factor := '(' expr ')' | cmp
cmp    := IDENT (>|<|>=|<=|=) NUMBER -- comparisons tightest
```

`parseExpr` calls `parseTerm`, which calls `parseFactor`, which calls
`parseCmp`. So a comparison is always fully resolved before `AND` tries to
grab its operands, and `AND` groups are resolved before `OR`. Parentheses
just jump back to `parseExpr`, which restarts the precedence chain.

The output is an `Expr` tree with a `NodeKind` enum tag (Column, Number,
BinOp). One struct, no virtual functions, no `dynamic_cast` at eval time.

For `marks >= 80 AND id < 6`:

```
        AND
       /   \
     >=     <
    /  \   / \
 marks 80 id  6
```

---

## 3. Executing

`run()` walks the tree once per row by calling `matches()`. The recursive
function is about ten lines:

- `AND` / `OR` nodes short-circuit into their children
- comparison nodes resolve the left column via `colVal()` and the right
  numeric literal via `stoi()`

The dataset is six students hard-coded in `main()`. The query is:

```
SELECT name FROM students WHERE marks >= 80 AND id < 6
```

Expected output:

```
Result of: SELECT name FROM students WHERE ...
  Riya
  Diya
  Meera
```

Riya (id 1, marks 92), Diya (id 3, marks 88) and Meera (id 5, marks 81)
all clear `marks >= 80` and have `id < 6`. Rohan (id 6) has marks < 80 and
fails the AND. Kabir and Aarav both fail the marks threshold.

---

## Build & run

```bash
g++ -std=c++17 -Wall -Wextra main.cpp -o main && ./main
```

---

## What I took away

- Precedence isn't something you check at runtime. It's baked into *which
  function calls which*. The grammar layering does the work.
- The AST is what real query planners pass around after parsing. Joins,
  indexes, projection pushdown, cost estimation — all of it starts by
  rewriting or annotating this tree.
- A single tagged struct + enum (`NodeKind`) for the AST nodes is enough for
  a lab-scale evaluator. A bigger language would want a visitor pattern or a
  virtual `eval()` method, but here the tagged union is cleaner and shorter.

---

## How this compares to `../dsy` (Shunting-Yard)

| | `queryParsing/` (this folder) | `dsy/` |
|---|---|---|
| Front-end output | AST (tree of `Expr` nodes) | flat postfix token list |
| Precedence source | grammar / call-graph depth | explicit precedence table |
| Evaluator | recursive tree walk | one-pass stack |
| Metadata support | easy — annotate AST nodes | hard — flat list loses structure |
| Error recovery | easy — at a known tree node | harder — stack state is opaque |
