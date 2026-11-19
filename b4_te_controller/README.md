# B4-inspired traffic-engineering controller

This directory contains a deployment-oriented C++17 traffic-engineering core derived from the objectives in Google's B4 paper. It is the third stage after the small `max_min_te_completed.cpp` teaching example and `max_min_te_extended.cpp` simulator.

The controller is suitable as a reference core for a modest WAN controller. It is not a drop-in Google B4 implementation: production use still requires durable database and device adapters, authentication, authorization, metrics, high availability, and integration testing against the target switches.

## Build and test

```bash
cd b4_te_controller
make
make test
./max_min_te_b4_controller
```

The build uses C++17 with warnings promoted to errors. `make test` exercises bandwidth-function interpolation, B4 flow-group aggregation, minimum guarantees, allocation conservation, topology-derived paths, global quantization, retry, policy activation, device reconciliation, and unreachable-demand rejection.

## Architecture

```text
application demands and bandwidth=f(fair_share) policies
                         |
                         v
              B4 flow-group aggregation
              {source, destination, QoS}
                         |
discovered topology ---> bounded k-path catalog and cache
 physical members        sparse trunk membership
 aggregated trunks                  |
 failure headroom                   v
                         fair-share TE allocator
                         + admission control
                                   |
                                   v
                         global bucket quantizer
                         with trunk validation
                                   |
                                   v
                 desired -> staged -> active TED policy
                                   |
                                   v
 tunnels -> groups -> policers -> classifiers -> activation
                                   |
                                   v
                  prepare/commit ACK, retry, abort,
                  rollback and version reconciliation
```

## The nine requested extensions

### 1. B4-compatible flow groups

`FlowKey` is exactly source site, destination site, and QoS. Individual `ApplicationDemand` objects remain visible inside the flow group, but the WAN optimizer handles one aggregate. Connection count therefore does not determine TE or forwarding-state size.

Each application has its own demand, admission priority, and bandwidth function. This permits policy composition without incorrectly making the application part of the flow-group identity.

### 2. Fair-share bandwidth functions

`BandwidthFunction` is a validated monotone piecewise-linear mapping:

```text
bandwidth = f(fair_share)
```

The flow-group function is the sum of its admitted application functions, capped by each application's current demand. The allocator advances a common fair-share coordinate and treats bandwidth-function breakpoints as allocation events. This is materially closer to B4 than the fixed or rate-indexed weights in the earlier examples.

The first curve point at fair share zero is the application's minimum guarantee. Later points express preferred and elastic bandwidth regions.

### 3. Traffic-engineering optimizer

`TrafficEngineer` performs event-driven max-min filling in fair-share space. At each event it:

1. selects the lowest-cost usable candidate tunnel for each active flow group;
2. calculates the aggregate bandwidth slope of each group;
3. finds the nearest application breakpoint or trunk saturation;
4. advances all active groups by the same fair-share delta;
5. accounts for the resulting rate on every sparse path resource;
6. uses another candidate tunnel after a bottleneck is exhausted.

Every loop is bounded and validates demand, residual capacity, and numerical progress. This follows B4's preferred-path progressive allocation approach. It is not a general multicommodity-flow LP solver; use an LP oracle in qualification tests if the deployment requires proof against an optimum.

### 4. Global capacity-aware quantization

`GlobalQuantizer` does not round each flow group independently. It maintains global trunk loads while assigning every hardware bucket. A candidate bucket is accepted only if all trunks on its tunnel remain within usable capacity. Among feasible choices, it selects the move with the smallest squared deviation from the continuous tunnel split.

The completed plan is independently capacity-validated. Quantization fails closed if it cannot place all buckets safely. Because the algorithm is greedy, an adversarial feasible problem can theoretically require backtracking; a production adapter should expose the bucket count and allow a solver-backed quantizer when this occurs.

### 5. Source-edge enforcement

Every admitted flow group produces:

- a tunnel object for each used path;
- a weighted tunnel group;
- one aggregate source policer at the allocated rate;
- a source classifier for the flow-group key.

The source policer prevents offered load from exceeding the allocation assumed by TE.

### 6. Topology discovery and headroom

`DiscoveredTopology` accepts idempotent trunk updates from a discovery adapter. Each directed site-to-site trunk aggregates multiple physical members. A member failure immediately changes usable capacity and increments the topology version.

```text
usable capacity = sum(capacity of UP members) * (1 - headroom)
```

Candidate paths are generated from current discovered state using a bounded best-first simple-path search. The cache includes the topology version, path count, and hop limit. Paths store only the trunk IDs they use.

The source file supplies the state model and update API. An actual deployment must connect LLDP, inventory, telemetry, or controller notifications to `upsertTrunk()` and `setMemberState()`.

### 7. Admission control

Admission rejects applications with no candidate tunnel. The controller then allocates minimum guarantees and iteratively removes the lowest admission-priority application from an infeasible flow group. It recomputes after every rejection so rejected load cannot reduce the surviving allocation.

Elastic bandwidth above the minimum may be partially satisfied; that is expected. Callers receive an explicit decision and reason for every demand.

### 8. Traffic Engineering Database

`PolicyStore` defines the TED persistence boundary. It has distinct desired, staged, and active snapshots. A snapshot contains:

- monotonically increasing policy version;
- topology version used by the computation;
- quantized flow groups;
- dependency-ordered device operations.

`InMemoryPolicyStore` is supplied for the demonstration and tests. Replace it with a transactional durable implementation before production—typically an HA SQL or consensus-backed store with optimistic checks on both policy and topology versions.

### 9. Failure-safe device rollout

`DeploymentCoordinator` programs objects in dependency order:

```text
Tunnel -> Group -> Policer -> Classifier -> Activation
```

The `DeviceAdapter` contract requires idempotent prepare, commit, abort, rollback, and active-version reads. Retryable failures use bounded exponential backoff. Any prepare failure aborts staged state. A partial commit failure restores already committed devices to the previous version. The TED becomes active only after all devices acknowledge the new version. `reconciled()` detects device/TED version drift.

`SimulatedDeviceAdapter` provides deterministic retry testing. A real OpenFlow, P4Runtime, gNMI, or vendor adapter must preserve the same contract and use make-before-break resources or atomic indirection where supported.

## Controller invariants

The implementation checks these invariants at runtime:

```text
application demand >= 0
bandwidth functions are monotone
flow-group identity = {source, destination, QoS}
flow-group rate = sum(tunnel rates)
continuous trunk load <= usable trunk capacity
quantized trunk load <= usable trunk capacity
every installed group has exactly the configured bucket count
objects are staged in dependency order
TED active version changes only after all device commits
```

## Reasonable-scale operating envelope

The code is intended for tens to low hundreds of sites, a few thousand trunks, thousands of flow groups, at most a handful of candidate tunnels per site pair, and tens of forwarding buckets per group. Exact capacity depends on topology density and update rate and must be benchmarked with the deployment's data.

Safeguards include:

- bounded candidate-path count, hop count, and search expansions;
- sparse tunnel membership;
- topology-versioned path caches;
- bounded allocation iterations;
- bounded device retries;
- deterministic ordering and tie-breaking;
- fail-closed validation.

The current implementation recomputes all groups after a demand batch. If update frequency or group count makes that expensive, carry forward the dependency-closure mechanism from `max_min_te_extended.cpp`, but preserve snapshot isolation: compute against an immutable topology/demand version and discard the result if either changes before staging.

## Production integration checklist

Before live deployment, supply and qualify:

1. A durable HA `PolicyStore` with schema migration, audit history, fencing, and disaster recovery.
2. An authenticated topology/telemetry adapter with stale-data detection.
3. A device adapter with TLS, authorization, capability discovery, idempotency keys, deadlines, and bounded concurrency.
4. Make-before-break capacity accounting for old and new tunnel groups during rollout.
5. Leader election and fencing so only one coordinator can activate a version.
6. Metrics for allocation latency, rejected demand, fairness, headroom, quantization error, retries, drift, and rollback.
7. Structured logs and traces containing topology, demand, policy, and transaction versions.
8. LP-oracle differential tests and randomized property tests.
9. Failure-injection tests for controller restart, database failover, device timeout, partial commit, topology churn, and stale acknowledgement.
10. Load and soak tests using the expected topology and demand-update distributions.

Do not describe the demonstration backend as an OpenFlow implementation. `Operation` objects are an intermediate device-independent plan; only a tested southbound adapter should translate them into hardware operations.

## Relationship to B4

The implementation follows the B4 paper's major abstractions: site/trunk topology, source-destination-QoS flow groups, per-application bandwidth functions, preferred candidate tunnels, max-min fair-share allocation, tunnel-group quantization, source enforcement, and a desired/current control-plane model.

Google's production system included operational machinery and deployment-specific optimizations beyond this standalone implementation. Google's later “B4 and After” work also introduced hierarchy and partitioning for substantially larger scale and availability domains. This controller deliberately keeps one logical computation and one deployment coordinator; horizontal partitioning is a future boundary, not something hidden inside the example.

## References

- Google, *B4: Experience with a Globally-Deployed Software Defined WAN*: https://research.google.com/pubs/archive/41761.pdf
- Google, *B4 and After: Managing Hierarchy, Partitioning, and Asymmetry for Availability and Scale*: https://research.google/pubs/b4-and-after-managing-hierarchy-partitioning-and-asymmetry-for-availability-and-scale-in-googles-software-defined-wan/
