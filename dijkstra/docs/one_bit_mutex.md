# One-bit mutual exclusion

## Purpose

`OneBitMutex` implements Figure 1 for `N` process identities. Each process has
a Boolean flag whose `true` value means that the process is not competing for
the critical section.

## Algorithm mapping

`lock(i)` performs the source algorithm's label-`L` retry:

1. Set `flag[i]` to `false`.
2. Inspect lower-numbered processes. If one is competing, withdraw by setting
   `flag[i]` to `true`, wait for that lower process, and retry.
3. Wait for every higher-numbered process to have a `true` flag.
4. Enter the critical section.

`unlock(i)` restores `flag[i]` to `true`.

## Guarantees and limitations

- Mutual exclusion: yes, under sequentially consistent atomic registers.
- Deadlock freedom: yes, assuming processes that are scheduled continue.
- Starvation freedom: no. Lower process numbers have fixed priority.
- Blocking style: spin/yield; no kernel-assisted sleeping.

## API

```cpp
dijkstra::concurrency::OneBitMutex mutex(process_count);
mutex.lock(process_id);
// critical section
mutex.unlock(process_id);
```

Every concurrently executing participant must use a unique stable ID in
`[0, process_count)`.
