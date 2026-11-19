# Stage 0 — First principles

## 1. Start with the correctness goal

Suppose two clients concurrently transfer money from the same account. A
distributed database must prevent both clients from reading the same old
balance and independently overwriting one another. The required abstraction is
not merely “only one thread enters a critical section.” It is:

> All reads and writes of a committed read-write transaction appear as one
> atomic operation in a single serial order, even when the affected data lives
> on different replicated servers.

Locks are one mechanism used to establish that order. They are only part of the
transaction protocol: replication preserves decisions, two-phase commit joins
shards, MVCC stores versions, and bounded clock uncertainty supports an
externally consistent commit timestamp.

## 2. This is not Chubby

Two different ideas are often called a “Spanner lock service”:

- **Chubby-like coordination locks** are coarse, long-lived application or
  infrastructure locks, such as leader election and configuration ownership.
- **Spanner transaction locks** are fine-grained internal locks on database
  cells/key ranges, held for the lifetime of a read-write transaction.

This project implements the second model. Exposing these locks as application
mutexes would couple user availability to database transaction internals and
would be the wrong API.

## 3. What is a lockable object?

The reference key is:

```text
Cell = (table, primary-key/row, column)
```

Cell granularity matters. Reading unused columns expands the lock footprint and
increases contention. Production Spanner also reasons about row-existence
cells and key ranges. This implementation keeps `Cell` exact so the concurrency
algorithm remains visible; a production storage adapter must add a range index
and canonical key encoding.

## 4. Lock modes

The compatibility model follows the documented Spanner modes:

| Held \ Requested | ReaderShared | WriterShared | Exclusive | WriterSharedTimestamp |
|---|---:|---:|---:|---:|
| ReaderShared | compatible | conflict | conflict | conflict |
| WriterShared | conflict | compatible | conflict | not a valid pairing |
| Exclusive | conflict | conflict | conflict | not a valid pairing |
| WriterSharedTimestamp | conflict | not a valid pairing | not a valid pairing | conflict |

The code conservatively treats every “not a valid pairing” as incompatible.
That is safer for a general lock API than silently allowing a combination the
storage layer should never request.

An `Exclusive` lock is the effective combination of a transaction's
`ReaderShared` and `WriterShared` ownership on the same cell.

## 5. The transaction identity has two dimensions

```text
transaction id  = uniqueness and idempotency
transaction age = deadlock-prevention priority
```

The age must remain stable across all shards and retries. Smaller means older.
Changing age on each retry can starve a repeatedly aborted transaction; a
production client normally carries forward or increases retry priority.

## 6. Safety invariants

These are the rules the implementation is organized around:

1. No two incompatible holders exist for one cell.
2. A transaction is never both committed and aborted.
3. All replicas apply the same command at the same log position and reach the
   same fingerprint.
4. A shard votes “prepared” only after all its requested locks are granted and
   its local transaction state is ready to commit.
5. Prepared locks are retained until an immutable coordinator decision is
   learned.
6. A commit decision is recorded before any participant is told to commit.
7. Recovery replays an existing decision; it never manufactures a different
   one.
8. Success is returned only after commit wait.

## 7. Why a local mutex is insufficient

A process mutex disappears when its process fails and cannot order requests at
another data shard. Putting the mutex state in an ordinary cache has the same
problem. Lock ownership and transaction state must be transitions in the
replicated shard log, adjacent to the data the shard owns.

That leads to Stage 1: first make the lock table deterministic and testable with
no threads or network. Distribution comes only after the local rules are exact.

