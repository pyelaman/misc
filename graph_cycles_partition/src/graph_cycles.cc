#include "util_graph/graph_cycles.h"

#include <algorithm>
#include <queue>

namespace util_graph {

bool GraphCycles::InitFrom(const Graph& src) {
  if (!Empty()) {
    return false;
  }

  for (Node node = 0; node < src.NodeLimit(); ++node) {
    EnsureNode(node);
  }

  rank_.assign(src.NodeLimit(), kMissingNodeRank);
  Rank last_rank = static_cast<Rank>(src.NodeLimit());
  auto leave = [&](Node node) {
    rank_[node] = --last_rank;
  };
  DFSAll(src, std::nullopt, leave);

  bool have_cycle = false;
  src.PerEdge([&](Edge e) {
    if (rank_[e.src] >= rank_[e.dst]) {
      have_cycle = true;
      return;
    }
    if (!HasEdge(e.src, e.dst)) {
      Graph::AddEdge(e.src, e.dst);
    }
  });

  if (have_cycle) {
    Clear();
    rank_.clear();
    return false;
  }
  return CheckInvariants();
}

bool GraphCycles::AddEdgeChecked(Node src, Node dst) {
  EnsureNode(std::max(src, dst));
  if (rank_.size() < NodeLimit()) {
    rank_.resize(NodeLimit(), kMissingNodeRank);
    RecomputeRanks();
  }

  if (src == dst) {
    return false;
  }
  if (HasEdge(src, dst)) {
    return true;
  }
  if (HasPath(dst, src)) {
    return false;
  }

  Graph::AddEdge(src, dst);
  return RecomputeRanks() && CheckInvariants();
}

bool GraphCycles::HasPath(Node start, Node target) const {
  if (start >= NodeLimit() || target >= NodeLimit()) {
    return false;
  }
  std::vector<std::uint8_t> visited(NodeLimit(), 0);
  std::queue<Node> queue;
  visited[start] = 1;
  queue.push(start);

  while (!queue.empty()) {
    Node node = queue.front();
    queue.pop();
    if (node == target) {
      return true;
    }
    for (Node dst : Successors(node)) {
      if (!visited[dst]) {
        visited[dst] = 1;
        queue.push(dst);
      }
    }
  }
  return false;
}

bool GraphCycles::RecomputeRanks() {
  bool has_cycle = false;
  std::vector<Node> order = TopologicalOrder(*this, &has_cycle);
  if (has_cycle) {
    return false;
  }

  rank_.assign(NodeLimit(), kMissingNodeRank);
  for (Rank idx = 0; idx < order.size(); ++idx) {
    rank_[order[idx]] = idx;
  }
  return true;
}

bool GraphCycles::CheckInvariants() const {
  if (rank_.size() != NodeLimit()) {
    return false;
  }
  std::vector<std::uint8_t> seen(NodeLimit(), 0);
  for (Rank rank : rank_) {
    if (rank == kMissingNodeRank || rank >= NodeLimit() || seen[rank]) {
      return false;
    }
    seen[rank] = 1;
  }

  bool ok = true;
  PerEdge([&](Edge e) {
    if (rank_[e.src] >= rank_[e.dst]) {
      ok = false;
    }
  });
  return ok;
}

std::vector<Node> GraphCycles::RankedNodes() const {
  std::vector<Node> nodes(NodeLimit());
  for (Node node = 0; node < NodeLimit(); ++node) {
    nodes[rank_[node]] = node;
  }
  return nodes;
}

}  // namespace util_graph
