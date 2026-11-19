#include "util_graph/graph.h"

#include <algorithm>
#include <limits>

namespace util_graph {

Node Graph::AddNode() {
  if (adjacency_.size() == std::numeric_limits<Node>::max()) {
    throw std::overflow_error("graph node id overflow");
  }
  Node node = static_cast<Node>(adjacency_.size());
  adjacency_.push_back({});
  return node;
}

void Graph::EnsureNode(Node node) {
  while (adjacency_.size() <= node) {
    AddNode();
  }
}

bool Graph::AddEdge(Node src, Node dst) {
  EnsureNode(std::max(src, dst));
  if (HasEdge(src, dst)) {
    return false;
  }
  adjacency_[src].push_back(dst);
  ++edge_count_;
  return true;
}

bool Graph::RemoveEdge(Node src, Node dst) {
  if (src >= adjacency_.size()) {
    return false;
  }
  auto& out = adjacency_[src];
  auto it = std::find(out.begin(), out.end(), dst);
  if (it == out.end()) {
    return false;
  }
  out.erase(it);
  --edge_count_;
  return true;
}

bool Graph::HasEdge(Node src, Node dst) const {
  if (src >= adjacency_.size()) {
    return false;
  }
  const auto& out = adjacency_[src];
  return std::find(out.begin(), out.end(), dst) != out.end();
}

void Graph::Clear() {
  adjacency_.clear();
  edge_count_ = 0;
}

const std::vector<Node>& Graph::Successors(Node node) const {
  if (node >= adjacency_.size()) {
    throw std::out_of_range("node is outside graph");
  }
  return adjacency_[node];
}

std::vector<Node> Graph::Predecessors(Node node) const {
  if (node >= adjacency_.size()) {
    throw std::out_of_range("node is outside graph");
  }
  std::vector<Node> preds;
  for (Node src = 0; src < adjacency_.size(); ++src) {
    for (Node dst : adjacency_[src]) {
      if (dst == node) {
        preds.push_back(src);
      }
    }
  }
  return preds;
}

std::vector<Node> TopologicalOrder(const Graph& graph, bool* has_cycle) {
  std::vector<std::uint32_t> indegree(graph.NodeLimit(), 0);
  graph.PerEdge([&](Edge e) { ++indegree[e.dst]; });

  std::queue<Node> ready;
  for (Node node = 0; node < graph.NodeLimit(); ++node) {
    if (indegree[node] == 0) {
      ready.push(node);
    }
  }

  std::vector<Node> order;
  order.reserve(graph.NodeLimit());
  while (!ready.empty()) {
    Node node = ready.front();
    ready.pop();
    order.push_back(node);
    for (Node dst : graph.Successors(node)) {
      if (--indegree[dst] == 0) {
        ready.push(dst);
      }
    }
  }

  bool cycle = order.size() != graph.NodeLimit();
  if (has_cycle != nullptr) {
    *has_cycle = cycle;
  }
  return order;
}

bool HasCycle(const Graph& graph) {
  bool has_cycle = false;
  TopologicalOrder(graph, &has_cycle);
  return has_cycle;
}

}  // namespace util_graph
