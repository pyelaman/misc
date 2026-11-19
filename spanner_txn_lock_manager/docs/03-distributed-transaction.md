# Stage 3 — Distributed transaction and recovery

A transaction touching one shard can commit through that shard's replicated
log. Touching multiple shards requires one indivisible decision.

## 1. Coordinator state

The coordinator assigns:

- a unique transaction id;
- a stable global age;
- a deadline;
- a set of participant shards discovered during acquire;
- monotonically increasing command IDs.

Cells are deterministically mapped to shards. `EnsureBegun()` installs the same
transaction descriptor before the first lock command at each participant.

## 2. Two-phase commit

```text
Coordinator                 Shard A                 Shard B
     |                         |                       |
     | Acquire locks           |                       |
     |------------------------>|                       |
     | Acquire locks           |                       |
     |------------------------------------------------>|
     |                         |                       |
     | PREPARE                 |                       |
     |------------------------>| retain locks, vote OK |
     | PREPARE                 |                       |
     |------------------------------------------------>| retain locks, vote OK
     |                         |                       |
     | persist COMMIT(txn, timestamp, [A,B])           |
     |                         |                       |
     | COMMIT(timestamp)       |                       |
     |------------------------>| release locks         |
     | COMMIT(timestamp)       |                       |
     |------------------------------------------------>| release locks
     |                         |                       |
     | commit wait             |                       |
     | return success          |                       |
```

If any prepare fails, the coordinator records abort and sends abort to every
known participant. A participant that voted prepared cannot decide locally; it
must learn the coordinator's immutable decision.

## 3. Why the decision record precedes commit messages

Suppose A commits, the coordinator crashes, and B has not received a message.
Without a durable decision, recovery cannot tell whether to commit B or undo A.
Recording the decision first converts recovery into replay:

```text
decision exists -> resend it until every participant acknowledges
no decision     -> coordinator is still free to abort
```

`IDecisionStore::PutIfAbsent()` also prevents a transaction from receiving two
different outcomes.

The supplied `InMemoryDecisionStore` exercises this protocol but is not durable.
A production implementation should place the coordinator record in a
replicated write-ahead log or choose one participant group as coordinator.

## 4. “In doubt” is a precise state

Once commit is recorded, a delivery timeout cannot be reported as an abort.
The client receives `kInDoubt` with the commit timestamp. `Recover()` reads the
record and replays it. Retrying with a new transaction before resolving the
outcome can duplicate non-idempotent application effects.

## 5. Commit timestamp and commit wait

`ICommitClock::Now()` returns `[earliest, latest]`, an uncertainty interval. The
coordinator selects a timestamp no earlier than `latest` and waits until
`earliest > commit_timestamp` before returning success.

This mirrors the role of TrueTime in external consistency: after success is
visible to the caller, real time is definitely later than the serialization
timestamp. `SystemUncertaintyClock` demonstrates the contract; production must
use a monitored bounded-uncertainty source and fail closed when its bound is
invalid.

## 6. Wounds across shards

A shard can discover that an older requester wounds a younger holder. The
victim must then abort on **all** its participants, not only on the conflicting
shard. The reference coordinator propagates the abort when it owns the victim.
A distributed deployment needs a transaction-home address/epoch in the
descriptor or a replicated transaction-status service so any shard can route
that wound safely.

