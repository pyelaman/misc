# Graph Cycle Detection And Partitioning

This is a small standalone C++17 graph library based on the provided cycle
checking and partition initialization snippets.

## Components

- `util_graph::Graph`: adjacency-list directed graph with node and edge helpers.
- `util_graph::DFSAll`: DFS traversal helper used to assign reverse topological ranks.
- `util_graph::GraphCycles`: graph wrapper that rejects cyclic input and rejects
  incremental edges that would create a cycle.
- `util_graph::MergeGraph`: simple partitioner that starts with one cluster per
  graph node and supports cluster merges only when the quotient/cluster graph
  remains acyclic.
- `tests/graph_driver.cc`: self-contained test driver.

## Build And Run

```bash
cd /data/hdd/pryelama/graph_cycles_partition
make clean
make
make run
```

## Design Notes

`GraphCycles::InitFrom()` mirrors the supplied snippet:

1. Create all nodes from the source graph.
2. Run DFS over all nodes.
3. For an acyclic graph, DFS leave order is reverse topological order, so the
   implementation assigns decreasing ranks as nodes are left.
4. Add every source edge while checking `rank[src] < rank[dst]`.
5. If any edge violates that ordering, the source graph contains a cycle.

`GraphCycles::AddEdgeChecked()` is intentionally conservative and simple: before
adding `src -> dst`, it checks whether `dst` already reaches `src`. If so, the
new edge would close a cycle and is rejected. If the edge is accepted, ranks are
recomputed with Kahn topological sort.

`MergeGraph` keeps the original graph unchanged and maintains a cluster graph.
When two clusters are merged, it builds a candidate quotient graph and accepts
the merge only if `GraphCycles::InitFrom()` confirms that the quotient remains
acyclic. This catches cases where individually legal clusters would create a
cycle after contraction.

## Test Coverage

The driver covers both basic and scale-oriented scenarios:

- small DAG initialization and rank invariant checks;
- cyclic input rejection;
- incremental edge insertion and cycle-edge rejection;
- small partition merge and quotient-cycle rejection;
- 4096-node DAG initialization, rank checks, and topological-order validation;
- 4096-node graph with a single back edge to verify cycle rejection at scale;
- 768-node incremental insertion with long-range forward edges and rejected
  back edges;
- 512-node partition graph with adjacent pair contractions and a rejected
  contraction that would cycle the quotient graph.
