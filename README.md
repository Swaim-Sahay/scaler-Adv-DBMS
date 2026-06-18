# Lab 6 - Transaction Manager with MVCC, Strict 2PL and Deadlock Detection

## Author

24BCS10335 — Swaim Sahay

---

# Overview

This project implements an in-memory transaction manager that combines three important database concurrency control mechanisms:

1. **Multi-Version Concurrency Control (MVCC)**
2. **Strict Two-Phase Locking (Strict 2PL)**
3. **Deadlock Detection using a Waits-For Graph**

The goal is to simulate how modern database systems manage concurrent transactions while preserving consistency and isolation.

---

# Objectives

* Understand transaction lifecycle management.
* Implement snapshot-based reads using MVCC.
* Prevent conflicting writes using Strict 2PL.
* Detect deadlocks among waiting transactions.
* Maintain version chains for database records.
* Support transaction commit and rollback operations.

---

# Features

## MVCC Version Chains

Each logical record maintains multiple historical versions.

Example:

```text
Record A

Version 3 (latest)
    |
Version 2
    |
Version 1
```

Readers access the version visible to their transaction snapshot without blocking writers.

Benefits:

* Non-blocking reads
* Improved concurrency
* Consistent snapshots

---

## Strict Two-Phase Locking (Strict 2PL)

The system supports:

### Shared Locks (S)

Used for read operations.

Multiple transactions may hold a shared lock simultaneously.

### Exclusive Locks (X)

Used for write operations.

Only one transaction may hold an exclusive lock at a time.

### Lock Upgrade

A transaction holding a shared lock may request promotion to an exclusive lock when permitted.

### Strict Rule

All locks are released only after:

* COMMIT
* ROLLBACK

This prevents cascading aborts.

---

## Deadlock Detection

A waits-for graph is maintained.

Example:

```text
T1 → T2
T2 → T3
T3 → T1
```

The cycle indicates a deadlock.

The transaction manager:

1. Detects cycles.
2. Chooses a victim transaction.
3. Aborts the victim.
4. Releases its locks.
5. Allows remaining transactions to continue.

---

# Architecture

The transaction manager consists of the following components:

### Transaction Table

Stores:

* Transaction ID
* Status
* Snapshot timestamp
* Lock information

---

### Version Store

Maintains version chains for records.

Each version contains:

* Value
* Creating transaction
* Commit timestamp

---

### Lock Manager

Responsible for:

* Shared lock acquisition
* Exclusive lock acquisition
* Lock upgrades
* Lock release

---

### Deadlock Detector

Maintains:

* Waits-for graph
* Cycle detection logic
* Victim selection

---

# Transaction Lifecycle

## Begin Transaction

A unique transaction identifier is generated.

```text
BEGIN T1
```

---

## Read

The transaction reads the latest visible committed version.

```text
READ(A)
```

---

## Write

The transaction creates a new uncommitted version.

```text
WRITE(A)
```

---

## Commit

The transaction:

1. Commits new versions.
2. Releases all locks.
3. Becomes durable.

```text
COMMIT T1
```

---

## Rollback

The transaction:

1. Discards uncommitted changes.
2. Releases locks.
3. Restores consistency.

```text
ROLLBACK T1
```

---

# Demonstrated Scenarios

The implementation demonstrates:

### Snapshot Visibility

Readers observe a consistent snapshot.

### Concurrent Readers

Multiple transactions acquire shared locks simultaneously.

### Exclusive Lock Blocking

Conflicting writes are blocked.

### Lock Promotion

Shared locks are upgraded to exclusive locks.

### Deadlock Recovery

Cycles are detected and resolved automatically.

### Write Conflict Handling

Conflicting updates are prevented.

### Version Cleanup

Obsolete versions are removed during garbage collection.

---

# Compilation

Compile the project:

```bash
make
```

or

```bash
g++ -std=c++17 -Wall -Wextra -pthread -O2 main.cpp -o transaction_engine
```

---

# Running

Execute the program:

```bash
make run
```

or

```bash
./transaction_engine
```

---

# Expected Learning Outcomes

After completing this lab, the following concepts should be understood:

* ACID Transactions
* Concurrency Control
* MVCC
* Snapshot Isolation
* Strict Two-Phase Locking
* Lock Compatibility
* Deadlock Detection
* Waits-For Graphs
* Transaction Recovery
* Version Management

---

# Conclusion

This project demonstrates how database systems coordinate concurrent transactions using MVCC and Strict Two-Phase Locking while ensuring correctness through deadlock detection and recovery. The implementation provides a simplified but practical model of the transaction processing techniques used in modern relational database systems.
