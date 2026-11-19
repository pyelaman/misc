# On-the-fly garbage collection

## Purpose

`OnTheFlyGarbageCollector` is an executable fixed-graph model of Sections
4.1–4.4. A mutator redirects edges while a collector performs repeated
marking and reclaiming cycles without a global stop-the-world lock.

It follows the corrected fine-grained protocol from the published algorithm,
also described in the [EWD 520 transcription](https://www.cs.utexas.edu/~EWD/transcriptions/EWD05xx/EWD520.html).

## Representation

Each node has:

- A fixed number of atomic outgoing edges.
- `white` and `black` bits. A node is gray when both bits are false.
- A reclaimed bit used by this model instead of an actual free list.

The graph and roots are fixed. A redirect must have reachable, allocated
source and target nodes, matching the source algorithm's precondition.

## Mutator barrier

`redirect(A, e, B)` consists of two separate sequentially consistent actions:

1. Store `B` into edge `e` of `A`.
2. Shade `B`, changing white to at least gray.

This is the corrected order. Shading the future target before installing the
edge can span collection cycles and allow a later black-to-white edge.

The proof in the source assumes one mutator; use only one thread to issue
redirect operations. A collector may run concurrently with it.

## Collector

1. Shade every root.
2. Scan nodes until a complete pass finds no gray node.
3. For each gray node, read and shade every successor, then make the node
   black and restart the complete-pass count.
4. Reclaim remaining white nodes.
5. Reset black nodes to white for the next collection.

No node storage is deleted. `collect()` returns reclaimed IDs so tests can
observe the algorithm without risking C++ use-after-free.

## Limitations

- Fixed node count and fixed out-degree.
- Exactly one collector and one mutator.
- No allocator/free-list producer-consumer protocol.
- Not a production tracing collector.
