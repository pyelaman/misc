# Finer-grained self-stabilizing token ring

## Purpose

`FineTokenRing` implements Section 3.3, splitting each process action into:

1. An atomic await/predicate action.
2. A later atomic state-update action.

The per-process `ControlState` records which action comes next and may itself
start in either state, as required by self-stabilization.

## Important distinction

For `i > 0`, the update action reads `S[i-1]` when the update executes. It
does not retain the predecessor value read by the earlier await action. The
chapter distinguishes this implementation from the alternate cached-read
interpretation and reports that the cached version is not self-stabilizing
for the small case it checked.

## Parameters and progress

With processes numbered `0..N`, the finer-grained proof requires `K > N`.
The C++ constructor therefore requires `K > process_count-1`.

`legitimate_and_settled()` requires exactly one token and all processes at
their await control point. This avoids reporting a transient state while a
previously enabled update remains pending.

```cpp
dijkstra::concurrency::FineTokenRing ring(
    5, 5, initial_values, initial_control_states);
ring.run_until_legitimate_and_settled(200000);
```
