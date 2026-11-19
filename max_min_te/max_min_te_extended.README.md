# Extended B4-inspired traffic-engineering controller

Companion design notes for `max_min_te_extended.cpp`.

## Purpose

This program extends the basic weighted max-min example toward the architecture needed by a large inter-datacenter WAN. It models physical-link aggregation, cached candidate tunnels, aggregate demands, piecewise policy, incremental recomputation, forwarding-table quantization, source-edge enforcement, transactional versions, and capacity headroom.

The implementation is a single-process simulator. Its value is the separation of concerns and the explicit invariants; it is not wire-compatible with B4 and does not implement B4's distributed control plane.

## Constraint-to-design map

| Constraint | Design in this implementation | Relationship to B4 |
|---|---|---|
| Aggregate application traffic | `ConnectionDemand` records are grouped into `FlowGroup` objects by `FlowKey`. | B4 controls flow groups rather than individual connections. B4's FG key is source, destination, and QoS; see the semantic difference below. |
| Aggregate physical links | `Trunk` sums usable `LinkMember` capacities. | B4's TE graph represents sites as nodes and site-to-site trunks as edges. |
| Cache a small tunnel set | `Topology::candidateTunnels()` uses DFS, sorts by cost, limits results, and caches by source/destination/topology version. | B4 generates a constrained set of tunnel groups instead of optimizing arbitrary packet paths. |
| Sparse path membership | A `Tunnel` stores only the trunk IDs on its path. | This is the natural sparse tunnel-to-edge representation used by the allocator. |
| Recompute affected traffic | `resourceClosure()` grows a closure of changed trunks and dependent flow groups. Unaffected allocations are preserved. | B4's logically centralized state enables targeted recalculation, although this exact closure algorithm is specific to the example. |
| Piecewise policy | `BandwidthFunction` contains rate breakpoints and marginal weights. | Inspired by B4 bandwidth functions, but not semantically identical; details below. |
| Finite ECMP buckets | Largest-remainder quantization maps tunnel rates to integer buckets. | B4 quantizes tunnel-group fractions for switch hardware. |
| Source-edge rate enforcement | `TransactionalDataplane` emits a policer rate for each flow group. | B4 limits aggregate traffic at the source site so offered load follows the TE allocation. |
| Versioned installation | A candidate policy is validated, assigned a monotonically increasing version, and committed as a unit in the simulator. | B4 maintains desired/current TE state and installs dependencies safely, but has a richer protocol. |
| Failure/burst headroom | Each trunk reserves a configurable fraction before allocation. | Operational extension for survivability; original B4 emphasized high utilization, elastic applications, and exposing failures rather than conventional overprovisioned protection. |

## Architecture

```text
connection demands
       |
       v
DemandAggregator ----> aggregate flow groups + bandwidth policy
       |                              |
       v                              v
Topology cache ----> candidate tunnels ----> IncrementalAllocator
 physical members       sparse trunk IDs        weighted filling
 trunk headroom                                  |
                                                   v
                                  continuous tunnel allocations
                                                   |
                                      bucket quantization
                                                   |
                                                   v
                                 TransactionalDataplane
                                  policers + tunnel groups
```

The layers deliberately keep demand, topology, optimization, rounding, and deployment separate. That boundary is essential at scale: each part changes at a different frequency and has different correctness requirements.

## 1. Physical links become TE trunks

`LinkMember` represents a physical circuit. `Trunk` represents the directed site-to-site resource visible to TE.

For trunk `e` with member capacities `c_m`, member availability `up_m`, and headroom fraction `h_e`:

```text
rawCapacity(e)       = sum(c_m for available members)
allocatableCapacity  = rawCapacity(e) * (1 - h_e)
```

Member failure changes the trunk capacity and increments the topology version. The TE layer need not optimize every physical member separately, which keeps the graph small and matches B4's site/trunk abstraction.

Headroom has a real cost: it deliberately prevents 100% allocation of raw capacity. Use it only when failure recovery or burst absorption is an explicit policy requirement. A production system may vary it by trunk risk, maintenance state, or traffic class rather than applying one constant fraction.

## 2. Candidate tunnel generation and caching

`Topology::candidateTunnels()` enumerates simple site paths with bounded depth, converts each path into a sparse trunk-ID vector, sorts by cost, and retains a limited number. The cache key includes source, destination, and topology version, so any topology mutation invalidates old results logically.

Why bound the set:

- optimization cost is proportional to candidate paths;
- forwarding hardware has finite groups and buckets;
- long paths consume more network capacity per delivered Gbps;
- a few diverse paths usually capture most useful capacity.

The demonstration's global version invalidation is safe but coarse. At scale, cache entries should depend on the specific trunks and nodes they use, allowing local invalidation after a member or trunk change.

## 3. Demand aggregation

`DemandAggregator` turns many `ConnectionDemand` records into fewer flow groups and sums their demand. This prevents controller and switch state from growing with every transport connection.

There is an important B4 fidelity issue: this implementation's `FlowKey` includes `application`, source, destination, and traffic class. It therefore aggregates connections within one application but keeps different applications in separate flow groups. In the B4 paper, a flow group is `{source site, destination site, QoS}` and multiplexes applications; their application bandwidth functions are composed into the FG bandwidth function.

To make the model B4-faithful:

1. Remove `application` from `FlowKey`.
2. Retain application sub-demands inside the aggregate.
3. Compose their bandwidth functions into one flow-group function.
4. Divide the resulting FG allocation among applications according to the policy function.

The present choice is still useful when per-application policers or accounting are desired, but it consumes more controller and forwarding state.

## 4. Piecewise bandwidth policy

`BandwidthFunction` contains segments with an `upToGbps` breakpoint and a marginal `weight`. As allocation crosses a breakpoint, the flow group's growth rate changes. This can express policies such as:

- a high-priority guaranteed initial region;
- a lower-priority elastic region;
- diminishing preference after an application reaches its useful rate.

The allocator treats the next breakpoint as another event, alongside demand completion and trunk saturation.

B4's published bandwidth function has a different formal meaning: it maps a scalar fair-share value to an application's bandwidth, and functions are composed additively for the flow group. The code's rate-indexed marginal weights are an intuitive approximation, not an exact implementation of that composition. Production fidelity requires representing `bandwidth = f(fair_share)`, evaluating/inverting the composed piecewise-linear function, and performing max-min filling in fair-share space.

## 5. Incremental resource closure

After a demand or topology change, recomputing every flow group is wasteful. `resourceClosure()` starts with changed flow groups or trunks and repeatedly includes:

- flow groups whose candidate tunnels touch an affected trunk;
- trunks touched by candidate tunnels of an affected flow group.

The fixed point is the set whose capacity constraints can interact. Allocations outside the closure are preserved and charged against residual capacity before the affected set is solved again.

Correctness requirement:

```text
Every preserved allocation must remain feasible on the current topology,
and every resource shared with a changed allocation must enter the closure.
```

The demonstration favors clarity over minimality. At scale, maintain bidirectional indexes (`flow group -> trunks`, `trunk -> flow groups`) and update them when tunnel caches change. Also handle flow deletion explicitly so stale allocations cannot remain charged.

## 6. Weighted progressive filling

Within the affected closure, the allocator repeatedly:

1. chooses each unsatisfied flow group's cheapest usable tunnel;
2. determines its current marginal weight from the piecewise function;
3. computes the nearest demand, policy-breakpoint, or trunk-capacity event;
4. advances every active group to that event;
5. charges tunnel increments to trunk residuals;
6. moves blocked groups to alternative tunnels where possible.

This is a greedy, event-driven approximation of B4's fairness mechanism. It preserves the important local invariants:

```text
0 <= FG allocation <= FG demand
sum(FG tunnel rates) = FG allocation
sum(rates crossing trunk) <= allocatable trunk capacity
```

It does not guarantee the same global optimum as a full linear program or B4's complete optimizer for every candidate-path arrangement. Tests should compare small cases against an LP oracle and include shared bottlenecks, unequal weights, breakpoint crossings, unreachable demands, and topology failures.

## 7. Bucket quantization

Continuous rate fractions cannot always be installed directly. The example uses largest-remainder rounding to map tunnel rates to a fixed number of ECMP/group-table buckets.

Benefits:

- deterministic;
- exact bucket-count conservation;
- small local fraction error;
- straightforward to inspect.

Limitations:

- rounding is independent per flow group;
- it does not verify trunk feasibility after rounding;
- it does not optimize global fair share or throughput;
- very small but important paths can round to zero.

B4's quantization stage evaluates candidate fraction changes using global TE objectives. A production implementation should simulate the quantized loads, reject capacity violations, and choose bucket moves that best preserve fairness and throughput.

## 8. Source enforcement and transactional deployment

The simulated dataplane validates a candidate policy, assigns a version, then prints:

- an aggregate source policer for each flow group;
- a tunnel group and its integer bucket allocation;
- the committed policy version.

This captures two crucial ideas: enforcement belongs at the source boundary, and a new policy should become visible as one coherent version.

It is not a real transaction. B4's Traffic Engineering Database and control plane distinguish desired and current state, create tunnels before objects that reference them, track acknowledgements, and use session/sequence identity plus retries and dirty state. A production rollout should provide:

1. preflight validation of paths, buckets, rates, and device capacity;
2. dependency ordering: tunnels, then groups, then source classifiers/policers;
3. per-device prepare acknowledgements;
4. version activation or make-before-break switching;
5. rollback/garbage collection of the old version;
6. idempotent retries and reconciliation after controller or switch restart.

## Demonstration lifecycle

The program exercises three useful controller events:

1. **Initial computation** — aggregate connection demands, generate candidate tunnels, allocate continuous rates, quantize, and commit a version.
2. **Demand update** — change one aggregate and recompute its resource closure while preserving disconnected allocations.
3. **Physical member failure** — reduce a trunk's usable capacity, invalidate paths through the topology version, recompute affected flow groups, and install a newer policy.

When reviewing output, distinguish raw capacity from allocatable capacity. With headroom enabled, full use of allocatable capacity is expected; 100% use of raw capacity is intentionally not.

## Complexity and scaling direction

Let `S` be sites, `E` trunks, `F` flow groups, `P` retained tunnels per site pair, and `L` average trunks per tunnel.

- Bounded path enumeration can still be exponential before the path/count limits; production systems use k-shortest/diverse-path algorithms.
- Sparse membership stores approximately `O(F * P * L)` references instead of an `F x P x E` dense matrix.
- Allocation cost is driven by the affected closure, not necessarily all `F` groups.
- Quantization is roughly `O(P log P)` per flow group.

For larger deployments, shard computation by topology region while retaining coordination for shared trunks, use immutable versioned snapshots, precompute diverse paths, and debounce bursts of telemetry changes. Google's later *B4 and After* work specifically addresses hierarchy, partitioning, and asymmetry needed beyond a single centralized abstraction.

## Review checklist

Before treating an extension as correct, verify:

- member failures reduce trunk capacity exactly once;
- headroom is applied once and never mistaken for physical loss;
- cached tunnels cannot survive a topology version change incorrectly;
- no preserved allocation uses a failed or removed trunk;
- the resource closure contains every shared bottleneck;
- breakpoint events cannot produce zero-progress loops;
- no flow group exceeds demand;
- no continuous or quantized policy overloads a trunk;
- all bucket counts sum to the hardware group size;
- a failed deployment leaves the previous version active;
- retries are idempotent.

## Recommended production roadmap

1. Align the flow-group key and bandwidth-function semantics with B4.
2. Add an LP reference solver for correctness tests.
3. Replace DFS with bounded k-shortest diverse path generation.
4. Build maintained dependency indexes for incremental closure and cache invalidation.
5. Make quantization globally capacity- and fairness-aware.
6. Add a real desired/current state store and acknowledged deployment protocol.
7. Model multiple failure scenarios and choose risk-based headroom.
8. Add telemetry feedback, hysteresis, and rate-limited recomputation.
9. Split controller computation into versioned snapshots suitable for hierarchical or partitioned operation.

## Build and run

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic \
    max_min_te_extended.cpp -o max_min_te_extended
./max_min_te_extended
```

Useful test assertions include continuous capacity feasibility, demand bounds, allocation conservation, exact bucket-count conservation, monotonic policy versions, preservation of genuinely disconnected allocations, and successful rerouting or explicit shortfall after failure.

## References

- Google, *B4: Experience with a Globally-Deployed Software Defined WAN*: https://research.google.com/pubs/archive/41761.pdf
- Google, *B4 and After: Managing Hierarchy, Partitioning, and Asymmetry for Availability and Scale*: https://research.google/pubs/b4-and-after-managing-hierarchy-partitioning-and-asymmetry-for-availability-and-scale-in-googles-software-defined-wan/
