# CUDA Asynchronous Barriers: A100 and H100

This directory contains two CUDA C++ programs that demonstrate the difference
between:

1. an **asynchronous split barrier**, hardware-accelerated on Ampere A100 and
   newer GPUs; and
2. a Hopper **asynchronous transaction barrier**, which completes only after
   both thread arrivals and tracked asynchronous memory transactions complete.

These are correctness-first teaching examples with small timing comparisons.
They are not intended to replace application-specific Nsight Compute analysis.

## Hardware model

| Capability | A100 (Ampere, SM80) | H100 (Hopper, SM90) |
|---|---|---|
| `cuda::barrier` split `arrive()` / `wait()` | Yes | Yes |
| Shared-memory hardware barrier | Yes | Yes |
| Work between arrival and wait | Yes | Yes |
| Waiting behavior | Earlier implementation may poll the shared barrier | Hopper can suspend waiting threads |
| Transaction count in the barrier | No | Yes |
| TMA bulk global-to-shared copy | No | Yes |
| Barrier completion condition | Arrival count reaches zero | Arrival count **and** outstanding transaction-byte count reach zero |

The transaction count is normally a **byte count**, not the number of copy
instructions. If one TMA operation transfers 4096 bytes, the barrier must be
told to expect 4096 bytes. If several producers add transaction counts, their
updates are summed.

## Programs

### `asynchronous_barrier`

File: `asynchronous_barrier/main.cu`

Each thread:

1. produces one value in shared memory;
2. calls non-blocking `barrier.arrive()`;
3. performs deterministic work that does not depend on the other threads;
4. calls `barrier.wait(token)`; and
5. consumes all shared values.

Threads are deliberately given different amounts of producer work. An early
thread can perform its independent work while later threads are still
producing:

```text
early thread:  produce -> arrive -> independent work ---------> wait -> consume
late thread:   produce for longer -----------> arrive -> work -> wait -> consume
                                          barrier phase complete ^
```

The program also runs an equivalent `__syncthreads()` implementation. In that
version early threads block before starting the independent work.

The sample requires compute capability 8.0 or newer and can run on either A100
or H100.

### `asynchronous_transaction_barrier`

File: `asynchronous_transaction_barrier/main.cu`

This sample requires Hopper compute capability 9.0.

For each block:

1. one designated thread issues a 4096-byte TMA copy from global memory to shared
   memory with the C++ `cuda::ptx::cp_async_bulk` wrapper;
2. every thread contributes one arrival;
3. the designated thread also adds 4096 expected transaction bytes using
   `cuda::device::barrier_arrive_tx`;
4. all threads perform independent work;
5. `wait()` completes only after all threads have arrived **and** TMA has
   delivered all 4096 bytes; and
6. threads safely consume the shared tile.

```text
arrival countdown:       256 -> 0
transaction countdown:  4096 -> 0 as TMA delivers bytes

barrier phase completes only when both are zero
```

This is the key Hopper distinction. A plain asynchronous barrier says, “all
participating threads reached this phase.” A transaction barrier can also say,
“the asynchronous data movement associated with this phase has completed.”

The program compares that implementation with a conventional cooperative
global-to-shared copy followed by `__syncthreads()`.

## Prerequisites

- NVIDIA CUDA Toolkit 12.5 or newer
- CMake 3.24 or newer
- C++17-capable host compiler
- NVIDIA driver compatible with the installed toolkit
- A100 or newer for `asynchronous_barrier`
- H100 or another compute-capability 9.x device for
  `asynchronous_transaction_barrier`

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

### Local compiler installation

On the host where these samples were created, the minimal CUDA compiler is
installed at:

```text
/data/hdd/pryelama/cuda-12.5-min
```

Use it without changing the system CUDA installation:

```bash
conda activate /data/hdd/pryelama/cuda-12.5-min

cmake -S . -B build \
  -DCMAKE_CUDA_COMPILER="$CONDA_PREFIX/bin/nvcc" \
  -DCMAKE_CUDA_HOST_COMPILER="$CONDA_PREFIX/bin/x86_64-conda-linux-gnu-c++" \
  -DCUDAToolkit_ROOT="$CONDA_PREFIX" \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build -j
```

Both SM80 and SM90 targets were compiled successfully with CUDA 12.5.82.
Kernel execution was not validated on this host because it does not expose a
compatible NVIDIA driver or GPU.

The CMake targets are intentionally different:

- `asynchronous_barrier` contains SM80 and SM90 device code.
- `asynchronous_transaction_barrier` contains only SM90 device code.

Direct compilation is also possible:

```bash
nvcc -std=c++17 -O3 -arch=sm_80 -I. \
  asynchronous_barrier/main.cu -o asynchronous_barrier_demo

nvcc -std=c++17 -O3 -arch=sm_90 -I. \
  asynchronous_transaction_barrier/main.cu \
  -o asynchronous_transaction_barrier_demo
```

## Run

On A100 or H100:

```bash
./build/asynchronous_barrier
```

On H100:

```bash
./build/asynchronous_transaction_barrier
```

Both programs accept:

```text
--blocks N        number of thread blocks
--work N          independent-work iterations per thread
--repetitions N   kernel launches used for timing
```

Example:

```bash
./build/asynchronous_barrier \
  --blocks 128 --work 1024 --repetitions 200

./build/asynchronous_transaction_barrier \
  --blocks 128 --work 1024 --repetitions 200
```

Successful runs print `Validation: PASS` before their timing results.

## Correctness requirements

### Split barriers

- Initialize a shared barrier from one thread before any arrival.
- Use a block synchronization operation to make initialization visible.
- Every participating thread must arrive exactly once per phase.
- A returned arrival token belongs to one phase and is consumed by one wait.
- Work between `arrive()` and `wait()` must not depend on data that becomes
  valid only after the barrier completes.
- Writes before arrival become visible to participants after a successful wait.

### Transaction barriers

All split-barrier rules still apply, plus:

- The barrier and shared destination must meet TMA alignment requirements.
- Global and shared addresses must be at least 16-byte aligned for the 1D bulk
  transfer used here.
- Transfer size must be a multiple of 16 bytes.
- Expected transaction bytes must exactly match the operation being tracked.
- Only the designated producer should issue the TMA transfer.
- Shared data must not be consumed before the transaction-aware wait completes.

An incorrect transaction count can deadlock the barrier or permit a phase to be
accounted incorrectly. Treat the byte count as part of the synchronization
protocol, not as optional metadata.

## Performance interpretation

The advantage comes from useful overlap:

```text
producer work | arrive | independent work | wait | dependent work
                         <--- overlap --->
```

If independent work lasts long enough for all other arrivals—and, on Hopper,
for the asynchronous transaction—to complete, `wait()` can retire quickly.

The included timings are deliberately simple. Results depend on occupancy,
warp scheduling, memory traffic, the amount of independent work, and compiler
optimization. For real analysis, inspect barrier stalls, TMA activity, and
eligible warps with Nsight Compute.

## Why transaction barriers matter for clusters

Hopper Thread Block Clusters allow a block to access distributed shared memory
belonging to another block in the same cluster. Such exchanges need more than a
thread-arrival condition: consumers must know that the associated data
transaction has completed. Transaction-counted barriers provide that combined
arrival-and-data-completion primitive and are therefore a foundation for
cluster-level data exchange.

## References

- [CUDA Programming Guide: Asynchronous Barriers](https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/async-barriers.html)
- [CUDA Programming Guide: Asynchronous Data Copies](https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/async-copies.html)
- [NVIDIA Hopper Tuning Guide](https://docs.nvidia.com/cuda/hopper-tuning-guide/)
- [libcu++ `cuda::barrier` reference](https://nvidia.github.io/cccl/libcudacxx/extended_api/synchronization_primitives/barrier.html)
