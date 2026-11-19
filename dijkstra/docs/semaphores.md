# P/V semaphores

## Purpose

Section 2.4 defines semaphore operations:

- `P`: wait until a permit is available, then consume one.
- `V`: publish a permit.

The workspace provides:

- `CountingSemaphore(initial, maximum)`
- `BinarySemaphore(initially_available)`
- `SemaphoreMutex`, a `BasicLockable` adapter usable with
  `std::lock_guard`

## Implementation

A mutex protects the permit count and a condition variable blocks waiters.
Wake-up order is unspecified, matching the chapter's base semaphore
definition. Consequently, the implementation does not promise FIFO service.

The chapter introduces a binary semaphore with value zero and uses `V` to
make it available. `SemaphoreMutex` starts available (one permit), which is
the initialized state needed by the `P; critical section; V` mutex pattern.

## Guarantees and limitations

- Permit count never becomes negative or exceeds the configured maximum.
- Deadlock freedom depends on every successful acquire being paired with a
  release.
- Starvation freedom is not part of the API contract because waiter ordering
  is unspecified.

```cpp
dijkstra::concurrency::CountingSemaphore slots(3, 3);
slots.acquire();
// one of at most three concurrent activities
slots.release();
```
