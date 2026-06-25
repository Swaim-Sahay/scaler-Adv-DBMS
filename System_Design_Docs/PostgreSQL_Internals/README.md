# PostgreSQL Internal Architecture

**Author:** Swaim Sahay
**Roll Number:** 24BCS10335
**Course:** Advanced DBMS - System Design Discussion

> I came into this topic thinking PostgreSQL was a single clever program. It
> isn't. It's four subsystems that mostly don't trust each other - a buffer
> manager that caches pages, a B-tree that finds rows, an MVCC layer that keeps
> multiple versions of every row, and a write-ahead log that makes the whole
> thing crash-safe. What surprised me is how tightly these four are wired
> together: MVCC only works because of how the heap stores tuples, the buffer
> manager only stays correct because WAL is written first, and the planner only
> makes good decisions because a fifth quiet subsystem (statistics) is feeding
> it numbers. This document follows those wires. I also built a miniature
> version of each subsystem in my capstone (MiniDB), so a lot of what's here is
> "I implemented this and saw why it had to work that way."

---

## 1. Problem Background

PostgreSQL descends from the POSTGRES project started by Michael Stonebraker at
UC Berkeley in 1986. The goal back then was research-grade: build a database
that could handle complex data types, enforce rules, and - critically - let
**many users read and write shared data at the same time without corrupting it
or blocking each other**. SQL support arrived in 1994-96 (Postgres95 →
PostgreSQL), and the system has been refined for nearly three decades since.

The reason its internals are worth studying is that PostgreSQL made a specific,
opinionated bet on **MVCC (Multi-Version Concurrency Control)** as the way to
get concurrency. That single decision shapes the storage layout (every row
carries version metadata), forces the existence of VACUUM (someone has to clean
up old versions), and interacts with WAL and the buffer manager in ways that are
genuinely instructive. You can't understand PostgreSQL's internals without
understanding *why* it keeps multiple copies of your data lying around.

The four subsystems the course asks about - **buffer manager, B-tree, MVCC,
WAL** - are exactly the four I'll trace, plus the **planner/statistics** layer
that ties query execution to all of them.

---

## 2. Architecture Overview

### 2.1 The shape of the system

PostgreSQL runs as a set of cooperating processes sharing one big region of
memory. A query travels from a client socket down through parsing, planning,
execution, the buffer cache, and finally to disk - with WAL sitting beside the
whole thing catching every change.

```
   client ──TCP/Unix socket──► backend process (one per connection)
                                    │
              ┌─────────────────────┼──────────────────────┐
              ▼                     ▼                       ▼
          Parser              Planner/Optimizer          Executor
        (SQL → tree)      (uses pg_statistic stats)   (runs the plan)
                                                          │
                                          reads/writes pages through
                                                          ▼
                            ┌───────────────────────────────────────┐
                            │      shared_buffers (page cache)        │
                            │   ClockSweep replacement, 8 KB pages    │
                            └───────────────┬─────────────────────────┘
                writes go to WAL FIRST ──►  │  ◄── background writer flushes dirty
                            ▼               ▼
                   ┌──────────────┐   ┌──────────────────────────────┐
                   │   pg_wal/    │   │  base/<db>/<rel> heap + index │
                   │  (WAL segs)  │   │  files on disk (8 KB pages)   │
                   └──────────────┘   └──────────────────────────────┘

   background processes: WAL writer, checkpointer, autovacuum, bgwriter, stats collector
```

### 2.2 Main components and what each owns

| Component | Source area | Responsibility |
|---|---|---|
| Buffer manager | `src/backend/storage/buffer/` | Cache 8 KB pages in `shared_buffers`, evict with ClockSweep |
| Access method (B-tree) | `src/backend/access/nbtree/` | Index structure, search, insert, page splits |
| Heap + MVCC | `src/backend/access/heap/` | Store tuples with `xmin`/`xmax`, visibility |
| WAL | `src/backend/access/transam/` | Durability, crash recovery, replication source |
| Planner | `src/backend/optimizer/` | Cost-based plan selection using statistics |
| Statistics | `pg_statistic`, ANALYZE | Feed selectivity estimates to the planner |

### 2.3 Data flow for one query

`SELECT ... JOIN ... WHERE ...` → parser builds a tree → planner asks the
statistics layer "how many rows will this predicate keep?" and picks a plan
(index scan vs seq scan, hash vs merge vs nested-loop join) → executor pulls
tuples, requesting each page from `shared_buffers` (a disk read only on a miss)
→ for every tuple it applies the **MVCC visibility check** against the
transaction's snapshot → matching rows stream back to the client. Any write
takes the same path but emits a **WAL record before** the page is modified.

---

## 3. Internal Design

### 3.1 Buffer Manager - how pages move through memory

`shared_buffers` is an array of fixed 8 KB **buffer frames** shared by every
backend. Each frame has a header tracking which page it holds, whether it's
**dirty**, a **pin count** (how many backends are currently using it), and a
**usage count** (0-5) for replacement.

The lifecycle of a page request (`ReadBuffer`):

```
  backend wants page P
        │
   look up P in the buffer hash table
        │
   ┌────┴─────────────────────────┐
   │ HIT: pin it, usage_count++    │   ← no disk I/O
   └───────────────────────────────┘
   ┌───────────────────────────────┐
   │ MISS: run ClockSweep to find   │
   │ a victim frame, evict it       │   ← if victim dirty, must write it back
   │ (write back if dirty), read P  │     AND its WAL must already be on disk
   │ from disk into that frame      │
   └───────────────────────────────┘
```

**Why ClockSweep and not LRU?** True LRU needs a list reordered on every single
access, which becomes a lock-contention nightmare when dozens of backends hit
the cache concurrently. ClockSweep approximates recency with a circular "hand"
and a per-frame counter: the hand sweeps, decrementing `usage_count`; a frame
with count 0 and no pins gets evicted. It's O(1), needs no global ordering, and
- importantly - the usage-count cap means a giant sequential scan can't flush
out the hot working set (each scanned page only bumps the count by one).

> **From building MiniDB:** I implemented exactly this ClockSweep loop. The thing
> that clicked for me is the *pin count*. Without it, the buffer pool could evict
> a page that an operator is still reading mid-row, and you'd get corruption. Pins
> are what make "cache pages but also let many readers touch them" safe.

A key correctness rule lives here too: **a dirty page may not be written to disk
until the WAL records describing its changes are already durable.** This is the
WAL ↔ buffer-manager contract that makes crash recovery possible.

### 3.2 B-Tree (nbtree) - how rows are found

PostgreSQL's default index is a **B+ tree** (Lehman-Yao variant). Real data
(index entries) live only in the **leaf level**; internal pages hold separator
keys that route a search downward. Leaf pages are linked left↔right so range
scans walk sideways.

```
                 [ • 50 • 100 • ]            ← internal (separators only)
                /      |       \
        [10 20 30]  [60 70 80] [110 120]     ← leaves: (key → heap TID), linked →
```

- **Search:** descend from the root, binary-searching separators, until a leaf;
  find the key and read its **TID** (tuple id = page number + slot).
- **Insert:** find the leaf, insert in order; if the page overflows, **split** it
  in half and push a separator key up to the parent - which may split too, all
  the way up, occasionally adding a new root level.
- **Index page layout:** each index page is itself a slotted 8 KB page; a special
  area at the bottom holds the left/right sibling pointers (the Lehman-Yao
  "high key" trick lets concurrent searches stay correct during a split without
  locking the whole tree).

The crucial PostgreSQL detail: the index entry points to the heap via a TID, so
**index and table are separate**. One heap can have many indexes; an index scan
is "search index → get TID → fetch heap page → check visibility."

> **From MiniDB:** my B+ tree stores `key → RID` (page,slot) - the same
> indirection as PostgreSQL's TID. Implementing the *split* was the moment B+
> trees stopped being abstract: when a leaf overflows you literally cut it in two
> and promote the middle key, and the recursion upward is what keeps every leaf at
> the same depth.

### 3.3 MVCC - heap tuple versioning

This is the centerpiece. PostgreSQL never updates a row in place. Every heap
tuple carries hidden system columns; the two that matter for visibility are:

- **`xmin`** - the transaction id that **created** this tuple version.
- **`xmax`** - the transaction id that **deleted or superseded** it (0 if live).

```
  txn 10 INSERT:                 (xmin=10, xmax=0)
  txn 15 UPDATE that row:
        old version becomes      (xmin=10, xmax=15)   ← kept, not erased
        new version written      (xmin=15, xmax=0)
  txn 12 (started before 15) reads → sees xmin=10 version
  txn 17 (started after 15)  reads → sees xmin=15 version
```

**The visibility rule** (what every tuple is tested against, using the reader's
**snapshot**):

```
visible(t) := (t.xmin is committed AND t.xmin < my snapshot AND not in-progress)
          AND (t.xmax is 0, OR t.xmax aborted, OR t.xmax not yet committed
               relative to my snapshot)
```

A **snapshot** records which transactions had committed at the instant the
statement (or transaction, depending on isolation level) began. Because every
reader filters tuples through its own snapshot, **readers never block writers and
writers never block readers** - the headline benefit. The price: old versions
("dead tuples") accumulate on the heap.

**Why VACUUM is necessary.** Those dead tuples don't clean themselves. VACUUM
walks the heap and reclaims space from tuples no snapshot can see anymore. It
also prevents **transaction-id wraparound**: xids are 32-bit and eventually
recycle; VACUUM "freezes" very old live tuples so they stay visible after
wraparound. Skip VACUUM and you get table **bloat** and, eventually, a database
that refuses writes to protect itself.

> **From MiniDB:** I implemented `xmin`/`xmax` and the visibility rule directly.
> The most elegant consequence I found: **ABORT needs no undo**. Since visibility
> checks commit status, an aborted transaction's inserts are simply never visible
> and its deletes "don't count" - I delete nothing physically. PostgreSQL relies
> on the same property; the cleanup is deferred to VACUUM, not done at abort.

### 3.4 WAL - durability and crash recovery

WAL ("Write-Ahead Logging") enforces one rule: **the log record describing a
change reaches disk before the change is considered durable.** PostgreSQL writes
a WAL record for every modification - heap insert/update/delete, index change,
even page-level metadata.

```
  modify page in shared_buffers (still dirty, not yet on disk)
        │
   write WAL record to pg_wal/   (sequential append - cheap)
        │
   at COMMIT: fsync the WAL      ← THIS is the durability point
        │
   (later) CHECKPOINT flushes dirty data pages, lets old WAL be recycled
        │
   (after crash) replay WAL from the last checkpoint forward → redo committed work
```

**Durability guarantee:** once `COMMIT` returns, the change survives a crash
because its WAL record is fsynced - even though the actual heap page might still
be sitting dirty in memory. This is the **no-force** policy (don't force data
pages at commit), and it's only safe because of WAL.

**Checkpointing** bounds recovery time. A checkpoint flushes all currently dirty
pages and records "everything up to WAL position X is on disk," so recovery only
needs to replay WAL *after* the last checkpoint, not from the beginning of time.

**Crash recovery** is automatic on startup: read the last checkpoint, replay WAL
forward (REDO). Committed transactions reappear; uncommitted ones leave no
visible trace (their tuples fail the visibility check because their xids never
committed).

A bonus that falls out of WAL for free: **streaming replication** (ship WAL to a
standby and replay it there) and **point-in-time recovery** (replay WAL up to a
chosen timestamp).

> **From MiniDB:** my WAL uses the same no-force/redo discipline. I "crash" the
> engine by dropping dirty pages without flushing, then replay committed WAL
> records on restart. Seeing committed rows survive and uncommitted ones vanish -
> with zero undo logic - made the PostgreSQL design concrete.

### 3.5 The planner and pg_statistic

The executor can run a query many ways; the **planner** picks the cheapest using
**statistics** gathered by `ANALYZE` and stored in `pg_statistic`:

- number of live rows (`reltuples`) and pages (`relpages`),
- per-column **histograms** (value distribution), **most-common-values** lists,
  and **n_distinct** (number of distinct values).

From these it estimates **selectivity** - the fraction of rows a predicate keeps
- and therefore the row counts flowing through each operator. Those estimates
drive the choices: **index scan vs sequential scan**, and **join algorithm**
(nested-loop for tiny inputs, **hash join** for large unsorted inputs, **merge
join** for sorted inputs) and **join order**.

> **From MiniDB:** my optimizer is a tiny version of this - it estimates
> cardinality from tuple/page counts, treats equality-on-primary-key as ~1 row
> (so it chooses the index), and orders a join to put the smaller relation
> outermost. Same decision *shape* as PostgreSQL, just with counts instead of
> histograms. Building it made `EXPLAIN` output finally readable to me.

---

## 4. Design Trade-Offs

### What MVCC buys, and what it costs

**Advantage:** true concurrency without read/write blocking. A heavy analytics
read and a steady write stream coexist. This is a *capability*, not just a speed
win - 2PL-only systems simply can't do this without readers and writers waiting.

**Cost:** dead-version accumulation → table/index bloat → mandatory VACUUM. A
single **long-running transaction** is the classic trap: it holds back the
"oldest snapshot," so VACUUM can't remove versions newer than it, and bloat
grows across the whole database until that transaction ends.

### Append-style updates vs in-place updates

PostgreSQL's "new version on every update" is simple and lock-free for readers,
but it means an UPDATE of one column rewrites the whole tuple and can move it to
a new page, which may require updating **every** index on the table. The **HOT
(Heap-Only Tuple)** optimization mitigates this: if no indexed column changed and
there's room on the same page, the new version is chained on that page and
indexes don't need touching. This is a direct engineering patch for an MVCC cost.

### Process-per-connection

Each connection is a full OS process sharing `shared_buffers`. Pros: strong
isolation, crash of one backend doesn't take others down, and shared cache
warming. Con: thousands of connections = thousands of processes = gigabytes of
overhead, which is why production deployments front PostgreSQL with a pooler
(PgBouncer).

### WAL: durability vs write amplification

Writing every change twice (once to WAL, once eventually to the data page) is
**write amplification**. The payoff - crash safety, replication, PITR - is worth
it for most workloads, but it's a real cost on write-heavy systems, and tuning
(`wal_compression`, checkpoint spacing) exists precisely to manage it.

### 8 KB pages

Bigger pages (vs SQLite's 4 KB) mean higher B-tree fan-out (shorter trees) and
fewer, larger I/Os - good for analytical scans. The cost is more wasted space at
the end of pages for tables with tiny rows, and a larger minimum I/O unit.

---

## 5. Experiments / Observations

### 5.1 Reading an `EXPLAIN ANALYZE` on a join

The recommended exercise is to run `EXPLAIN ANALYZE` on a multi-table join. A
representative plan looks like:

```
EXPLAIN ANALYZE
SELECT u.name, o.total
FROM users u JOIN orders o ON u.id = o.user_id
WHERE o.total > 1000;

Hash Join  (cost=... rows=4500 width=...) (actual time=... rows=4392 ...)
  Hash Cond: (o.user_id = u.id)
  ->  Seq Scan on orders o  (cost=...) (actual ... rows=4392 ...)
        Filter: (total > 1000)
        Rows Removed by Filter: 95608
  ->  Hash  (cost=...) (actual ...)
        ->  Seq Scan on users u  (cost=...) (actual ...)
Planning Time: 0.3 ms
Execution Time: 22.7 ms
```

What to read here:
- **Hash Join** was chosen (not nested-loop) because the planner estimated both
  inputs were large and unsorted - hashing the smaller side and probing is
  O(M+N) vs nested-loop's O(M×N).
- `cost=` are the planner's **estimates** (from `pg_statistic`); `actual` are the
  measured numbers. The gap between them tells you whether your statistics are
  fresh. A big estimate-vs-actual mismatch usually means you need to `ANALYZE`.
- **Rows Removed by Filter** shows the selectivity of `total > 1000` in practice.
- The smaller relation (`users`) is built into the hash table - the same
  "smaller side as build/outer input" principle I used in MiniDB's join.

The lesson: the planner is only as good as its statistics. Stale stats → wrong
row estimates → wrong join choice → an order-of-magnitude slower query.

### 5.2 Watching MVCC create dead tuples

A simple observation anyone can reproduce conceptually:

```
UPDATE accounts SET balance = balance + 1;   -- touches every row once
```

After this, the table has roughly **double** its live data on disk: the old
version of every row is now dead (its `xmax` set) but still physically present.
Querying `pg_stat_user_tables` shows `n_dead_tup` jump. Run it a few more times
without VACUUM and the table file keeps growing even though the live row count
never changes. Run `VACUUM` and space becomes reusable (though the file may not
shrink without `VACUUM FULL`). This is MVCC's storage cost made visible.

### 5.3 Why a hot page is fast the second time

Run a point query, then run it again. The first run shows disk reads (a buffer
miss → ClockSweep → read from disk). The second run is markedly faster because
the page is now pinned-then-cached in `shared_buffers` with a raised
`usage_count`, so it survives eviction. `EXPLAIN (ANALYZE, BUFFERS)` literally
prints `shared hit` vs `shared read` counts - a direct window into the buffer
manager I described in 3.1.

### 5.4 Crash recovery is a non-event

Killing the server mid-transaction and restarting it: PostgreSQL reads WAL from
the last checkpoint, replays it, and is consistent before it accepts the first
query - no manual step. I verified the same behaviour in MiniDB by dropping
dirty pages and replaying the WAL; committed data returned, uncommitted data did
not. The takeaway is that "durability" is not a vague promise - it's the precise
consequence of fsyncing the WAL at commit and replaying it on startup.

---

## 6. Key Learnings

**1. MVCC is the keystone, and everything bends around it.**
Tuple layout (`xmin`/`xmax`), the need for VACUUM, the no-force WAL policy, even
the HOT optimization - all of it exists because PostgreSQL chose to keep multiple
versions instead of locking rows. You can't explain any one subsystem in
isolation; they're co-designed around that bet.

**2. "Don't block readers and writers" has a hidden janitor.**
The elegance of snapshot reads comes with a deferred bill: dead tuples. The
cleverness didn't remove the cost, it relocated it into a background process. A
long-running transaction can quietly hold that janitor hostage and bloat the
whole database - a failure mode that only makes sense once you understand MVCC.

**3. The WAL-before-page rule is the quiet hero.**
Almost every durability and availability feature - crash recovery, replication,
PITR - is a consequence of one ordering constraint: log first, modify later.
Implementing it myself showed me that durability is mechanical, not magical.

**4. The planner is statistics-bound, not algorithm-bound.**
PostgreSQL knows hash join, merge join, and nested-loop. Which one it picks is
decided entirely by estimated row counts from `pg_statistic`. Good plans are
really a story about good statistics; that's why `ANALYZE` matters so much.

**5. Approximations beat perfection at scale.**
ClockSweep instead of exact LRU, cost *estimates* instead of exact counts - the
recurring theme is "a cheap approximation that's right often enough beats an
exact answer that's too expensive to compute." That's an engineering instinct I
picked up here and reused throughout my own MiniDB.

**6. Building a small one teaches the big one.**
Writing my own buffer pool, B+ tree, MVCC, WAL, and planner made PostgreSQL's
internals readable. The concepts transfer almost one-to-one - the difference is
that PostgreSQL handles the thousand edge cases (concurrency during splits,
wraparound, partial-page writes) that I got to ignore.

---

## References

- PostgreSQL Documentation - *Internals: Buffer Manager, MVCC, WAL, Planner* - https://www.postgresql.org/docs/current/internals.html
- PostgreSQL source - `src/backend/storage/buffer/`, `src/backend/access/nbtree/`, `src/backend/access/heap/`
- P. Lehman & S. B. Yao, *Efficient Locking for Concurrent Operations on B-Trees* (1981)
- PostgreSQL Wiki - *MVCC* and *VACUUM* - https://wiki.postgresql.org/wiki/MVCC
- Alex Petrov, *Database Internals* (O'Reilly, 2019)
- B. Momjian, *MVCC Unmasked* and *WAL Internals* talks
- My capstone implementation, MiniDB (`MiniDB_Projects/Team_HarshDB/`) - buffer pool, B+ tree, MVCC, WAL, and a cost-based planner built from scratch
