# Dekker's two-process mutex

## Purpose

`DekkerMutex` implements Figure 2. It augments the one-bit protocol with a
dynamic `turn` register so two processes can withdraw and retry without
deadlock or fixed-priority starvation.

## State and actions

- `flag[i] == false` means process `i` is competing.
- `turn` selects the process allowed to keep competing when both flags are
  false.
- A process that lacks the turn temporarily restores its flag, waits for its
  turn, then announces interest again.
- On exit, process `i` gives the turn to `1-i` and restores its flag.

All shared reads and writes are sequentially consistent atomics.

## Guarantees and limitations

- Exactly two process identities, `0` and `1`.
- Mutual exclusion and deadlock freedom.
- Starvation freedom under the source's process-fairness assumption.
- Busy waiting makes this an educational mutex rather than a preferred
  production primitive.

## Example

```cpp
dijkstra::concurrency::DekkerMutex mutex;
mutex.lock(0);
// process 0 critical section
mutex.unlock(0);
```
