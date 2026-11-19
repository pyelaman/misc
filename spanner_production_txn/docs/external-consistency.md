# External-consistency invariants

## Safety target

For transactions `T1` and `T2`:

```text
client observes T1 complete before T2 starts committing
                       implies
commit_timestamp(T1) < commit_timestamp(T2)
```

This is stricter than serializability because the legal serial order must agree
with externally observed real-time order.

## Timestamp-selection rule

After the commit request reaches the coordinator, choose:

```text
s >= TT.now().latest
s >= every participant prepare timestamp
s > coordinator's previously assigned timestamp
```

The prepare lower bounds are required for safe-time reasoning at participant
replicas. The TrueTime upper bound orders even causally related transactions
that touch disjoint splits.

## Commit-wait rule

Do not expose committed data or success to the client until:

```text
TT.now().earliest > s
```

Therefore `s` is certainly in the past when the client learns success.

## Ordering proof

If `T1` completes before `T2` starts committing:

```text
s1 < absolute_commit_time(T1)       commit wait
   < absolute_start_time(T2)        observed real-time order
  <= coordinator_arrival(T2)        causality
  <= s2                             timestamp-selection rule
```

Thus `s1 < s2`.

## Atomic visibility

Timestamp order is insufficient if one participant exposes data early. This
module therefore enforces:

1. prepare records and mutation intents reach a split quorum;
2. one coordinator decision reaches a quorum;
3. commit wait finishes;
4. all participants create versions at the same timestamp;
5. reads wait for their target timestamp to be safe and applied.

Prepared locks remain protected until step 4. A replacement leader reconstructs
them before accepting conflicts.

## Safe time

A participant with an unresolved prepare at lower bound `p` cannot promise that
no write will later appear at `p` or above. Its transaction-manager safe time is
therefore at most `p - 1`.

```text
replica_safe_time = min(consensus_safe_time,
                        minimum_unresolved_prepare_timestamp - 1)
```

Only reads with `read_timestamp <= replica_safe_time` are served.

## Failure interpretation

Before a coordinator decision is quorum durable, abort remains legal. After a
commit decision is quorum durable, abort is no longer legal—even if every RPC
times out. That state is reported as `kInDoubt`, and recovery can only replay the
recorded decision.

