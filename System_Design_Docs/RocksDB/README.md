# RocksDB Architecture (LSM-Tree Storage)

**Author:** Swaim Sahay
**Roll Number:** 24bcs10335
**Course:** Advanced DBMS - System Design Discussion

> Every other database I studied this term - SQLite, PostgreSQL, InnoDB - is
> built on a B-tree, a structure that updates data *in place*. RocksDB throws
> that out. Its core idea is almost rebellious: **never modify data on disk; only
> ever append.** Writes go to memory and a log, get flushed as sorted immutable
> files, and a background process later merges those files. The whole design is a
> bet that *random writes are the enemy* and that turning every write into a
> sequential append - then paying to clean up later - is the better trade. After
> studying a B-tree-based engine myself, studying RocksDB felt like learning a
> second, totally different philosophy of how a database can work.

---

## 1. Problem Background

RocksDB was created at Facebook (Meta) around 2012, forked from Google's
LevelDB, and tuned for fast storage (SSDs/flash) and server workloads. It is not
a full SQL database - it's an **embeddable key-value storage engine** (a C++
library you link into your app, like SQLite but key-value). It powers the storage
layer underneath many bigger systems: MySQL (MyRocks), CockroachDB, TiKV, Kafka
Streams, and countless internal services.

The problem it targets is **write-heavy workloads on flash storage**. B-trees
update pages in place, which on a heavy write load means lots of **random** page
writes - and random writes are exactly what hurts on SSDs (write amplification,
wear) and what was catastrophic on spinning disks. The **LSM-tree (Log-Structured
Merge-tree)** answers this by converting random writes into **sequential** writes:
buffer in memory, append to a log, flush sorted runs, and merge them in the
background. RocksDB is essentially a highly engineered, configurable LSM-tree.

Why study it: it represents the *other* major storage-engine family. B-trees
optimise for reads and in-place updates; LSM-trees optimise for writes and accept
more complex reads. Understanding both is understanding the central storage trade-
off in modern databases.

---

## 2. Architecture Overview

### 2.1 The LSM data flow

Data flows in one direction - **memory → log → flushed file → merged into deeper
levels** - and is never edited in place along the way.

```
   write(key,value)
        │
        ├──────────────► WAL (append-only log on disk)   ← durability
        ▼
   ┌─────────────┐  full   ┌──────────────────────┐  flush  ┌──────────────┐
   │  MemTable    │ ──────► │ Immutable MemTable(s) │ ──────► │  L0 SSTables │
   │ (in-memory,  │         │ (read-only, awaiting  │         │ (sorted, on  │
   │  sorted map) │         │  flush)               │         │  disk)       │
   └─────────────┘         └──────────────────────┘         └──────┬───────┘
                                                                   │ compaction
                                                                   ▼
                                          L1 ──► L2 ──► ... ──► Ln  SSTables
                                          (each level ~10× larger, sorted,
                                           non-overlapping key ranges)
```

### 2.2 Components

| Component | Where | Role |
|---|---|---|
| **MemTable** | RAM | Active in-memory sorted structure (skip list) receiving all writes |
| **Immutable MemTable** | RAM | A full MemTable, frozen, waiting to be flushed |
| **WAL** | Disk | Append-only log so a crash doesn't lose the in-memory MemTable |
| **SSTable** | Disk | Sorted String Table - an immutable file of sorted key→value, with a block index and Bloom filter |
| **Levels L0..Ln** | Disk | Tiers of SSTables; deeper levels are larger and (below L0) have non-overlapping key ranges |
| **Compaction** | Background | Merges SSTables, drops obsolete/deleted keys, pushes data downward |
| **Bloom filter** | In SSTables | Probabilistic "is this key maybe here?" to skip files on reads |

### 2.3 Read vs write paths in one glance

- **Write:** append to WAL → insert into MemTable. That's it - the write returns.
  Everything else (flush, compaction) happens later in the background.
- **Read:** check MemTable → immutable MemTables → L0 files → L1..Ln, using Bloom
  filters to skip files that definitely don't contain the key, stopping at the
  first (newest) version found.

---

## 3. Internal Design

### 3.1 The write path - why it's so fast

A write does only two cheap things:

1. **Append to the WAL** (sequential disk write - fast even on slow media).
2. **Insert into the MemTable** (an in-memory sorted **skip list** - O(log n),
   no disk I/O).

Then it returns. There is **no in-place update, no page split, no random I/O** on
the write path. When the MemTable fills (default ~64 MB), it's marked
**immutable**, a fresh MemTable takes over, and a background thread flushes the
immutable one to disk as an **L0 SSTable**. Because the MemTable is already
sorted, the flush is a single sequential write of a sorted file.

> **Contrast with B-tree engines:** in a B-tree engine, an insert finds the right leaf
> page and may trigger a **page split** - a random, in-place modification. RocksDB
> never does that. Every write is an append. This is the whole reason LSM-trees
> win on write throughput: they trade "do the work now, in place" for "append now,
> reorganise later in bulk."

**Updates and deletes are also just appends.** An update writes a new key→value
record (the newer one shadows the older). A delete writes a special **tombstone**
record. Nothing on disk is modified; the old value simply becomes obsolete and is
physically removed only when compaction eventually rewrites that file.

### 3.2 SSTables - immutable sorted files

An SSTable (Sorted String Table) is the on-disk unit. Inside it:

```
  ┌──────────────────────────────────────────┐
  │ data blocks: sorted key→value (~4-16 KB)   │
  ├──────────────────────────────────────────┤
  │ index block: first key of each data block  │  ← binary search to find a block
  ├──────────────────────────────────────────┤
  │ Bloom filter block                         │  ← "is key maybe here?"
  ├──────────────────────────────────────────┤
  │ footer: offsets of index/filter blocks     │
  └──────────────────────────────────────────┘
```

Because the file is sorted and immutable, a lookup is: consult the index block to
find the right data block, then binary-search within it. Immutability is what
makes SSTables safe to read without locks and easy to cache, replicate, and back
up - but it's also *why* compaction must exist (you can't edit them, so you
rewrite them).

### 3.3 Levels and compaction - the "merge" in LSM

L0 files come straight from MemTable flushes, so their key ranges can **overlap**
(two L0 files might both contain key `42`). Below L0, each level holds SSTables
with **non-overlapping** key ranges, and each level is roughly **10× larger** than
the one above.

**Compaction** is the background process that keeps this organised. It picks
SSTables from level Lₙ (and the overlapping ones in Lₙ₊₁), **merge-sorts** them,
drops obsolete versions and tombstoned keys, and writes fresh non-overlapping
SSTables into Lₙ₊₁.

```
   L0:  [a..z][c..m][b..p]   (overlapping, from flushes)
            │ compaction merge-sorts + dedups
            ▼
   L1:  [a..f][g..m][n..z]   (non-overlapping, sorted)
            │ as L1 fills, compact into L2 ...
            ▼
   L2:  ... (10× bigger, still non-overlapping)
```

**Why compaction is required:** without it, reads would have to check an
ever-growing pile of overlapping files, deleted keys would never actually free
space, and obsolete versions would accumulate forever. Compaction is the cleanup
bill the LSM design defers from write time.

**Compaction strategies** are a core tuning knob:
- **Leveled** (default): tight, non-overlapping levels → good read & space
  efficiency, but more write amplification (data gets rewritten as it moves down).
- **Universal/Tiered:** fewer, larger merges → lower write amplification, but more
  overlapping files → worse read and space amplification.

### 3.4 The read path - and why it's harder than a B-tree's

A single key can exist in several places (newest first): the MemTable, the
immutable MemTables, then L0 files, then L1..Ln. A read must check them **in
newest-to-oldest order** and stop at the first hit (which might be a tombstone,
meaning "deleted"):

```
  get(key):
    MemTable?           → if found, return
    immutable MemTables? → if found, return
    L0 files (newest)?   → check each (they overlap)
    L1, L2, ... Ln?      → one file per level (non-overlapping → binary search)
    not found anywhere   → key doesn't exist
```

The danger is obvious: a read might touch many files. Two mechanisms rescue it:

- **Bloom filters:** each SSTable has a compact probabilistic filter. Before
  reading a file, RocksDB asks the filter "could this key be here?" If it says no
  (which it can guarantee), the file is skipped entirely - no disk read. It can
  give false positives (rarely says "maybe" when the key is absent) but never
  false negatives. This is what keeps point lookups fast despite many files.
- **Block cache:** hot data blocks (and filter/index blocks) are cached in RAM,
  so repeated reads avoid disk.

> **From studying MiniDB:** a B-tree `get` is one downward traversal to a single
> place. RocksDB's `get` may consult many files across levels. That single
> comparison captures the whole LSM trade-off: writes got simpler and faster,
> reads got more complicated and need Bloom filters to stay fast.

### 3.5 Durability - WAL again, same rule

RocksDB's WAL plays the identical role it does in PostgreSQL/InnoDB: the MemTable
lives in volatile memory, so each write is **first appended to the WAL** on disk.
If the process crashes before the MemTable is flushed to an SSTable, recovery
**replays the WAL** to rebuild the lost MemTable. Once a MemTable is safely
flushed to an SSTable, the WAL segment that covered it can be discarded. (RocksDB
lets you tune the fsync policy per write - full durability vs higher throughput -
which is the same durability/performance dial every WAL exposes.)

---

## 4. Design Trade-Offs

The cleanest way to reason about LSM-trees is the **three amplifications** - the
three costs you trade between:

| Amplification | What it means | LSM behaviour |
|---|---|---|
| **Write** | bytes written to disk ÷ bytes the user wrote | **High** - data is rewritten repeatedly as compaction moves it down levels |
| **Read** | files/blocks read ÷ what one ideal read needs | **Higher than a B-tree** - a key may live across levels (mitigated by Bloom filters) |
| **Space** | disk used ÷ live data size | **Temporarily high** - obsolete versions/tombstones linger until compaction reclaims them |

You cannot minimise all three at once - improving one usually worsens another,
and the compaction strategy is how you choose the balance.

### Where LSM-trees win

**Write throughput.** Turning every write into a sequential append + an in-memory
insert means enormous ingest rates, far beyond what an in-place B-tree sustains
under random keys. For logging, time-series, event streams, and metrics - write-
dominated workloads - this is the right structure.

**SSD-friendliness.** Sequential writes and bulk rewrites are kinder to flash
(less random write wear) than a B-tree's scattered in-place page updates.

**Good compression.** SSTables are immutable, sorted blocks - ideal for block
compression, so LSM stores often use less space for the *live* data than a
B-tree (between compactions).

### Where LSM-trees struggle

**Read amplification.** A point read may probe multiple levels; a range scan must
merge across all of them. Bloom filters fix point lookups but **don't help range
scans** (a Bloom filter answers "is *this exact key* present?", not "is anything
in this range?"). Range-heavy read workloads are where B-trees still win.

**Compaction cost and "stalls."** Compaction consumes CPU and disk bandwidth in
the background. Under a write burst, if flushes/compactions can't keep up, RocksDB
**throttles or stalls** incoming writes to let the LSM catch up - latency spikes
that B-trees don't have.

**Space overhead between compactions.** Deleted data isn't really gone until a
compaction rewrites its file, so disk usage can temporarily exceed live data.

### The honest summary

> LSM-trees move work from **write time to background time**. You get cheap,
> sequential, fast writes now, and you pay later with compaction (CPU + write
> amplification) and at read time (multiple levels + Bloom filters). B-trees do
> the opposite - pay at write time for cheap reads. Choose LSM for write-heavy or
> SSD-bound workloads; choose a B-tree for read- and range-heavy ones.

---

## 5. Experiments / Observations

### 5.1 The recommended exercise - measuring the three amplifications

Running `db_bench` (RocksDB's benchmark tool) under different compaction
strategies makes the trade-offs measurable:

- **Leveled compaction** typically shows **higher write amplification** (often
  10-30× on heavy workloads, because a byte gets rewritten each time it descends a
  level) but **lower space and read amplification** (tight, non-overlapping
  levels, fewer files to check).
- **Universal/tiered compaction** shows **lower write amplification** (fewer, big
  merges) but **higher space amplification** (more overlapping data lingers) and
  often worse read amplification.

The reason this matters: there is no globally "best" setting - you pick the
compaction strategy to match whether your bottleneck is write bandwidth, read
latency, or disk capacity. The benchmark numbers move in opposite directions,
which is the whole point of the three-amplification model.

### 5.2 Writes stay fast, reads depend on level count

A simple observation: write throughput in an LSM stays high and fairly flat as
the dataset grows, because writes only ever hit the MemTable + WAL regardless of
how big the database is. Read latency, however, tends to **grow with the number of
levels** - more levels = potentially more files to consult. This is the inverse
of a B-tree, where write cost grows (deeper tree, more splits) while read cost
stays logarithmic. Watching these curves go opposite directions is the clearest
demonstration of the philosophical difference between the two families.

### 5.3 Bloom filters earning their keep

With Bloom filters **disabled**, a point lookup for a non-existent key must touch
one file per level to prove the key is absent - expensive. With them **enabled**,
each level's filter answers "definitely not here" and the file is skipped, so the
same negative lookup does almost no disk I/O. RocksDB exposes statistics
(`rocksdb.bloom.filter.useful`) counting how many reads a filter saved. This is a
direct, observable payoff of a probabilistic data structure - a few bits per key
turning a multi-file search into near-nothing.

### 5.4 A write burst triggering compaction stalls

Hammer RocksDB with a sustained write burst and watch the level statistics
(`rocksdb.stats`): L0 fills with overlapping files, pending-compaction bytes
climb, and if compaction can't keep up RocksDB starts **write stalls** -
deliberately slowing writers so the background merge can catch up. Seeing latency
spike *only* under sustained writes (not reads) made the deferred-work nature of
LSM concrete for me: the bill for cheap writes comes due during compaction.

---

## 6. Key Learnings

**1. "Append, never overwrite" is a whole different worldview.**
Every B-tree database I studied updates in place. RocksDB's refusal to do that -
writes are appends, files are immutable, cleanup is deferred - reorganises every
other decision (compaction, Bloom filters, multi-level reads). It's the LSM
counterpart to "MVCC is PostgreSQL's keystone."

**2. The three amplifications are the right mental model.**
Once I framed LSM-trees as "trade write vs read vs space amplification," the
compaction strategies stopped being arbitrary knobs and became points on a
clearly-shaped trade-off surface. You optimise one by spending another.

**3. LSM and B-tree are mirror images.**
B-trees pay at write time (splits, in-place I/O) for cheap reads. LSM-trees pay at
read time (multi-level lookups) and in the background (compaction) for cheap
writes. Neither is "better" - they're optimised for opposite workload shapes.

**4. Bloom filters are how LSM survives its own read path.**
Without them, reading across many levels would make LSM-trees uncompetitive for
lookups. A tiny probabilistic structure (no false negatives, rare false
positives) is what makes the design practical - a great example of trading a
little accuracy for a lot of speed.

**5. The same WAL idea shows up everywhere.**
RocksDB, PostgreSQL, and InnoDB all protect volatile in-memory state with an
append-only write-ahead log replayed on crash. I implemented this exact pattern in
my own engine. The structures above the WAL differ wildly (heap, clustered B+ tree, LSM),
but "log first, recover by replay" is universal.

**6. Deferred work is a powerful but visible trade.**
LSM's brilliance is postponing reorganisation to the background so writes are
instant. But the work doesn't vanish - it reappears as compaction CPU, write
amplification, and occasional write stalls. The most important lesson across this
whole course: in databases, you rarely remove a cost, you relocate it - and good
design is choosing *where* the cost is least painful for your workload.

---

## References

- RocksDB Wiki - *RocksDB Architecture, Compaction, Bloom Filters, WAL* - https://github.com/facebook/rocksdb/wiki
- P. O'Neil et al., *The Log-Structured Merge-Tree (LSM-Tree)* (1996)
- Google LevelDB documentation (RocksDB's ancestor) - https://github.com/google/leveldb
- S. Dong et al., *Optimizing Space Amplification in RocksDB* (CIDR 2017)
- Mark Callaghan, *small datum* blog - write/read/space amplification analyses
- Alex Petrov, *Database Internals* (O'Reilly, 2019) - LSM-tree chapters
