# PostgreSQL vs SQLite - Architecture Comparison

**Author:** Swaim Sahay
**Roll Number:** 24bcs10335
**Course:** Advanced DBMS - System Design Discussion

> When I started studying these two databases, I thought the differences would
> be surface-level - maybe just performance numbers or feature lists. What I
> actually found is that almost every difference between them comes down to one
> single question answered differently at design time: *does the database live
> inside your application, or does it run separately as a server?* SQLite said
> "inside." PostgreSQL said "as a server." Everything else - storage layout,
> concurrency, memory, recovery - falls out of that one choice.

---

## 1. Problem Background

### The problem each one was actually trying to solve

Here's a thing that doesn't get said enough: both databases are solving the
same surface-level problem (store and query relational data), but for
completely different real-world situations.

**SQLite (created in 2000 by D. Richard Hipp)**

Hipp built SQLite when he was working on a battleship management system that
needed a database - but couldn't have a server running. No DBA, no network,
no daemon. He needed the database to just be a file the application could
open and use. So he wrote a C library that does exactly that. The entire
database - all tables, indexes, schema - lives in one `.db` file. You don't
install SQLite, you don't start it, you don't configure it. Your app links
the library and calls functions. That's it.

This is why SQLite ended up in literally everything - Android, iOS, Firefox,
Chrome, WhatsApp, Python's standard library. It ships *inside* other software
rather than running alongside it.

**PostgreSQL (started as POSTGRES at UC Berkeley in 1986, went public in 1996)**

PostgreSQL was built for a fundamentally different scenario: what happens
when many users need to read and write shared data at the same time without
stepping on each other? A university research group needed something that
could handle real concurrent workloads, survive crashes without losing data,
and answer complex queries intelligently. So they built a proper server -
something that runs independently, accepts connections, and manages
concurrency explicitly.

### What this means for every design decision after it

| | SQLite | PostgreSQL |
|---|---|---|
| Starting assumption | One app, one user, no server possible | Many clients, many users, shared data |
| Core goal | Zero config, zero ops, maximum portability | Correctness and concurrency at scale |
| What it gives up | Concurrent writers, complex query planning | Operational simplicity |

Once you internalize this table, the rest of the document basically writes
itself. Every architectural choice in both databases is a logical consequence
of these starting assumptions.

---

## 2. Architecture Overview

### 2.1 How SQLite is structured - the embedded model

There is no SQLite process. When your code calls `sqlite3_open()`, you are
loading a library into your own process. Everything - SQL parsing, query
execution, disk I/O - happens right there in your application's memory space.

```
        ┌──────────────────────────────────────────────┐
        │               Your Application Process        │
        │                                               │
        │   your code ──(function call)──► SQLite lib   │
        │                                     │         │
        │                          ┌──────────▼──────┐  │
        │                          │  SQL Compiler   │  │
        │                          │  VDBE (bytecode)│  │
        │                          │  B-tree Engine  │  │
        │                          │  Pager / Cache  │  │
        │                          └──────────┬──────┘  │
        └─────────────────────────────────────┼─────────┘
                                              │ read() / write() / fsync()
                                              ▼
                                 ┌────────────────────────┐
                                 │    single .db file     │
                                 │  (+ -wal or -journal)  │
                                 └────────────────────────┘
```

The path a query takes: your SQL string → compiled into VDBE bytecode (think
of it as a tiny virtual machine's instruction set) → the bytecode drives the
B-tree layer → the pager fetches 4 KB chunks of the file into its cache →
rows come back to you. No network, no second process, no context switches
between processes. Just function calls.

### 2.2 How PostgreSQL is structured - the client-server model

PostgreSQL runs as a background daemon called `postmaster`. Your application
connects to it over a TCP socket (or a Unix socket if it's local). For every
connection, postmaster forks a dedicated backend process. All these backends
share a common block of memory called `shared_buffers`, and several background
processes hum along taking care of maintenance work.

```
  your app ──TCP / Unix socket──► postmaster (always-running daemon)
                                        │
                        forks one backend process per connection
          ┌─────────────────────────────┼────────────────────┐
          ▼                             ▼                     ▼
     backend #1                   backend #2            backend #N
     parse → plan → execute       parse → plan → execute   ...
          │                             │                     │
          └────────────┬────────────────┘─────────────────────┘
                       ▼
          ┌──────────────────────┐    ┌───────────────────────────┐
          │   shared_buffers     │    │  Background Workers:       │
          │  (shared page cache) │    │  WAL writer, checkpointer  │
          └──────────┬───────────┘    │  autovacuum, bg writer     │
                     │                └───────────────────────────┘
                     ▼
       data directory on disk:
         base/<db-oid>/<rel-oid>   ← heap files and index files
         pg_wal/                   ← write-ahead log segments
```

The path a query takes: SQL arrives on a socket → the backend parses it →
the cost-based planner looks at table statistics (stored in `pg_statistic`)
and picks an execution plan → the executor fetches pages through
`shared_buffers` → any changes go to WAL first → result streamed back over
the socket to the client.

### 2.3 Both at a glance

| Component | SQLite | PostgreSQL |
|---|---|---|
| Process model | In-process library - no separate process | Daemon + one backend per connection |
| Where data lives | One `.db` file | Directory of heap files, index files, WAL |
| Memory sharing | None - each connection has its own cache | `shared_buffers` shared across all backends |
| Background work | Nothing - it's a library | WAL writer, checkpointer, autovacuum, bgwriter |
| How client connects | Function call | TCP or Unix socket |

---

## 3. Internal Design

### 3.1 What the data looks like on disk

**SQLite - literally just pages stacked in a file**

The `.db` file is exactly N fixed-size pages placed one after another. By
default each page is 4 KB. Open any SQLite file in a hex editor and the very
first 16 bytes spell out `"SQLite format 3\000"` - that's the magic header.
Offset 16 in that header holds the page size (stored as a 2-byte big-endian
integer). Every table is a B-tree that spans some of these pages. The schema
itself (`sqlite_schema`) is just another B-tree that starts on page 1 and
maps table names to their root pages.

```
  offset   0        4096       8192      12288     16384
           ├─ Page 1 ─┼─ Page 2 ─┼─ Page 3 ─┼─ Page 4 ─┤ ...
           │ file hdr │  table   │  index   │  leaf    │
           │ + schema │  root    │  root    │  rows    │
           │ b-tree   │(interior)│(interior)│          │
           └──────────┴──────────┴──────────┴──────────┘
```

**PostgreSQL - a directory of separate files**

PostgreSQL doesn't put everything in one file. Each table gets its own heap
file at `base/<db-oid>/<relation-oid>`, split into 1 GB chunks if it gets
large. Each index is a separate file too. Then there's `pg_wal/` for the
write-ahead log, and system catalog files that store schema information
(`pg_class`, `pg_attribute`, etc.). Pages here are 8 KB instead of 4 KB.

The benefit of separate files is flexibility - a table and its indexes can
grow independently, WAL can be recycled separately from table data, and the
OS can manage them as distinct resources.

| | SQLite | PostgreSQL |
|---|---|---|
| Default page size | 4 KB | 8 KB |
| Files per table | 0 extra (all in one .db file) | 1 heap file + separate file per index |
| Schema stored in | `sqlite_schema` B-tree on page 1 | System catalog tables (`pg_class`, etc.) |

### 3.2 Inside a single page - the slotted page layout

Both databases use what's called a **slotted page** design. If you opened a
page in a hex editor, you'd see three zones:

```
  offset 0                                              offset 4095 (SQLite)
  ├──────────┬──────────────────┬─────────────────────┬──────────────┤
  │  Page    │  Cell pointers   │                     │  Row data    │
  │  header  │  (N × 2 bytes,   │  ← free space →     │  (grows ←    │
  │ (8-12 B) │  growing →)      │                     │  from end)   │
  └──────────┴──────────────────┴─────────────────────┴──────────────┘
```

The header has page metadata. The cell-pointer array is a list of 2-byte
offsets pointing to where each row starts inside the page. The actual row
data is packed from the opposite end. The middle is unused space.

Why this design? Because rows can be variable length (a name column might be
5 bytes for "Ali" and 20 bytes for "Christopher"). The pointer array gives
you a fixed-size slot per row regardless of how big the actual data is.

When free space runs out, the page **splits** - the B-tree gets a new page
and the data is redistributed. This is universal across real databases.

### 3.3 Indexes and how they relate to the actual rows

Both databases use B-trees for indexing, but the relationship between the
index and the data is very different.

**SQLite - the table is the index**

In SQLite, the table itself is stored as a B-tree sorted by `rowid` (an
implicit integer key every row gets). The row data lives at the **leaf nodes**
of this tree. When you do `INTEGER PRIMARY KEY`, that column becomes the
rowid. So a lookup by primary key is just: walk the tree, reach the leaf,
you're done - one tree traversal and the data is right there.

**PostgreSQL - the table and index are separate**

PostgreSQL stores the table as an unordered **heap** - rows are written
wherever there's space, in no particular order. Indexes are separate B-tree
files. Each index entry stores the search key alongside a **TID** (tuple
identifier), which is basically `(page number, slot number within the page)`.
So a lookup goes: search the index tree → get the TID → go to the heap page
at that position → read the row.

```
SQLite:    B-tree traversal ──────────────────────► row data at leaf
                                                     (one step)

PostgreSQL: index B-tree ──TID──► heap page ──slot──► row data
                                                     (two steps)
```

SQLite's approach is faster for primary key lookups. PostgreSQL's approach is
more flexible - you can have multiple indexes on different columns pointing
into the same heap, and the heap doesn't need to be re-sorted when an index
is added.

### 3.4 Concurrency - where the real divergence is

This is the part that matters most if you're choosing between them for a
real system.

**SQLite - one writer, everyone else waits**

SQLite uses OS-level file locks. When someone writes, they grab a lock on
the entire database file. Every other writer blocks until the lock is
released. Even in WAL mode (where a separate `-wal` file is used and readers
don't block the writer), there is still only **one writer at a time**. That's
a fundamental constraint of the design, not a bug they forgot to fix - it's
the trade-off they consciously made to keep everything simple.

For a to-do app on your phone, this is completely fine. For a web app with
thousands of concurrent users, it's a deal-breaker.

**PostgreSQL - multiple readers and writers, all at once**

PostgreSQL uses **MVCC** (Multi-Version Concurrency Control). The core idea
is: instead of locking a row while someone's updating it, keep multiple
versions of it and let different transactions see the version that was current
when they started.

Every row in PostgreSQL carries two hidden fields:
- **`xmin`** - the ID of the transaction that created this version of the row
- **`xmax`** - the ID of the transaction that deleted or replaced this version
  (empty if the row is still live)

When you run `UPDATE`, PostgreSQL doesn't touch the old row. It writes a brand
new version with the new data, marks the old version's `xmax` with your
transaction ID, and moves on. Both versions now exist on disk simultaneously.

Each transaction sees a **snapshot** - the state of the database as of when
it started. A row is visible to your snapshot if its `xmin` committed before
your snapshot started and its `xmax` either hasn't committed yet or doesn't
exist.

```
  txn 10 INSERTs a row:   (xmin=10, xmax=0)    ← visible to everyone after txn 10
  txn 15 UPDATEs that row:
      old version:         (xmin=10, xmax=15)   ← visible only to snapshots before txn 15
      new version:         (xmin=15, xmax=0)    ← visible to snapshots after txn 15
  txn 12 reads:  started before txn 15 → sees the old version (xmin=10)
  txn 17 reads:  started after txn 15  → sees the new version (xmin=15)
```

The result is that **readers never block writers and writers never block
readers**. It's a genuinely elegant solution - the cost is that dead old
versions pile up on disk and a background process called **VACUUM** has to
periodically clean them out.

| | SQLite | PostgreSQL |
|---|---|---|
| Concurrency model | Database-level file lock | MVCC - concurrent readers and writers |
| How UPDATE works | Modifies the row in place | Writes new version, marks old one dead |
| Readers vs writers | WAL mode: readers don't block writer | Neither ever blocks the other |
| Dead version cleanup | Not needed | VACUUM reclaims dead tuple space |

### 3.5 Surviving crashes - WAL in both systems

Both systems use a **write-ahead log** for crash safety, though PostgreSQL's
is more central to how the whole system works.

**The rule behind WAL:** Before you change data on disk, first write a record
describing that change to a log file. If you crash, replay the log.

**SQLite in WAL mode:** Changes go to a `-wal` file. On commit, it's
`fsync`-ed - that's your durability guarantee. A checkpoint later folds those
changes back into the main file. On crash, uncommitted WAL entries are just
ignored; committed ones are replayed.

**PostgreSQL:** WAL is a first-class subsystem. Every change - inserts,
updates, deletes, even index modifications - gets a WAL record before the
data page is touched. On `COMMIT`, WAL is `fsync`-ed. The heap page in
`shared_buffers` can stay dirty - the WAL record on disk is enough to survive
a crash.

```
  change happens → WAL record written to pg_wal/
                         │
                   WAL fsync at COMMIT time   ← this is where durability lands
                         │
       (later) checkpoint flushes dirty pages, old WAL recycled
       (after crash) replay WAL from last checkpoint → everything committed is recovered
```

A nice side effect of PostgreSQL's WAL: it also powers streaming replication
(send the WAL to standby servers in real time) and point-in-time recovery
(replay WAL up to any specific timestamp).

### 3.6 How each handles memory

**SQLite** keeps a private page cache per connection. No memory is shared
between different connections. You can optionally turn on `mmap_size` to
memory-map the file directly, which skips the kernel-to-userspace copy on
reads - useful on Linux with cold cache, but on macOS the OS page cache
already does this implicitly so the gain is often near zero.

**PostgreSQL** maintains one shared `shared_buffers` pool for all backends.
If backend #1 reads a frequently-used page into the pool, backend #2 finds it
there already - no disk read needed. This shared warming effect is a big
advantage under multi-user workloads. Cache replacement uses a clock-sweep
algorithm. A separate background writer process slowly flushes dirty pages so
that checkpoints don't have to do everything at once.

| | SQLite | PostgreSQL |
|---|---|---|
| Cache location | Private per-connection | Shared across all backends |
| Cache replacement | LRU-like | Clock-sweep |
| Shared memory | None | Yes - `shared_buffers` |
| Background flushing | None | bgwriter + checkpointer |

---

## 4. Design Trade-Offs

### Where SQLite wins

**It has zero operational overhead.** There's nothing to install, start,
monitor, or tune. You ship the library with your app, and the database is
just a file. For mobile apps, desktop apps, browser extensions, IoT devices,
and anything embedded - this is huge. The operational burden of running and
maintaining a server is real, and SQLite eliminates it entirely.

**The single-file model is genuinely useful.** You can copy a SQLite database
with `cp`. You can email it. You can version-control it. You can open it in a
hex editor. That portability has real value in many workflows.

**For simple queries, it's fast.** No socket, no serialisation, no process
context switch. A SELECT on an indexed column is a few function calls and a
couple of disk reads. When your bottleneck is query complexity rather than
concurrency, SQLite is competitive.

### Where SQLite struggles

**One writer at a time is genuinely limiting.** Under write contention from
multiple threads or processes, SQLite serialises everything. Throughput
doesn't scale with more CPU cores. For any system with meaningful concurrent
writes - an API serving many users, a background job writing while users are
reading - this becomes a real bottleneck.

**It has no query optimizer worth speaking of.** For joining large tables,
SQLite defaults to nested-loop joins. On a 500k × 1.5M row join, this can
take minutes or simply never finish. PostgreSQL handles the same query in
seconds using a hash join it chose automatically.

### Where PostgreSQL wins

**Real concurrency.** MVCC means a read-heavy dashboard query and a
write-heavy batch job can run at the same time without either one waiting for
the other. This isn't a minor performance win - it's a fundamentally
different capability.

**A smart query planner.** PostgreSQL collects statistics about table data
and uses them to choose the best execution strategy - hash join, merge join,
nested loop, parallel scan, whatever fits. The planner is one of the most
sophisticated parts of the system, and it makes a massive difference on
complex queries.

**WAL-powered reliability features.** Replication, standby failover, and
PITR all fall out of WAL for free. These matter in production systems where
data loss or downtime has real consequences.

### Where PostgreSQL struggles

**You have to run it.** A PostgreSQL deployment needs someone to set it up,
configure it, tune `shared_buffers` and `max_connections`, set up backups,
handle failover. That's real operational work. Comparing it to "just use
SQLite" isn't always fair, but the contrast is real.

**MVCC has a storage cost.** Dead tuple versions take up space. If VACUUM
doesn't run frequently enough, tables bloat. In the worst case, if a
long-running transaction prevents VACUUM from cleaning old versions, the
database can slow down noticeably.

**Process-per-connection doesn't scale to thousands of connections.** Each
backend is a full OS process using several MB of memory. At 1,000 connections
you're looking at gigabytes of memory just for process overhead - which is
why production PostgreSQL deployments almost always use a connection pooler
like PgBouncer in front of it.

### The honest summary

> SQLite makes the server problem disappear entirely, at the cost of
> concurrent writes and query sophistication. PostgreSQL solves both of those,
> at the cost of needing to be operated. If you're building something embedded
> or local, SQLite. If you're building a multi-user backend, PostgreSQL. The
> answer is obvious once you know which problem you're actually solving.

---

## 5. Experiments / Observations

### 5.1 Why PostgreSQL files are bigger for the same data

If you create the same table with the same 100,000 rows in both databases,
the PostgreSQL file will be noticeably larger. The reason isn't bloat - it's
architecture:

- Every PostgreSQL tuple carries `xmin`, `xmax`, and a few other system
  fields for MVCC visibility. Each row is larger than its SQLite equivalent
  even before any data is written.
- After UPDATE and DELETE operations, dead tuple versions accumulate until
  VACUUM runs. These dead tuples take up space.
- PostgreSQL's 8 KB pages pack rows less efficiently than SQLite's 4 KB pages
  for tables with small rows, because more space is wasted at the end of each
  page.

SQLite's in-place updates and lack of MVCC versioning mean it carries none
of this overhead. The smaller file size is the storage dividend of its
simpler concurrency model.

### 5.2 What happens under concurrent writes

Imagine 10 threads each trying to insert 1,000 rows simultaneously:

**SQLite (WAL mode):** Thread 1 grabs the write lock, threads 2–10 are
blocked waiting. Thread 1 commits, thread 2 gets the lock, and so on. Total
time is roughly the sum of all individual insert times - no parallelism. Under
high load you'll also see `SQLITE_BUSY` errors when a thread gives up waiting.

**PostgreSQL:** All 10 backends run their INSERT transactions concurrently.
Each one writes its own WAL records and tuple versions independently. They
only interact at commit time to ensure ordering. Total time approaches the
time of the longest single batch - genuine parallelism.

This isn't a benchmark you need to run to see - it's the direct consequence
of the locking model. SQLite serialises; PostgreSQL parallelises.

### 5.3 How query complexity changes the picture

For a point lookup like `SELECT * FROM users WHERE id = 5`:
- SQLite walks its B-tree: root → interior node → leaf → row returned. Fast.
- PostgreSQL searches its nbtree index, gets the TID, fetches the heap tuple.
  Slightly more machinery, but on a warm cache the difference is imperceptible.

At this scale, SQLite's advantage of "no server overhead" is measurable.

For a join like `orders JOIN order_items WHERE order_items.quantity > 100`
on datasets with hundreds of thousands of rows on each side:
- SQLite falls back to a nested-loop join - it checks every order against
  every qualifying order_item. Complexity is O(M × N). On large datasets this
  stops being practical.
- PostgreSQL's planner recognises this, builds a hash table on the smaller
  side, and probes it for each row on the larger side. Complexity drops to
  roughly O(M + N). The difference in wall-clock time can be minutes vs.
  seconds.

The planner is what makes PostgreSQL useful for analytics. Without it, raw
concurrency alone wouldn't be enough.

### 5.4 Crash recovery in practice

**SQLite:** Close a SQLite connection while a write is in progress (or kill
the process). On the next open, the library checks for a leftover `-wal` file.
If the last transaction wasn't fully committed, its WAL entries are discarded.
The database comes back exactly at the last committed state, every time.

**PostgreSQL:** Kill a backend mid-transaction (or crash the whole server).
On restart, the postmaster reads WAL from the last checkpoint and replays it
forward. Committed transactions reappear exactly as they were committed.
Uncommitted transactions leave no trace. The database is consistent from the
very first query after restart.

The difference worth noting: PostgreSQL's WAL replay is automatic and happens
as part of startup. There's no user action needed. For a production system,
this automatic recovery behaviour - combined with the ability to ship WAL to a
standby for replication - is a significant operational advantage.

---

## 6. Key Learnings

**1. One design decision really does explain almost everything.**
I kept expecting to find independent reasons for each difference between the
two systems. Instead, almost every difference traces back to the same root:
embedded library vs. client-server. Once you hold that in your mind, the storage
layout, the concurrency model, the memory architecture - they all make sense
as consequences rather than independent choices.

**2. MVCC is elegant but not free.**
The `xmin`/`xmax` trick is genuinely clever - you get concurrent readers and
writers without locking by just keeping multiple row versions around. But the
cost is real: storage grows, VACUUM has to run, and a long-running transaction
can hold back cleanup for the whole database. The cleverness of the design
doesn't eliminate the trade-off; it just moves it somewhere less obvious.

**3. "Which database is faster" is the wrong question.**
SQLite is faster than PostgreSQL for a simple point lookup on a local file.
PostgreSQL is faster than SQLite for a multi-table join over 500k rows.
Both statements are true simultaneously. The right question is "faster for
what?" - and the answer depends entirely on the workload shape.

**4. Shared memory matters more than it sounds.**
PostgreSQL's `shared_buffers` means that when one backend reads a hot page
into the cache, every other backend benefits. SQLite's per-connection cache
means each new connection starts cold. For any workload where many connections
access the same data, this shared warming effect translates directly into
lower disk I/O.

**5. The internals are more similar than the marketing suggests.**
Slotted pages, B-tree indexes, and write-ahead logging show up in both
systems. The concepts aren't PostgreSQL-specific or SQLite-specific - they're
database-general. Learning how SQLite lays out pages on disk transfers almost
directly to understanding how PostgreSQL does it, because they're both solving
the same sub-problem: pack variable-length rows into fixed-size disk blocks.

**6. Simplicity is a legitimate engineering goal.**
SQLite's one-writer-at-a-time constraint is easy to dismiss as a limitation.
But it was a deliberate choice in exchange for something genuinely valuable:
zero operational complexity. The fact that SQLite runs reliably in billions of
devices with zero maintenance is not an accident - it's the payoff for that
trade-off. Knowing when to trade capability for simplicity is one of the
harder engineering judgment calls, and SQLite is a good example of getting it
right.

---

## References

- D. R. Hipp, *SQLite Database File Format* - https://www.sqlite.org/fileformat.html
- *SQLite WAL Mode* - https://www.sqlite.org/wal.html
- PostgreSQL Documentation - *Database Page Layout, MVCC, WAL, Buffer Manager* - https://www.postgresql.org/docs/current/
- Alex Petrov, *Database Internals* (O'Reilly, 2019)
- PostgreSQL Wiki - *MVCC* - https://wiki.postgresql.org/wiki/MVCC
- *The Definitive Guide to SQLite* - Mike Owens & Grant Allen
