# Coarse-grained self-stabilizing token ring

## Purpose

`CoarseTokenRing` implements Figure 4 as an executable state machine. It
starts from any vector `S` and repeatedly executes enabled token-passing
actions until exactly one process has the token.

## Token predicate

For processes numbered `0..P-1`:

- Process 0 has the token when `S[0] == S[P-1]`.
- Process `i > 0` has the token when `S[i] != S[i-1]`.

An enabled nonzero process copies its predecessor's value. Process 0
increments its value modulo `K`. The predicate test and update execute under
one action mutex, representing the figure's single coarse atomic action.

## Parameters and progress

The PDF uses processes `0..N`; therefore the implementation checks its
condition `K >= N` as `K >= P-1`. Scheduler fairness is required for eventual
stabilization.

`run_until_legitimate(max_actions, seed)` supplies a randomized fair-enough
test scheduler. Production callers may instead invoke `try_step(i)` from
independent workers.

## Example

```cpp
dijkstra::concurrency::CoarseTokenRing ring(
    5, 5, {3, 1, 4, 1, 2});
auto actions = ring.run_until_legitimate(100000);
```
