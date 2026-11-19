# Spanner-style transactional lock manager

This project is a staged, executable reference design for the **internal
transaction lock manager** used by a Spanner-like distributed database. It is
not Chubby, not a user-facing distributed mutex API, and not Google Spanner
source code.

The implementation is deliberately split at the same boundaries a production
system needs:

1. a deterministic cell lock table;
2. a replicated shard state machine with idempotent commands;
3. a multi-shard transaction coordinator with prepare, durable-decision-shaped
   two-phase commit, recovery, and commit wait;
4. explicit interfaces where real consensus, durable storage, networking, and
   a TrueTime-like clock must be inserted.

The result is near-production in **structure and invariants**, but an
educational reference in **infrastructure**. The supplied replica group and
decision store are in memory, and the lock index covers exact cells rather
than arbitrary key ranges.

## The system in one picture

```text
 application transaction
          |
          v
 +------------------------+
 | TransactionCoordinator |  stable txn id/age, participants, 2PC decision
 +------------------------+
      |              |
      | commands     | commands
      v              v
 +-----------+    +-----------+        each shard is an independent
 | shard 101 |    | shard 202 |        replicated state machine
 +-----------+    +-----------+
 | log 1..N  |    | log 1..M  |
 | txn table |    | txn table |
 | lock table|    | lock table|
 +-----------+    +-----------+
      |              |
      +------ prepare+------------------ locks remain held
                     |
             immutable decision record
                     |
      +------ commit/abort replay ------- locks are released
                     |
                 commit wait
                     |
               client response
```

The coordinator never directly edits a lock table. Every mutation is an
ordered `CommandEnvelope`, so a production Raft/Paxos layer can replace the
in-memory harness without changing transaction semantics.

## Staged reading path

Read the documents in order; each stage explains why the next layer is needed.

| Stage | Document | Question answered |
|---|---|---|
| 0 | [First principles](docs/00-first-principles.md) | What is being locked, and what must always be true? |
| 1 | [Local lock table](docs/01-local-lock-table.md) | How do modes, upgrades, queues, and wound-wait work? |
| 2 | [Replicated shard](docs/02-replicated-shard.md) | How does the lock state survive leader changes without diverging? |
| 3 | [Distributed transaction](docs/03-distributed-transaction.md) | How do multiple shards reach one atomic outcome? |
| 4 | [Production hardening](docs/04-production-hardening.md) | What remains before this could carry real data? |

## Source map

| Component | Files | Responsibility |
|---|---|---|
| Shared types | `types.h`, `types.cc` | cells, lock modes, transaction age, status, results |
| Lock core | `lock_table.h`, `lock_table.cc` | compatibility, upgrades, wound-wait, queues, release |
| Shard state machine | `lock_shard.h`, `lock_shard.cc` | transaction lifecycle, command deduplication, expiry |
| Replication boundary | `replication.h`, `replication.cc` | quorum admission, ordered apply, catch-up, fault injection |
| Coordinator | `coordinator.h`, `coordinator.cc` | sharding, participants, 2PC, decisions, recovery, commit wait |
| Verification | `tests/test_main.cc` | deterministic safety and failure-path tests |
| Example | `examples/demo.cc` | a two-shard account-transfer lock/commit flow |

## Build and run

Requirements: CMake 3.20+ and a C++20 compiler.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/spanner_lock_demo
```

The build enables `-Wall -Wextra -Wpedantic -Werror` for all project targets.

## Implemented guarantees

- Exact-cell locks use Spanner-style `ReaderShared`, `WriterShared`,
  `Exclusive`, and `WriterSharedTimestamp` modes.
- A stable transaction age drives wound-wait: an older conflicting requester
  aborts a younger holder; a younger requester waits for an older holder.
- Wound cleanup cannot wake a queued transaction ahead of the requester that
  caused the wound.
- Lock upgrades combine `ReaderShared + WriterShared` into `Exclusive`.
- Every shard transition is deterministic and applied at a strict log index.
- Command IDs make retries idempotent within a bounded deduplication window.
- Active transactions can expire; prepared transactions are protected from a
  local timeout or a later wound because their outcome must come from the
  coordinator decision.
- Cross-shard commit uses prepare before recording an immutable decision.
- A failed commit delivery returns `kInDoubt`; recovery replays the recorded
  decision rather than choosing a new outcome.
- Commit timestamps are chosen at or above the clock's uncertainty upper bound,
  followed by commit wait before success is returned.

## Deliberate non-goals of this repository

This implementation does not include a data/MVCC engine, network RPC, TLS/IAM,
real persistent consensus, replica reconfiguration, range locking, durable
snapshots, or distributed coordinator failover. Those omissions are not hidden
behind misleading class names; each has an interface or a documented insertion
point in [the production hardening guide](docs/04-production-hardening.md).

## Primary references

- [Spanner: Google's Globally-Distributed Database](https://research.google/pubs/spanner-googles-globally-distributed-database-2/)
- [Cloud Spanner transactions](https://docs.cloud.google.com/spanner/docs/transactions)
- [Cloud Spanner lock modes and conflict statistics](https://docs.cloud.google.com/spanner/docs/introspection/lock-statistics)
- [The Chubby lock service](https://research.google/pubs/the-chubby-lock-service-for-loosely-coupled-distributed-systems/) — useful mainly for understanding what this project is *not*
