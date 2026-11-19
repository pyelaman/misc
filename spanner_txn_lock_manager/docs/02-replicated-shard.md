# Stage 2 — Replicated shard state machine

Stage 1 is correct only while one process remains alive. Stage 2 turns each
transaction and lock mutation into a deterministic log command.

## 1. Commands, not callbacks

The shard accepts this closed command set:

```text
Begin(txn descriptor)
Acquire(txn descriptor, cell, mode, queue sequence)
Prepare(txn)
Commit(txn, timestamp)
Abort(txn, reason)
Tick(logical time)
```

Every command is wrapped in `(client id, command sequence)`. The state machine
remembers recent results, so retrying a command cannot acquire twice or commit
with a second timestamp.

## 2. Determinism boundary

All replicas must receive the same explicit inputs. Consequently:

- the lock table never reads wall-clock time;
- queue order does not depend on thread scheduling;
- timeout processing uses a replicated logical `TickCommand`;
- random victim selection is forbidden;
- transaction age is assigned before entering any shard.

`LockShardStateMachine::Apply()` requires the next exact log index. It returns a
result but never skips or reorders an entry.

## 3. Transaction lifecycle at a shard

```text
             Acquire
   ACTIVE --------------> ACTIVE
      |                     |
      | Prepare             | Abort / expiry / wound
      v                     v
   PREPARED               ABORTED
      |
      | Commit(decided timestamp)
      v
   COMMITTED
```

Only active transactions acquire locks. Prepare is rejected while any request
is waiting. Commit requires prepared state. Abort releases held and queued
locks. A logical deadline aborts only active transactions; expiring a prepared
participant independently could contradict a durable commit decision.

Prepare also marks the participant's holders as **protected**. Even an older
requester must wait for them. Wounding a prepared holder would be unsafe because
the coordinator might already have recorded commit while its commit message is
still in flight. Only replay of the recorded commit/abort decision may release
those locks.

## 4. The included replica group

`InMemoryReplicatedShard` demonstrates the integration contract:

- an odd replica count;
- quorum required before admission;
- one ordered log;
- apply to every live replica;
- fail-stop injection and follower catch-up;
- fingerprint comparison to detect nondeterminism.

It is **not consensus**: there is no leader election, term, persistent WAL,
network partition model, or quorum intersection proof. In production,
`IReplicatedShard::Propose()` should be implemented by a vetted Raft/Paxos
library. Committed log entries and snapshots must be persisted before success.

## 5. Deduplication window

The sample bounds remembered command results to avoid unbounded memory use. A
production protocol needs an explicit client-session epoch and acknowledged
sequence watermark so old dedup entries are discarded only when no legal retry
can refer to them. A time-based eviction alone is unsafe.
