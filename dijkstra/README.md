# Dijkstra-era concurrent algorithms in C++

This workspace implements the concurrent algorithms specified in Leslie
Lamport's *Dijkstra Book — Chapter on Concurrent Algorithms* (18 December
2021), supplied as `dijkstra.pdf`. The code is deliberately pedagogical:
atomic-action boundaries, fairness assumptions, and progress guarantees are
made explicit instead of being hidden behind production synchronization
primitives.

## Implemented inventory

| PDF section | Implementation | Documentation |
|---|---|---|
| Figure 1, one-bit algorithm | `OneBitMutex` | [One-bit mutex](docs/one_bit_mutex.md) |
| Figure 2, Dekker's algorithm | `DekkerMutex` | [Dekker mutex](docs/dekker_mutex.md) |
| Figure 3, Dijkstra's N-process algorithm | `DijkstraMutex` | [Dijkstra mutex](docs/dijkstra_mutex.md) |
| Section 2.4, P/V semaphores | `CountingSemaphore`, `BinarySemaphore`, `SemaphoreMutex` | [Semaphores](docs/semaphores.md) |
| Section 2.6, dining philosophers | `DiningPhilosophers` | [Dining philosophers](docs/dining_philosophers.md) |
| Figure 4, coarse self-stabilizing ring | `CoarseTokenRing` | [Coarse token ring](docs/coarse_token_ring.md) |
| Section 3.3, fine self-stabilizing ring | `FineTokenRing` | [Fine token ring](docs/fine_token_ring.md) |
| Figures 5 and Section 4.4, on-the-fly marking | `OnTheFlyGarbageCollector` | [On-the-fly GC](docs/on_the_fly_gc.md) |
| Section 5.2, tree termination detection | `TreeTerminationDetector` | [Tree termination](docs/tree_termination.md) |
| Figures 6–7, ring termination detection | `RingTerminationDetector` | [Ring termination](docs/ring_termination.md) |

The chapter mentions readers/writers, Knuth's mutex, the bakery algorithm,
and Dijkstra's other three- and four-state rings but does not specify those
algorithms. They are therefore not presented as implementations from this
source.

## Build and test

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
./build/dijkstra_demo
```

The library requires C++20 and has no third-party dependencies.

## Design rules

- Shared registers in the historical algorithms use sequentially consistent
  `std::atomic` operations, matching the source's sequentially consistent
  atomic-register model.
- Process-indexed mutexes expose `lock(process_id)` and
  `unlock(process_id)` because a caller's identity is part of each algorithm.
- Busy waits yield the current C++ thread. This is an execution convenience,
  not a change to the algorithms' progress guarantees.
- Algorithms described as distributed are implemented as thread-safe state
  machines. The methods represent their atomic actions and can be invoked by
  independent worker threads or by a deterministic test scheduler.
- The token-ring and garbage-collection implementations are executable
  models. The collector models a fixed graph and returns reclaimed node IDs;
  it is not a general-purpose C++ memory allocator.
- Runtime tests check safety and bounded executions. They do not constitute
  formal proofs of liveness for every possible schedule.

## Layout

```text
include/dijkstra_concurrency/   public APIs
src/                            implementations
docs/                           one design note per algorithm
examples/demo.cpp               small usage examples
tests/                          threaded and state-machine tests
```

## Practical warning

`OneBitMutex`, `DekkerMutex`, and `DijkstraMutex` are historical algorithms.
For production C++, use standard mutexes, semaphores, and established runtime
collectors. The implementations here are intended for study, testing, and
comparison with the chapter's pseudocode.
