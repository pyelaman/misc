# Production substitutions and rollout

## Consensus substitution

Replace `PersistentConsensusGroup` behind the split API with a vetted
Paxos/Raft implementation. The replacement must preserve:

- one total log order per split and leader epoch;
- durable quorum acknowledgement;
- deterministic state-machine inputs;
- committed-entry replay after crashes;
- no stale leader writes after election;
- safe membership changes;
- prepared records restored before serving new transactions.

The local harness intentionally serializes all replica operations. It does not
model message reordering, asymmetric partitions, elections or old-leader races.
Its recovery rule treats an identical entry found at the same index on a quorum
as chosen. A production consensus implementation must establish that fact from
durable protocol hard state, not infer safety from log-file voting alone.

## Storage substitution

Replace `MvccStore` with an LSM/B-tree whose write batch atomically updates:

- MVCC data versions;
- secondary indexes;
- prepared intents and transaction state;
- applied timestamp and safe-time metadata;
- log/snapshot manifest state.

The engine needs checksummed blocks, snapshots, compaction, garbage collection,
backup/PITR and corruption repair.

## Time substitution

Replace `SystemTimeSource` with local time-slave clients polling diverse
hardware-backed masters. The returned uncertainty interval must account for:

- master reference error;
- round-trip and asymmetric network delay;
- time since last synchronization;
- conservative oscillator drift;
- leap handling and monotonicity;
- master and local-clock health.

Uncertainty must grow during degraded synchronization. If it cannot be bounded,
externally consistent commits must pause or fail closed.

## Safe rollout

1. Verify the deterministic state machine with exhaustive replay tests.
2. Run the production consensus library against an in-memory state machine.
3. Run MVCC storage in shadow mode and compare versions/checksums.
4. Exercise every crash point around prepare, decision, commit wait and apply.
5. Compare the new scheduler with an existing serializable database oracle.
6. Canary single-split transactions before enabling multi-split 2PC.
7. Enable snapshot reads only after safe-time lag is observable and bounded.
8. Enable bounded-time timestamping in shadow mode before enforcing commit wait.
9. Use reversible feature flags and mixed-version protocol tests.
10. Require model checking and independent consistency-history validation before
    advertising external consistency.

## Operational stop conditions

Stop or degrade safely when any of these occur:

- no consensus quorum;
- uncertainty has no trusted quorum overlap;
- stale leader epoch or unknown split-routing epoch;
- corrupted WAL/snapshot checksum;
- prepared transaction whose coordinator cannot be located;
- safe time moving backward;
- two different decisions for one transaction;
- replica fingerprints diverging at the same committed index.
