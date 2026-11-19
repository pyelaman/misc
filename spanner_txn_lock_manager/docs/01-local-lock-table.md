# Stage 1 — Deterministic local lock table

## 1. State owned by the actor

`LockTable` is intentionally single-threaded. Its caller—the shard state
machine—serializes commands. Internally it owns:

```text
Cell -> holders[transaction] + ordered waiters[]
transaction -> held cells
transaction -> waiting cells
aborted transaction ids
```

Reverse indexes make abort/release proportional to a transaction's footprint
instead of requiring a scan of the complete lock table.

## 2. Acquire algorithm

For transaction `T` requesting mode `M` on cell `C`:

```text
1. If T was already aborted, reject the request.
2. If T already holds a sufficient mode, return success (idempotent acquire).
3. If this is an upgrade, combine the old and requested modes.
4. Find incompatible holders, excluding T itself.
5. For every conflicting younger holder, wound it and remove all its locks.
6. If an older conflicting holder remains, enqueue T by (age, id, sequence).
7. Otherwise install T as holder.
8. Only now process queues affected by wounded transactions.
```

Step 8 is subtle. If aborting a victim immediately wakes waiters, a queued
younger transaction can steal `C` between steps 5 and 7. Deferring wakeups
until the requester is installed makes the whole operation atomic at the state
machine boundary.

## 3. Why wound-wait prevents deadlock

The rule is asymmetric:

```text
older requester vs younger holder -> wound (abort) holder
younger requester vs older holder -> wait
```

Wait edges therefore point from younger to older transactions. Ages are a
strict global order, so a wait-for cycle cannot form: following the cycle would
require age to decrease forever and eventually return to its starting value.

This trades deadlock detection for controlled aborts. Older work eventually
wins, which also limits starvation when retry priority is preserved.

## 4. Lock upgrades

A transaction that read and then writes the same cell needs:

```text
ReaderShared + WriterShared -> Exclusive
```

Its own current lock is excluded from conflict checking. Other readers still
conflict with the upgrade. The upgrader wounds younger readers or waits for
older readers according to the same age rule—there is no separate upgrade
protocol to deadlock.

## 5. Queue policy

Waiters are ordered by transaction age, transaction id, then enqueue sequence.
After a holder releases:

- compatible readers can be granted as a group;
- compatible writer-shared requests can be granted as a group;
- the first incompatible waiter stops the scan.

This deliberately favors stable transaction priority over raw arrival order.
Production systems may add admission classes, tenant fairness, or contention
backoff without changing the incompatibility invariant.

## 6. API behavior

`Acquire()` returns one of:

- `kGranted`: the caller may proceed;
- `kWaiting`: ownership has not been obtained;
- `kAborted`: the transaction must stop/retry.

The reference API reports asynchronous grant events to its state-machine
caller. A production RPC service would map this to a cancellable future or a
server-side wait token rather than asking clients to poll the lock table.

