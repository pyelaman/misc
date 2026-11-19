#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace dijkstra::concurrency {

// A fixed-size graph model of the chapter's on-the-fly collector. It is a
// pedagogical implementation of the marking protocol, not a C++ allocator.
class OnTheFlyGarbageCollector {
public:
    using NodeId = std::size_t;

    struct CollectionResult {
        std::vector<NodeId> reclaimed;
    };

    OnTheFlyGarbageCollector(
        std::size_t node_count,
        std::size_t edges_per_node,
        std::vector<NodeId> roots,
        std::vector<NodeId> initial_edges);

    OnTheFlyGarbageCollector(const OnTheFlyGarbageCollector&) = delete;
    OnTheFlyGarbageCollector& operator=(
        const OnTheFlyGarbageCollector&) = delete;

    // Precondition from the paper: source and target are both reachable at
    // the start of the operation. The store and shade are separate atomic
    // actions, in that order, implementing the corrected fine-grained barrier.
    void redirect(NodeId source, std::size_t edge, NodeId target);

    // Only one collector invocation may execute at a time. Mutator redirects
    // may execute concurrently.
    [[nodiscard]] CollectionResult collect();

    [[nodiscard]] bool is_reclaimed(NodeId node) const;
    [[nodiscard]] std::vector<NodeId> reachable_snapshot() const;
    [[nodiscard]] std::vector<NodeId> edge_snapshot(NodeId node) const;

private:
    void check_node(NodeId node) const;
    [[nodiscard]] std::size_t edge_index(
        NodeId node,
        std::size_t edge) const;
    void shade(NodeId node);
    [[nodiscard]] bool is_gray(NodeId node) const;

    std::size_t node_count_;
    std::size_t edges_per_node_;
    std::vector<NodeId> roots_;
    std::unique_ptr<std::atomic<NodeId>[]> edges_;
    std::unique_ptr<std::atomic<bool>[]> white_;
    std::unique_ptr<std::atomic<bool>[]> black_;
    std::unique_ptr<std::atomic<bool>[]> reclaimed_;
    std::atomic_flag collector_active_ = ATOMIC_FLAG_INIT;
};

} // namespace dijkstra::concurrency
