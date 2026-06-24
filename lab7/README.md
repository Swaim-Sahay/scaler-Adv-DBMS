# Lab 7 — 24BCS10335 Swaim Sahay

Two implementations of a SQL `WHERE`-clause evaluator, both written in C++17:

- `DSY/` — Dijkstra's Shunting-Yard: infix → postfix → one-pass stack evaluator
- `queryParsing/` — Lexer + recursive-descent parser → AST → tree-walk evaluator

See each subfolder for its own README and `main.cpp`.

## Build & run

```bash
# DSY
cd DSY
g++ -std=c++17 -Wall -Wextra main.cpp -o main && ./main

# queryParsing
cd ../queryParsing
g++ -std=c++17 -Wall -Wextra main.cpp -o main && ./main
```
