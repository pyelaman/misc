# Stage 4 — Production hardening plan

The code establishes useful protocol boundaries, but “near-production” must not
be confused with “safe to deploy.” The following work is required in roughly
this order.

## 1. Couple locks to the data engine

Today the project commits lock state, not user mutations. A storage engine must
atomically persist:

- prepared write intents or mutations;
- transaction status;
- lock ownership needed for recovery;
- MVCC versions at the chosen commit timestamp.

A participant may vote prepared only after its write intent is durable. Commit
must make the version visible and release locks in one replicated transition.

## 2. Replace the consensus harness

Implement `IReplicatedShard` using production Raft/Paxos with:

- durable WAL and checksums;
- terms/ballots and leader fencing;
- quorum reads or lease-safe leadership;
- snapshots and log compaction;
- membership changes with joint consensus;
- corruption detection and repair;
- backpressure when followers or disks lag.

The state machine must remain deterministic. Never allow a follower to obtain
time, randomness, or external state not present in the log entry.

## 3. Durable coordinator and termination protocol

Replace `InMemoryDecisionStore` with a replicated record containing transaction
id, coordinator epoch, participants, decision, and commit timestamp. Add:

- coordinator failover and fencing;
- participant inquiry for prepared transactions;
- presumed-abort rules only where formally safe;
- indefinite retry of a durable commit decision;
- garbage collection after every participant acknowledges;
- tombstones/epochs that prevent transaction-id reuse.

## 4. Range and existence locking

Exact cells do not prevent phantoms. Add a canonical byte-ordered key encoding
and an interval/range-lock index supporting point/range overlap. Row existence
and interleaved hierarchy locks must be represented explicitly. Test prefix,
open/closed endpoint, empty-range, and split/merge cases.

## 5. Asynchronous waiter service

Replace result polling with a wait token or future:

- cancellation removes the queued request through the replicated log;
- disconnect does not silently release a prepared transaction;
- notification is edge-trigger-safe and idempotent;
- waiters re-register after leader failover using transaction ID and sequence;
- queue and transaction limits prevent memory exhaustion.

## 6. Time correctness

The clock provider needs health state, uncertainty bounds, monotonicity checks,
and operational alerts. Reject or pause externally consistent commits when the
bound cannot be trusted. Persist the last assigned timestamp per leader epoch,
and account for timestamps already promised before failover.

## 7. Security and multi-tenancy

Add mutually authenticated RPC, authorization at database/table scope, request
size limits, per-tenant quotas, audit events, secret rotation, and denial-of-
service protection. Transaction IDs must be unforgeable capabilities or bound
to authenticated sessions.

## 8. Observability

At minimum expose:

- acquire latency and queue duration by mode/table/key-prefix;
- holder/waiter counts and oldest transaction age;
- wound/abort/retry rates and transaction tags;
- prepare-to-decision and decision-to-ack latency;
- prepared transaction count/age;
- dedup hits and evictions;
- consensus proposal, quorum, disk, and follower lag;
- commit-wait duration and clock uncertainty.

Avoid logging raw keys or values by default; use redacted fingerprints and
controlled sampling.

## 9. Verification strategy

Use layers, not only example tests:

1. exhaustive compatibility and upgrade tests;
2. property tests asserting no incompatible holders;
3. deterministic replay/fingerprint tests;
4. model checking of wound-wait and 2PC crash points;
5. Jepsen-style client histories for serializability/external consistency;
6. fault injection at every WAL, prepare, decision, delivery, and recovery
   boundary;
7. long-running contention, split/merge, clock-skew, and rolling-upgrade tests;
8. sanitizers, fuzzers, and invariant checks in debug/canary builds.

## 10. Safe rollout

Start with shadow computation: run the new lock scheduler beside the existing
one and compare decisions without enforcing them. Then use canary shards,
strict abort/latency/error budgets, and reversible feature flags. Protocol and
on-disk format upgrades require mixed-version compatibility tests before any
rolling deployment.

