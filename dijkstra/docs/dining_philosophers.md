# Dining philosophers

## Purpose

`DiningPhilosophers` implements the state-and-semaphore solution described in
Section 2.6. A global semaphore protects philosopher state, while a private
binary semaphore blocks each hungry philosopher until both neighboring forks
are available.

## State transition

`take_forks(i)`:

1. Under the global semaphore, change `thinking` to `hungry`.
2. If neither neighbor is eating, change to `eating` and signal process `i`.
3. Wait on process `i`'s private semaphore.

`put_forks(i)`:

1. Under the global semaphore, change `eating` to `thinking`.
2. Test and potentially wake the left neighbor.
3. Test and potentially wake the right neighbor.

No explicit fork objects are needed; neighboring `eating` states encode fork
ownership.

## Guarantees and limitations

- Adjacent philosophers never eat simultaneously.
- The circular wait in which every philosopher holds one fork is avoided.
- The algorithm as described is not starvation-free; repeatedly favored
  neighbors can postpone a hungry philosopher.

```cpp
dijkstra::concurrency::DiningPhilosophers table(5);
table.take_forks(i);
// eat
table.put_forks(i);
```
