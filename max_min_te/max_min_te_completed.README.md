# Weighted max-min traffic engineering

Companion design notes for `max_min_te_completed.cpp`.

## Purpose

The program is a compact teaching implementation of the central traffic-engineering idea in Google's B4 WAN: allocate scarce inter-site capacity fairly, route each aggregate flow group over a small set of tunnels, and translate fractional tunnel rates into forwarding buckets.

It demonstrates the allocation mechanism; it is not a reproduction of the production B4 controller, its OpenFlow control plane, or its failure-management system.

## B4 objectives represented here

| B4 idea | Representation in this program |
|---|---|
| Optimize aggregates, not individual connections | `FlowGroup` is the allocation unit. |
| Model the WAN as site-to-site capacity | `Edge` is a directed inter-site link with capacity and residual capacity. |
| Use a small set of candidate paths | Each flow group owns a list of `Tunnel` objects. |
| Allocate according to policy | `weight` controls how quickly a flow group grows during progressive filling. |
| Respect shared bottlenecks | Every tunnel carries edge IDs; an edge constrains all tunnels that cross it. |
| Prefer efficient routes | The allocator selects the lowest-cost currently usable tunnel. |
| Split an aggregate across tunnels | `tunnelRateGbps` records the continuous allocation per tunnel. |
| Program finite forwarding tables | `quantize()` converts rates into a fixed number of buckets. |

The B4 paper defines a flow group more specifically as `{source site, destination site, QoS}` and composes the bandwidth functions of multiple applications within that group. The example uses named aggregate demands and constant weights, so it captures the allocator's shape without implementing B4's complete policy model.

## Data model and invariants

### Edge

An `Edge` has a configured `capacityGbps`, a mutable `residualGbps`, and a `saturated` flag.

The intended invariant is:

```text
0 <= residualGbps <= capacityGbps
usedGbps = capacityGbps - residualGbps
```

### Tunnel

A `Tunnel` is a sparse path: its `edges` vector contains only the IDs of edges it traverses. `cost` provides path preference. A tunnel is usable only while every edge on it has positive residual capacity and is not marked saturated.

### FlowGroup

A `FlowGroup` contains source, destination, traffic class, demand, weight, candidate tunnels, total allocation, and per-tunnel rates. The allocator maintains:

```text
allocatedGbps = sum(tunnelRateGbps)
0 <= allocatedGbps <= demandGbps
```

## Allocation algorithm

The implementation is event-driven weighted progressive filling.

1. Reset edge residuals and all flow-group allocations.
2. For each unsatisfied flow group, select the lowest-cost usable tunnel.
3. Treat every active flow group as growing at its policy weight `w_i`.
4. Compute the time until either a demand is satisfied or an edge fills.
5. Advance all active flow groups by the smallest such time.
6. Charge each increment to the edges in its selected tunnel.
7. Mark newly exhausted edges saturated and repeat with remaining paths.
8. Stop when every demand is met or no flow group has a usable tunnel.

For active flow group `i`, selected tunnel `p_i`, remaining demand `d_i`, and weight `w_i`, its demand-completion event is:

```text
t_demand(i) = d_i / w_i
```

For edge `e`, let `A(e)` be active flow groups whose selected tunnel crosses that edge. Its saturation event is:

```text
t_edge(e) = residual(e) / sum(w_i for i in A(e))
```

The next event time is the minimum positive demand or edge event. Flow group `i` then receives `w_i * t` Gbps on its selected tunnel. This produces weighted max-min behavior: competing groups increase in proportion to their weights until one finishes or a shared resource becomes a bottleneck.

This code realizes “freezing” indirectly. Once an edge is saturated, tunnels crossing it become unusable. A flow group can continue on another candidate tunnel, which mirrors B4's progression from preferred paths to alternatives.

## Example topology

The sample constructs five directed 10-Gbps links:

```text
A --10--> B
|         ^
10        | 10
v         |
C --------+
^
| 10
D
^
| 10
A
```

`FG-backup` has 20 Gbps of demand and weight 2. It can use `A-B`, `A-C-B`, and `A-D-C-B`. `FG-sync` has 10 Gbps of demand and weight 1 and can use `A-C` and `A-D-C`.

The deliberately chosen demands and paths allow all five links to reach full utilization. That is a property of this example, not a guarantee of max-min fairness. Fairness maximizes the minimum policy-relative allocation subject to path and capacity constraints; arbitrary topology, demand, headroom, or path restrictions can leave links underutilized.

## Fractional splits and bucket quantization

The continuous allocator produces a rate `r_j` for each tunnel. Its ideal fraction is:

```text
fraction_j = r_j / sum(r)
```

`quantize()` maps that fraction to `bucketCount` integer buckets using the largest-remainder method:

1. Compute each exact bucket count.
2. Assign its floor.
3. Give leftover buckets to the largest fractional remainders.

This guarantees exactly `bucketCount` assigned buckets and closely approximates the continuous ratio. It does not re-solve feasibility after rounding. Production B4 quantization evaluates the effects on global fair share and throughput, so this local method is educational rather than production-safe.

## Complexity

Let `F` be flow groups, `P` the average tunnels per group, `L` the average edges per tunnel, and `K` allocation events. The straightforward scans cost approximately:

```text
O(K * F * P * L)
```

The example is intentionally small. A large controller would index edge-to-tunnel membership, cache paths, recompute affected aggregates, and separate policy computation from device programming.

## Build and run

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic \
    max_min_te_completed.cpp -o max_min_te_completed
./max_min_te_completed
```

Inspect these properties in the output:

- no flow group exceeds its demand;
- no link exceeds capacity;
- per-tunnel rates sum to each flow group's allocation;
- bucket counts sum to the requested bucket count;
- the example's link utilizations reach 100%.

## Important differences from B4

- Candidate tunnels are supplied manually rather than generated from topology and policy.
- Constant weights replace per-application piecewise bandwidth functions.
- The allocator is a compact greedy progressive-filling implementation, not B4's complete TE optimizer.
- Quantization is local largest-remainder rounding rather than global fairness-aware quantization.
- There is no source-edge rate limiter, topology discovery, failure headroom, or admission control.
- There is no Traffic Engineering Database, OpenFlow programming, dependency ordering, acknowledgement, retry, or transactional rollout.

These boundaries matter: the program is best used to understand the interaction of fairness, bottlenecks, multipath routing, and finite forwarding buckets.

## References

- Google, *B4: Experience with a Globally-Deployed Software Defined WAN*: https://research.google.com/pubs/archive/41761.pdf
- Google, *B4 and After: Managing Hierarchy, Partitioning, and Asymmetry for Availability and Scale*: https://research.google/pubs/b4-and-after-managing-hierarchy-partitioning-and-asymmetry-for-availability-and-scale-in-googles-software-defined-wan/
