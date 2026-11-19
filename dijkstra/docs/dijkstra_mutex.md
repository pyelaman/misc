# Dijkstra's N-process mutex

## Purpose

`DijkstraMutex` implements Figure 3, the N-process mutual-exclusion algorithm
that introduced Dijkstra's published formulation of the problem.

## Shared state

- `flag[i]` is the one-bit-protocol flag.
- `idle[i]` records whether process `i` is in its noncritical section.
- `turn` names the process currently allowed to complete the one-bit scan.

On entry, a process clears its idle state. If it does not own `turn`, it
withdraws its flag and can claim `turn` only when the current owner is idle.
The process owning `turn` lowers its flag and scans every other flag before
entering. On exit it raises its flag and marks itself idle.

## Guarantees and limitations

- Mutual exclusion and deadlock freedom under sequential consistency and
  process fairness.
- Not starvation-free: one process can repeatedly reclaim `turn`.
- The initial turn is configurable and defaults to process 0.
- Each participant requires a unique stable process ID.

## Example

```cpp
dijkstra::concurrency::DijkstraMutex mutex(8);
mutex.lock(my_id);
// critical section
mutex.unlock(my_id);
```
