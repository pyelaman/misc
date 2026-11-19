# Production-shaped Spanner transaction engine

This is a separate, staged implementation of the transaction path described in
the published Spanner papers and documentation. It leaves the earlier
`spanner_txn_lock_manager` project unchanged.

The module implements the publicly described **protocol shape**:

- ordered key-range splits;
- a persistent replicated state-machine boundary per split;
- leader-local active locks and durable prepared-lock reconstruction;
- timestamped MVCC versions and mutation intents;
- participant prepare timestamps and replica safe time;
- a coordinator decision stored in one participant's replicated log;
- multi-split two-phase commit and outcome replay;
- a bounded-uncertainty interval clock and commit wait.

It does **not** claim to reproduce Google's proprietary Paxos, distributed
filesystem, GPS/atomic-clock fleet, networking, or fleet-management code. The
included consensus group and time sources are executable local harnesses with
production substitution points. That distinction is part of the design, not a
footnote.

## Relationship to the first module

| Concern | `spanner_txn_lock_manager` | This module |
|---|---|---|
| Primary goal | Explain lock/2PC algorithms | Model published persistence and recovery boundaries |
| Sharding | Cell hash | Ordered key-range directory |
| Active locks | Every lock command enters replicated state | Leader-local and volatile |
| Prepared locks | Protected in memory | Lock footprint persisted in prepare record and restored after election |
| User data | Not represented | Immutable MVCC versions and tombstones |
| Replication | In-memory ordered apply | Checksummed per-replica WAL, committed-index records, quorum and restart replay |
| Coordinator decision | Separate in-memory store | Stored in coordinator participant's replicated log |
| Coordinator restart | Requires original process state | Recovery token locates durable decision and participants |
| Clock | One uncertainty wrapper | Multiple source intervals, quorum validation, fail-closed commit wait |
| Read safety | Not represented | Safe time capped below unresolved prepare timestamps |

## Architecture

```text
                       API / transaction coordinator
                         txn id + stable priority
                                  |
                         ordered RangeDirectory
                                  |
              +-------------------+-------------------+
              |                                       |
              v                                       v
       SplitGroup 101                           SplitGroup 202
   [-inf, Accounts/m)                       [Accounts/m, +inf)
              |                                       |
     leader transaction actor                leader transaction actor
     + volatile active locks                 + volatile active locks
              |                                       |
       prepare boundary                         prepare boundary
              |                                       |
              v                                       v
   +-----------------------+               +-----------------------+
   | replicated log/WAL x3 |               | replicated log/WAL x3 |
   +-----------------------+               +-----------------------+
   | prepared lock records |               | prepared lock records |
   | mutation intents      |               | mutation intents      |
   | MVCC versions         |               | MVCC versions         |
   | safe-time watermark   |               | safe-time watermark   |
   +-----------------------+               +-----------------------+
              |                                       |
              +--------- replicated 2PC decision -----+
                                  |
                    TrueTime interval + commit wait
                                  |
                          externally ordered reply
```

## Implementation phases

### Phase 0 — Invariants and durable boundaries

The design starts with invariants rather than classes:

1. No cell has incompatible lock holders.
2. Active locks may disappear with a leader; prepared locks may not.
3. A prepare record is committed only while every declared lock is held.
4. A new leader restores all prepared lock footprints before serving traffic.
5. One transaction has at most one replicated coordinator decision.
6. Every participant applies a committed transaction at the same timestamp.
7. No snapshot reads beyond replica safe time.
8. No commit success is returned until the commit timestamp is definitely in
   the past.

See [External-consistency invariants](docs/external-consistency.md).

### Phase 1 — MVCC storage

`MvccStore` maintains:

```text
(table, row, column) -> {commit timestamp -> value or tombstone}
```

`Read(cell, t)` returns the newest version with timestamp `<= t`. Applying a
different value at an already used cell/timestamp is rejected. Mutation batches
are validated completely before any version is installed, preventing partial
apply if a later mutation conflicts. The persistent state machine rebuilds every
version from committed WAL commands.

Implemented in:

- `include/spanner_prod/mvcc_store.h`
- `src/mvcc_store.cc`

### Phase 2 — Transaction scheduler and prepare recovery

`LockManager` implements Spanner-style `ReaderShared`, `WriterShared`,
`Exclusive`, and `WriterSharedTimestamp` modes with wound-wait priority.

The important lifecycle is:

```text
ACTIVE
  locks exist only at the current leader
  leader loss aborts the transaction
       |
       | replicated PrepareCommand
       v
PREPARED
  lock footprint and mutations are durable
  locks cannot be wounded or locally expired
  leader election reconstructs them before new traffic
       |
       +---- replicated CommitCommand --> MVCC versions
       |
       +---- replicated AbortCommand  --> discard intents
```

Implemented in:

- `include/spanner_prod/lock_manager.h`
- `include/spanner_prod/split_group.h`
- `src/lock_manager.cc`
- `src/split_group.cc`

### Phase 3 — Persistent split replication boundary

Each local replica has a checksummed WAL containing:

- encoded log entries with term, index, operation ID and command;
- explicit durable committed-index records;
- truncation of incomplete trailing records;
- deterministic state-machine replay after restart.

`PersistentConsensusGroup` requires an odd quorum, provides leader terms,
follower catch-up, fail-stop control, fingerprint comparison, and an injected
failure after quorum append. A quorum-appended entry is recoverable even when
the original caller receives `kInDoubt`.

Application-level rejection is a committed deterministic no-op; it advances the
replicated log without being confused with WAL/storage failure.

Implemented in:

- `include/spanner_prod/persistent_replica.h`
- `src/persistent_replica.cc`
- `include/spanner_prod/codec.h`
- `src/codec.cc`

This class is **not networked consensus**. Production must implement the same
boundary using verified Paxos/Raft with durable hard state, election, fencing,
membership changes and snapshots.

### Phase 4 — Range routing and replicated 2PC

`RangeDirectory` maps canonical `table + row` routing keys to ordered split
ranges. It rejects overlaps rather than silently routing a key ambiguously.

For a multi-split commit:

```text
1. Every participant verifies all active locks are granted.
2. Every participant quorum-persists locks, intents and prepare timestamp.
3. Coordinator chooses:

   commit_ts >= max(all prepare timestamps,
                    coordinator's prior timestamp + 1,
                    TT.now().latest)

4. Coordinator participant quorum-persists one immutable decision.
5. Coordinator performs commit wait.
6. Every participant quorum-persists commit/abort.
7. Commit creates MVCC versions at the common timestamp and releases locks.
8. Safe-time watermarks advance and the client receives success.
```

`RecoveryToken(transaction_id, coordinator_split)` lets a replacement
coordinator recover without the original process memory. If a durable decision
exists, it is replayed. If prepares exist but no decision exists, recovery
records and replays abort.

Implemented in:

- `include/spanner_prod/range_directory.h`
- `include/spanner_prod/coordinator.h`
- `src/range_directory.cc`
- `src/coordinator.cc`

### Phase 5 — TrueTime contract and safe-time reads

`QuorumTrueTime` consumes independent source intervals:

```text
source.earliest <= actual time <= source.latest
```

It fails closed when too few healthy intervals overlap. Commit wait polls until:

```text
TT.now().earliest > commit_timestamp
```

The included `SystemTimeSource` is only a source adapter with a configured error
bound. Real deployment must use authenticated GPS/atomic/holdover masters,
hardware timestamping, measured delay, drift enforcement, liar rejection and
machine eviction.

Each split also tracks:

```text
safe_time = min(replicated safe watermark,
                every unresolved prepare timestamp - 1)
```

A snapshot read later than that value is rejected until the relevant decision
is applied.

Implemented in:

- `include/spanner_prod/true_time.h`
- `src/true_time.cc`
- `SplitStateMachine::safe_time()`

### Phase 6 — Verification and fault injection

The test executable covers:

- binary command-codec round trips and checksums;
- timestamped versions and deletion tombstones;
- prepared holders resisting wound-wait;
- quorum append, `kInDoubt`, recovery and follower catch-up;
- restart from three on-disk replica WALs;
- committed semantic errors not poisoning replay;
- leader election restoring prepared locks;
- clock-source quorum failure;
- two externally ordered multi-split transactions;
- cold coordinator recovery from a replicated commit decision.
- restart recovery of a prepared transaction with no decision to durable abort;
- concurrent clients through the coordinator and split synchronization;
- all-or-nothing MVCC batches when one mutation conflicts.

## Build and run

Requirements: Linux/POSIX storage APIs, CMake 3.20+, and a C++20 compiler.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/spanner_prod_demo
```

Sanitizer verification used during development:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-asan --parallel
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=thread -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=thread'
cmake --build build-tsan --parallel
setarch "$(uname -m)" -R ./build-tsan/spanner_prod_tests
```

The `setarch` form avoids the known ThreadSanitizer `unexpected memory mapping`
failure caused by this host's randomized address layout; it does not suppress
ThreadSanitizer findings.

The demo writes two account cells owned by different ranges. Its WALs are
created beneath `/tmp/spanner-production-demo` and removed after success.

## Source map

| Component | Responsibility |
|---|---|
| `types.*` | Transactions, locks, mutations, prepare/decision commands |
| `codec.*` | Versioned deterministic binary encoding and checksums |
| `mvcc_store.*` | Timestamped immutable values and tombstones |
| `lock_manager.*` | Leader-local wound-wait and prepared protection |
| `split_state_machine.*` | Deterministic prepare/commit/abort/data/safe-time state |
| `persistent_replica.*` | WAL, quorum harness, recovery and leader epochs |
| `split_group.*` | Joins volatile scheduling to durable replication |
| `range_directory.*` | Ordered split ownership |
| `true_time.*` | Bounded-time quorum and commit wait |
| `coordinator.*` | Participants, replicated 2PC decision and recovery |

## What is still required for production

The following are intentionally explicit gaps:

1. Replace the in-process quorum harness with real Paxos/Raft networking,
   persistent terms/votes, leases, joint membership and fencing.
2. Add WAL snapshots, compaction, manifests, atomic installation and repair.
3. Replace the in-memory MVCC map with a durable LSM/B-tree and intent/index
   maintenance integrated with the replicated apply transaction.
4. Add interval/range and row-existence locks to prevent phantoms; current lock
   granularity is exact cells even though split routing is range based.
5. Replace the process-local range directory with replicated metadata, routing
   epochs, online split/merge and movement protocols.
6. Replace the serialized coordinator with asynchronous per-transaction actors,
   durable client epochs, cancellation and admission control.
7. Deploy real diverse time masters and prove conservative uncertainty bounds.
8. Add authenticated RPC, IAM, quotas, encryption, auditing and key redaction.
9. Add metrics for lock waits, prepares, decision replay, safe time, WAL/fsync,
   consensus lag and time uncertainty.
10. Model-check crash points and run Jepsen-style strict-serializability tests
    before making an external-consistency claim.

See [Production substitutions and rollout](docs/production-substitutions.md).

## Primary references

- [Spanner: Google's Globally-Distributed Database](https://research.google/pubs/spanner-googles-globally-distributed-database-2/)
- [Life of Spanner reads and writes](https://docs.cloud.google.com/spanner/docs/whitepapers/life-of-reads-and-writes)
- [TrueTime and external consistency](https://docs.cloud.google.com/spanner/docs/true-time-external-consistency)
- [Spanner transaction and wound-wait behavior](https://docs.cloud.google.com/spanner/docs/transactions)
- [Spanner lock modes](https://docs.cloud.google.com/spanner/docs/introspection/lock-statistics)
