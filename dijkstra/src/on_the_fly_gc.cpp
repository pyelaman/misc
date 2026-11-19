#include "dijkstra_concurrency/on_the_fly_gc.hpp"

#include <queue>
#include <stdexcept>
#include <utility>

namespace dijkstra::concurrency {

OnTheFlyGarbageCollector::OnTheFlyGarbageCollector(
    const std::size_t node_count,
    const std::size_t edges_per_node,
    std::vector<NodeId> roots,
    std::vector<NodeId> initial_edges)
    : node_count_(node_count),
      edges_per_node_(edges_per_node),
      roots_(std::move(roots)),
      edges_(std::make_unique<std::atomic<NodeId>[]>(
          node_count * edges_per_node)),
      white_(std::make_unique<std::atomic<bool>[]>(node_count)),
      black_(std::make_unique<std::atomic<bool>[]>(node_count)),
      reclaimed_(std::make_unique<std::atomic<bool>[]>(node_count)) {
    if (node_count_ == 0 || edges_per_node_ == 0) {
        throw std::invalid_argument(
            "collector requires nodes with at least one edge each");
    }
    if (roots_.empty()) {
        throw std::invalid_argument("collector requires at least one root");
    }
    if (initial_edges.size() != node_count_ * edges_per_node_) {
        throw std::invalid_argument("initial edge table has the wrong size");
    }

    for (const NodeId root : roots_) {
        check_node(root);
    }
    for (std::size_t i = 0; i < initial_edges.size(); ++i) {
        check_node(initial_edges[i]);
        edges_[i].store(initial_edges[i]);
    }
    for (NodeId node = 0; node < node_count_; ++node) {
        white_[node].store(true);
        black_[node].store(false);
        reclaimed_[node].store(false);
    }
}

void OnTheFlyGarbageCollector::check_node(const NodeId node) const {
    if (node >= node_count_) {
        throw std::out_of_range("node id is outside collector heap");
    }
}

std::size_t OnTheFlyGarbageCollector::edge_index(
    const NodeId node,
    const std::size_t edge) const {
    check_node(node);
    if (edge >= edges_per_node_) {
        throw std::out_of_range("edge id is outside collector node");
    }
    return node * edges_per_node_ + edge;
}

void OnTheFlyGarbageCollector::shade(const NodeId node) {
    // white=false means at least gray. A concurrent black node remains black.
    white_[node].store(false);
}

bool OnTheFlyGarbageCollector::is_gray(const NodeId node) const {
    return !white_[node].load() && !black_[node].load();
}

void OnTheFlyGarbageCollector::redirect(
    const NodeId source,
    const std::size_t edge,
    const NodeId target) {
    check_node(source);
    check_node(target);
    if (reclaimed_[source].load() || reclaimed_[target].load()) {
        throw std::logic_error(
            "redirect source and target must be reachable, allocated nodes");
    }

    // Corrected fine-grained mutator barrier from the published algorithm:
    // place the edge first, then shade the target as a separate action.
    edges_[edge_index(source, edge)].store(target);
    shade(target);
}

OnTheFlyGarbageCollector::CollectionResult
OnTheFlyGarbageCollector::collect() {
    if (collector_active_.test_and_set()) {
        throw std::logic_error("only one collector may run at a time");
    }
    struct ClearFlag {
        std::atomic_flag& flag;
        ~ClearFlag() { flag.clear(); }
    } clear{collector_active_};

    for (const NodeId root : roots_) {
        shade(root);
    }

    // Scan until a complete pass sees no gray node. Discovering one resets
    // the remaining scan count, as in the paper's k := M marking cycle.
    std::size_t node = 0;
    std::size_t remaining = node_count_;
    while (remaining > 0) {
        if (!reclaimed_[node].load() && is_gray(node)) {
            remaining = node_count_;
            for (std::size_t edge = 0; edge < edges_per_node_; ++edge) {
                const NodeId target =
                    edges_[edge_index(node, edge)].load();
                shade(target);
            }
            black_[node].store(true);
        } else {
            --remaining;
        }
        node = (node + 1) % node_count_;
    }

    CollectionResult result;
    for (NodeId candidate = 0; candidate < node_count_; ++candidate) {
        if (reclaimed_[candidate].load()) {
            continue;
        }

        if (white_[candidate].load()) {
            // At the end of marking there are no gray nodes; the invariant
            // therefore establishes that a white node is garbage.
            reclaimed_[candidate].store(true);
            result.reclaimed.push_back(candidate);
        } else if (black_[candidate].load()) {
            black_[candidate].store(false);
            white_[candidate].store(true);
        }
    }
    return result;
}

bool OnTheFlyGarbageCollector::is_reclaimed(const NodeId node) const {
    check_node(node);
    return reclaimed_[node].load();
}

std::vector<OnTheFlyGarbageCollector::NodeId>
OnTheFlyGarbageCollector::edge_snapshot(const NodeId node) const {
    check_node(node);
    std::vector<NodeId> result(edges_per_node_);
    for (std::size_t edge = 0; edge < edges_per_node_; ++edge) {
        result[edge] = edges_[edge_index(node, edge)].load();
    }
    return result;
}

std::vector<OnTheFlyGarbageCollector::NodeId>
OnTheFlyGarbageCollector::reachable_snapshot() const {
    std::vector<bool> seen(node_count_, false);
    std::queue<NodeId> pending;
    for (const NodeId root : roots_) {
        if (!reclaimed_[root].load() && !seen[root]) {
            seen[root] = true;
            pending.push(root);
        }
    }

    while (!pending.empty()) {
        const NodeId source = pending.front();
        pending.pop();
        for (std::size_t edge = 0; edge < edges_per_node_; ++edge) {
            const NodeId target =
                edges_[edge_index(source, edge)].load();
            if (!reclaimed_[target].load() && !seen[target]) {
                seen[target] = true;
                pending.push(target);
            }
        }
    }

    std::vector<NodeId> result;
    for (NodeId node = 0; node < node_count_; ++node) {
        if (seen[node]) {
            result.push_back(node);
        }
    }
    return result;
}

} // namespace dijkstra::concurrency
