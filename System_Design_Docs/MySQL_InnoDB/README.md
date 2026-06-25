# MySQL / InnoDB Storage Engine

**Author:** Swaim Sahay
**Roll Number:** 24bcs10335
**Course:** Advanced DBMS - System Design Discussion

> The single sentence that unlocked this topic for me: **InnoDB stores the table
> *inside* the primary-key B+ tree, and PostgreSQL stores the table separately
> from every index.** That one structural difference explains why InnoDB's
> primary-key lookups are so fast, why its secondary indexes work the strange way
> they do, and why it needs *two* logs (undo + redo) where PostgreSQL's MVCC gets
> by differently. Having studied both designs, reading InnoDB felt like seeing the
> "other branch" of the same family tree - same problems, opposite answers.

---

## 1. Problem Background

InnoDB became MySQL's default storage engine in 2010 (MySQL 5.5), replacing
MyISAM. MyISAM was fast for reads but had no transactions, no crash recovery,
and only table-level locking - fine for read-heavy websites in the early 2000s,
dangerous for anything that writes concurrently. InnoDB was created (originally
by Innobase Oy, later owned by Oracle) to give MySQL what serious applications
needed: **ACID transactions, row-level locking, crash recovery, and MVCC** -
while keeping MySQL's reputation for fast lookups.

The design InnoDB landed on is heavily influenced by Oracle and by the
realisation that most queries hit rows **by primary key**. So it optimised
ruthlessly for that case: the table itself is physically organised as a B+ tree
keyed on the primary key (a **clustered index**). Everything else - secondary
indexes, undo logs, redo logs, locking - is built around that central choice.

This makes InnoDB the perfect contrast to PostgreSQL. Both are mature,
transactional, MVCC databases, but they made nearly opposite structural and
versioning decisions, which is exactly what the course wants analysed.

---

## 2. Architecture Overview

### 2.1 High-level structure

InnoDB is a storage engine *inside* the MySQL server process (MySQL has a
pluggable storage-engine API; the SQL parser/optimizer sit above it). Within
InnoDB, the action splits between an in-memory area and on-disk structures, with
the buffer pool in the middle and two logs guarding correctness.

```
        SQL layer (parser, optimizer)  ── MySQL server
                     │  handler API
                     ▼
   ┌──────────────────────────────────────────────────────────┐
   │                       InnoDB engine                        │
   │                                                            │
   │   ┌──────────────────────────────────────────────────┐    │
   │   │            Buffer Pool (in-memory)                 │    │
   │   │  cached 16 KB pages, LRU (young/old sublists)      │    │
   │   │  change buffer, adaptive hash index                │    │
   │   └───────────────┬───────────────────┬────────────────┘    │
   │   writes redo ──► │                   │ ◄── undo pages cached │
   │                   ▼                   ▼                       │
   │   ┌────────────────┐   ┌────────────────────────────────┐    │
   │   │  Redo log       │   │  Tablespaces (.ibd files)      │    │
   │   │  (ib_logfile /  │   │   clustered index B+ tree      │    │
   │   │   #innodb_redo) │   │   secondary index B+ trees     │    │
   │   └────────────────┘   │   undo log segments            │    │
   │                        └────────────────────────────────┘    │
   └──────────────────────────────────────────────────────────┘
```

### 2.2 Main components

| Component | Role |
|---|---|
| **Clustered index** | The table itself, a B+ tree keyed on the primary key; leaf pages *are* the rows |
| **Secondary indexes** | B+ trees keyed on other columns; leaves store the **primary key**, not a physical pointer |
| **Buffer pool** | In-memory cache of 16 KB pages, LRU-based with a midpoint insertion strategy |
| **Redo log** | Physical-ish log of page changes → crash recovery (durability) |
| **Undo log** | Old row versions → rollback + MVCC read views |
| **Lock system** | Row locks, gap locks, next-key locks for isolation |

### 2.3 Data flow

A primary-key read (`WHERE id = 5`) descends the **clustered index** B+ tree and
finds the full row at the leaf - one traversal, done. A read on a secondary
column descends the **secondary index** to get the primary key, then descends the
clustered index again to fetch the row (a "**double lookup**"). Writes modify
pages in the buffer pool, append a **redo** record (for durability) and an
**undo** record (for rollback/MVCC), and are flushed to the `.ibd` tablespace
later.

---

## 3. Internal Design

### 3.1 Clustered index - the table *is* the index

This is InnoDB's defining feature. The table's rows are stored **in the leaf
nodes of a B+ tree ordered by the primary key**. There's no separate heap.

```
Clustered index (= the table), keyed on PK:

                 [ • 50 • 100 • ]
                /       |       \
   leaf:[id=10 | full row][id=20 | full row] ...   ← entire row lives here
```

Consequences:
- **PK lookup is one B+ tree traversal** and you have the whole row - no second
  fetch. This is the case InnoDB optimises hardest, and it's genuinely fast.
- **Rows are physically sorted by PK**, so range scans on the PK
  (`WHERE id BETWEEN ...`) read sequential leaf pages - cache- and disk-friendly.
- **Choice of primary key matters enormously.** Since the PK orders the whole
  table, a random PK (like a UUID) causes inserts to land in random leaf pages →
  page splits and fragmentation. A monotonic PK (auto-increment) appends to the
  rightmost leaf → tight packing. This is why InnoDB best practice is "use a
  small, monotonic primary key."
- If you don't declare a PK, InnoDB invents a hidden 6-byte `DB_ROW_ID` to order
  by - you get a clustered index whether you asked for one or not.

> **Contrast with PostgreSQL:** PostgreSQL stores rows in an **unordered heap**
> and keeps the index separate, pointing at rows by `(page, slot)`. InnoDB folds
> the two together. The heap approach makes "add another index" cheap and keeps
> updates from reorganising the table; the clustered approach makes PK access
> unbeatable. Same data, opposite layout.

### 3.2 Secondary indexes - and why they store the PK

A secondary index (say on `email`) is its own B+ tree keyed on `email`. But its
leaves do **not** store a physical row pointer. They store the **primary key
value**.

```
Secondary index on email:        Clustered index (PK = id):
  [ 'a@x' → id=10 ]   ──────────►  [ id=10 | full row ]
  [ 'b@y' → id=20 ]   ──────────►  [ id=20 | full row ]
```

So `WHERE email = 'a@x'` is a **two-step lookup**: search the secondary index to
get `id=10`, then search the clustered index for `id=10` to get the row.

**Why store the PK instead of a physical pointer?** Because rows move. In a
clustered index, a page split or row update can relocate a row to a different
physical page. If secondary indexes stored physical addresses, every such move
would force updating every secondary index. By storing the *logical* PK instead,
secondary indexes are stable across row movement - the indirection costs an extra
lookup but saves enormous maintenance churn. (A **covering index** avoids the
second step: if the secondary index already contains all the columns the query
needs, InnoDB never touches the clustered index.)

### 3.3 MVCC the Oracle way - undo logs, not extra heap versions

Both InnoDB and PostgreSQL do MVCC, but they store old versions completely
differently.

- **PostgreSQL:** writes a new tuple version *in the table itself*; old versions
  sit in the heap until VACUUM removes them.
- **InnoDB:** **updates the row in place** in the clustered index, and pushes the
  *previous* version into the **undo log**. Each clustered-index row carries two
  hidden fields: `DB_TRX_ID` (the transaction that last modified it) and
  `DB_ROLL_PTR` (a pointer into the undo log to the prior version).

To read under MVCC, a transaction uses a **read view** (its snapshot). When it
encounters a row whose `DB_TRX_ID` is too new to be visible, it follows
`DB_ROLL_PTR` back through the undo log, reconstructing the older version it's
allowed to see:

```
  clustered row (current):  [data v3 | DB_TRX_ID=30 | DB_ROLL_PTR ─┐]
                                                                   ▼
  undo log:                 [data v2 | trx=20 | roll_ptr ─┐]
                                                          ▼
                            [data v1 | trx=10 | roll_ptr = NULL]

  a reader whose snapshot < 20 walks back to v1.
```

The neat result: the **current** version stays compact in the table (no bloat of
the main structure), and old versions live in a separate, purgeable place. A
background **purge** thread deletes undo records once no read view needs them -
InnoDB's equivalent of VACUUM, but cleaning the undo log rather than the table.

### 3.4 The two logs - undo vs redo (the classic exam question)

InnoDB keeps **two** logs because they solve **two different problems**:

| | **Redo log** | **Undo log** |
|---|---|---|
| Question it answers | "We committed - how do we not lose it after a crash?" | "We need to take it back / show an old version" |
| Contains | Physical page changes (after-images) | Logical old row versions (before-images) |
| Used for | **Crash recovery (REDO)** - durability | **Rollback** + **MVCC reads** |
| Direction | Replays committed changes *forward* | Reconstructs/reverts changes *backward* |

The write path on COMMIT:

```
  change row in buffer pool (page now dirty)
        │
   write UNDO record (old version) ── into undo segment
   write REDO record (page change) ── into redo log buffer
        │
   at COMMIT: flush REDO log + fsync   ← durability point (WAL rule)
        │
   (later) dirty data pages flushed to .ibd by background flushing
   (crash) replay REDO from last checkpoint → committed work restored
           uncommitted work rolled back using UNDO
```

This is **ARIES**-style recovery: REDO brings the database forward to the moment
of the crash (including changes from transactions that hadn't flushed their data
pages), then UNDO rolls back anything that was in-flight and never committed.
PostgreSQL, by contrast, needs no UNDO pass because its uncommitted versions are
simply invisible via `xmin`/`xmax` - a direct consequence of the heap-versioning
choice.

> **From studying MiniDB:** I implemented **redo-only** recovery (no-force + no-steal), so
> I never needed an undo pass - exactly PostgreSQL's situation. Studying InnoDB
> showed me the *other* valid design: allow in-place updates and dirty-page steal,
> and pay for it with an undo log + an undo recovery pass. Neither is "more
> correct" - they're different points on the same trade-off curve.

### 3.5 Locking - rows, gaps, and next-key locks

InnoDB does **row-level locking** (shared `S` / exclusive `X`), held until the
transaction ends (Strict 2PL). But the famous part is **gap locks** and
**next-key locks**, which exist to stop **phantom reads** under the REPEATABLE
READ isolation level.

- A **record lock** locks an existing index row.
- A **gap lock** locks the *space between* index values - nothing is there yet,
  but the lock prevents another transaction from *inserting* into that gap.
- A **next-key lock** = record lock + the gap before it. This is InnoDB's default
  locking unit for range scans under REPEATABLE READ.

Example: under REPEATABLE READ, `SELECT ... WHERE id BETWEEN 10 AND 20 FOR
UPDATE` next-key-locks that whole range, so no one can insert `id=15` until you
commit. Without gap locks, a re-run of the same range query could see a new
"phantom" row.

```
  index values:   ... 10        20        30 ...
  next-key lock on (10,20]:  locks row 20 AND the gap (10..20)
                             → INSERT id=15 blocks until lock released
```

### 3.6 The buffer pool - LRU with a twist

InnoDB caches 16 KB pages in its buffer pool using an **LRU list split into two
sublists**: a "young" (hot) end and an "old" end. Newly read pages are inserted
at the **midpoint** (head of the old sublist), not the very top. Only if a page
is accessed *again* after a short delay is it promoted to the young end.

**Why the midpoint trick?** To stop a one-off **full table scan** from evicting
the genuinely hot working set. A big scan brings in many pages used exactly once;
inserting them at the midpoint means they age out of the old sublist quickly
without ever displacing the hot pages at the young end. (This is InnoDB's answer
to the same "scan resistance" problem PostgreSQL solves with the ClockSweep
usage-count cap - different mechanism, same goal.)

Two more buffer-pool features worth naming: the **change buffer** (batches
secondary-index updates for pages not currently in memory, applying them later to
save random I/O) and the **adaptive hash index** (InnoDB notices hot index
lookups and builds an in-memory hash on top of the B+ tree for them).

---

## 4. Design Trade-Offs

### Clustered index: advantages and the price

**Advantages:** primary-key reads and PK range scans are extremely fast (one
traversal, sequential leaves). Great for the overwhelmingly common "fetch by id"
and "scan a key range" workloads.

**Costs:** (1) secondary indexes pay a double lookup and are larger (they embed
the PK). (2) a large or random primary key (UUIDs) hurts *everything*, because
the PK is copied into every secondary index and random inserts fragment the
table. (3) updating the PK is expensive - it physically moves the row.

### Undo+redo vs PostgreSQL's heap versioning

**InnoDB advantage:** the live table stays compact (current versions in place,
old ones off in undo), so the main structure doesn't bloat the way a PostgreSQL
heap does, and PK access touches one tight B+ tree.

**InnoDB cost:** more moving parts - two logs, a purge thread, and undo
"history" that a long-running transaction can balloon (the same long-txn hazard
PostgreSQL has, relocated to the undo tablespace). Reconstructing old versions
means chasing roll pointers, so heavy MVCC reads do extra work.

### Row + gap locking

**Advantage:** real concurrency at row granularity plus phantom protection under
REPEATABLE READ without resorting to full serializability everywhere.

**Cost:** gap/next-key locks lock *empty space*, which can block inserts that
"feel" unrelated and cause surprising deadlocks. They're one of the most common
sources of confusing InnoDB lock-wait timeouts in production.

### Isolation levels

InnoDB defaults to **REPEATABLE READ** (with gap locks preventing most
phantoms), while PostgreSQL defaults to **READ COMMITTED**. InnoDB's RR uses a
**consistent read view** taken at the first read of the transaction; PostgreSQL's
READ COMMITTED takes a fresh snapshot per statement. This is a visible behavioural
difference that trips up developers porting between them.

---

## 5. Experiments / Observations

### 5.1 Primary-key vs secondary-key lookup

Conceptually compare two queries on a table with a PK `id` and an index on
`email`:

- `SELECT * FROM users WHERE id = 5;` → one clustered-index traversal; the row is
  at the leaf. Minimum work.
- `SELECT * FROM users WHERE email = 'x';` → secondary-index traversal yields
  `id`, then a clustered-index traversal for the row. Two B+ tree descents.

`EXPLAIN` reflects this: the PK query shows `type: const`/`eq_ref` touching the
`PRIMARY` key; the email query shows a lookup on the secondary index. If you add
a **covering index** `(email, name)` and select only `email, name`, `EXPLAIN`
shows `Using index` - the second lookup disappears entirely because every needed
column is already in the secondary index. This is the clearest demonstration of
why clustered storage makes secondary indexes behave the way they do.

### 5.2 The auto-increment vs UUID primary key effect

Insert a million rows with an auto-increment PK, then repeat with a random UUID
PK. The auto-increment table stays compact and inserts stay fast: every new row
appends to the rightmost leaf page. The UUID table fragments: random keys force
inserts into the middle of the B+ tree, triggering page splits and leaving pages
half-full. You can see the difference in the table's on-disk size and in
`SHOW TABLE STATUS` (data length vs row count). This is a direct, reproducible
consequence of the clustered-index design - and the reason "always use a
monotonic surrogate key" is repeated so often in MySQL circles.

### 5.3 Watching a deadlock and reading `SHOW ENGINE INNODB STATUS`

Two transactions updating the same two rows in opposite order will deadlock.
InnoDB detects the cycle (waits-for graph), **rolls back the cheaper transaction**,
and lets the other proceed. `SHOW ENGINE INNODB STATUS` prints a `LATEST DETECTED
DEADLOCK` section naming both transactions, the locks each held and waited for,
and which was rolled back. Reading that output is the most concrete way to *see*
row locks, gap locks, and deadlock victim selection in action.

### 5.4 Crash recovery with two logs

Kill `mysqld` mid-transaction and restart. InnoDB's recovery does two passes:
**REDO** from the last checkpoint (re-applying committed page changes from the
redo log, including ones whose data pages never reached disk), then **UNDO**
(rolling back the transaction that was in flight, using the undo log). The server
is consistent before accepting queries. I'd already seen the REDO half in my own
engine; the eye-opener was that InnoDB *needs* the UNDO half precisely because it
allows in-place updates and steals dirty pages - the design choice creates the
recovery requirement.

---

## 6. Key Learnings

**1. One structural choice (clustered storage) radiates outward.**
Storing the table inside the PK B+ tree explains fast PK reads, the secondary-
index double lookup, why secondary indexes store the PK, and why PK selection is
a performance decision. Like PostgreSQL's MVCC bet, it's the root that the rest
of the design grows from.

**2. "Why two logs?" finally has a clean answer.**
Redo = durability (replay committed changes forward after a crash). Undo =
rollback + MVCC (reconstruct old versions backward). They answer different
questions, which is why one log can't do both jobs in InnoDB's design.

**3. There are (at least) two legitimate ways to do MVCC.**
PostgreSQL keeps old versions *in the table* and vacuums them; InnoDB keeps the
current version in place and stashes old ones in the *undo log* with roll
pointers. Both give snapshot reads; they just bloat and clean different
structures. Seeing both cured me of thinking "MVCC" means one specific thing.

**4. Locking empty space is a real concept.**
Gap and next-key locks were the most surprising part - the idea that you lock the
*absence* of a row to prevent phantoms is subtle, and it explains a whole class
of InnoDB deadlocks that look mysterious until you know gaps are lockable.

**5. Buffer-pool design is mostly about resisting scans.**
InnoDB's midpoint-insertion LRU and PostgreSQL's usage-count ClockSweep are
different mechanisms aimed at the exact same enemy: a big sequential scan
flushing the hot working set. Cache replacement is less about "track recency
perfectly" and more about "don't let one scan wreck everything."

**6. Studying both designs made the contrast vivid.**
Because PostgreSQL uses heap + separate index + redo-only recovery, every InnoDB choice
read as "the road not taken." That comparison - same problems, deliberately
opposite answers - is the most useful thing I took from this topic.

---

## References

- MySQL Reference Manual - *InnoDB Storage Engine* (clustered/secondary indexes, buffer pool, redo/undo, locking) - https://dev.mysql.com/doc/refman/8.0/en/innodb-storage-engine.html
- J. Tuente / MySQL Docs - *InnoDB Multi-Versioning* and *InnoDB Locking* - https://dev.mysql.com/doc/refman/8.0/en/innodb-multi-versioning.html
- C. Mohan et al., *ARIES: A Transaction Recovery Method*
- Baron Schwartz et al., *High Performance MySQL* (O'Reilly)
- Jeremy Cole, *InnoDB blog series* (page/record format, B+ tree internals)
- Alex Petrov, *Database Internals* (O'Reilly, 2019)
