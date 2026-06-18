# Lab 4 — Red-Black Tree + B-Tree

## Overview

Two classic self-balancing / disk-friendly index structures used in
database engines, implemented in C++17 from the spec.

| File | Algorithm | Used in |
|---|---|---|
| [`RedBlackTree.cpp`](RedBlackTree.cpp) | Red-Black Tree (self-balancing BST) | In-memory indexes, `std::map` / `std::set` |
| [`BTree.cpp`](BTree.cpp) | B-Tree of minimum degree T | On-disk indexes (PostgreSQL, MySQL InnoDB, SQLite) |

---

## Part 1 — Red-Black Tree (`RedBlackTree.cpp`)

A BST that stays balanced by enforcing four invariants on node colors:

1. Every node is RED or BLACK.
2. The root is BLACK.
3. No two consecutive RED nodes (a RED node's parent must be BLACK).
4. Every path from a node to its NULL descendants has the same number
   of BLACK nodes ("black-height").

These four rules together cap the height at `2 * log2(n+1)`, giving
O(log n) insert / search / delete.

### Operations

- **`insert(key)`** — standard BST insert, then `fix_insert` restores
  the invariants. The fix walks up the tree handling three cases:
  - **Case 1** — uncle is RED: recolor parent + uncle BLACK, grandparent RED, recurse on grandparent.
  - **Case 2** — triangle (LR or RL): rotate the parent to convert into a "line".
  - **Case 3** — line (LL or RR): rotate the grandparent and swap parent / grandparent colors.
- **`remove(key)`** — CLRS-style: find the node, transplant with successor / `minimum(right)`, then `fix_delete` to restore black-height when a BLACK node was removed.
- **`print()`** — inorder traversal annotated with each node's color.

### Demo

```text
$ g++ -std=c++17 -o rbt RedBlackTree.cpp && ./rbt
Inorder (key + color R/B):
1R 5B 10R 15B 20B 25R 30B
After removing 20:
1R 5B 10R 15B 25B 30B
```

---

## Part 2 — B-Tree of order T (`BTree.cpp`)

The on-disk index structure used by every major RDBMS. Each node
holds between `T-1` and `2T-1` keys; with `T = 2` a node can have
2, 3, or 4 children, which gives a short, bushy tree. In
PostgreSQL, B-Tree nodes are 8 KB pages, so one disk read fetches
hundreds of keys — that's the whole point of the B-Tree.

### Operations

- **`insert(key)`** — "split on the way down". Before descending into
  a full child (`2T-1` keys) we call `split_child`: the right half
  of the child's keys becomes a new sibling, the median key is
  promoted to the parent. This guarantees we never recurse into a
  full node.
- **`search(key)`** — binary search within a node, descend to the
  appropriate child, repeat.
- **`remove(key)`** — the tricky one. Before descending we call
  `fill(idx)` to ensure the target child has at least `T` keys.
  `fill` first tries to **borrow** from a richer sibling; if neither
  sibling has a key to lend, it **merges** the child with one
  sibling and pulls down the separator. Inside the node, we either
  erase directly (leaf) or swap the key with its predecessor /
  successor and recurse.
- **`print()`** — inorder traversal.

### Demo

```text
$ g++ -std=c++17 -o btree BTree.cpp && ./btree
Inorder after inserts:
1 3 5 6 7 10 12 17 20 25 30
Search 17: found
Search 99: not found
Inorder after removing 6 and 20:
1 3 5 7 10 12 17 25 30
```

---

## Red-Black Tree vs B-Tree — when to use which

| Property | Red-Black Tree | B-Tree (order t) |
|---|---|---|
| Storage | In-memory | Designed for disk (one node = one page) |
| Node size | 1 key per node | Up to `2t-1` keys per node |
| Height | O(log n) | O(log_t n) — much shorter for large t |
| Use in databases | In-memory indexes, `std::map` | On-disk indexes (PG, MySQL, InnoDB) |
| Cache friendliness | Poor (pointer chasing) | Excellent (sequential keys per page) |
| Re-balancing | Rotations only | Child split / sibling borrow / merge |

PostgreSQL's B-Tree index pages are 8 KB by default — the same
`page_size` we saw in Lab 2 — so one disk read fetches an entire
B-Tree node.

---

## Build

```bash
# Red-Black Tree
g++ -std=c++17 -o rbt     RedBlackTree.cpp && ./rbt

# B-Tree
g++ -std=c++17 -o btree   BTree.cpp       && ./btree
```

Both compile clean under `-Wall -Wextra`.

---

## Author

24BCS10335 — Swaim Sahay
