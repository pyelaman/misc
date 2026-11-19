#ifndef UTIL_GRAPH_GRAPH_H_
#define UTIL_GRAPH_GRAPH_H_

#include <cstdint>
#include <functional>
#include <optional>
#include <queue>
#include <stdexcept>
#include <vector>

namespace util_graph {

using Node = std::uint32_t;

struct Edge {
  Node src;
  Node dst;

  bool operator==(const Edge& other) const {
    return src == other.src && dst == other.dst;
  }
};

class Graph {
 public:
  Graph() = default;
  virtual ~Graph() = default;

  Node AddNode();
  void EnsureNode(Node node);
  virtual bool AddEdge(Node src, Node dst);
  bool RemoveEdge(Node src, Node dst);
  bool HasEdge(Node src, Node dst) const;
  void Clear();

  std::size_t NodeLimit() const { return adjacency_.size(); }
  std::size_t EdgeCount() const { return edge_count_; }
  bool Empty() const { return adjacency_.empty() && edge_count_ == 0; }

  const std::vector<Node>& Successors(Node node) const;
  std::vector<Node> Predecessors(Node node) const;

  template <typename Fn>
  void PerNode(Fn fn) const {
    for (Node node = 0; node < adjacency_.size(); ++node) {
      fn(node);
    }
  }

  template <typename Fn>
  void PerEdge(Fn fn) const {
    for (Node src = 0; src < adjacency_.size(); ++src) {
      for (Node dst : adjacency_[src]) {
        fn(Edge{src, dst});
      }
    }
  }

 protected:
  std::vector<std::vector<Node>> adjacency_;
  std::size_t edge_count_ = 0;
};

namespace internal {

template <typename EnterFn, typename LeaveFn>
void DFSVisit(const Graph& graph, Node node, std::vector<std::uint8_t>& visited,
              const std::optional<EnterFn>& enter, LeaveFn& leave) {
  visited[node] = 1;
  if (enter.has_value()) {
    (*enter)(node);
  }
  for (Node dst : graph.Successors(node)) {
    if (!visited[dst]) {
      DFSVisit(graph, dst, visited, enter, leave);
    }
  }
  leave(node);
}

}  // namespace internal

template <typename EnterFn, typename LeaveFn>
void DFSAll(const Graph& graph, const std::optional<EnterFn>& enter,
            LeaveFn leave) {
  std::vector<std::uint8_t> visited(graph.NodeLimit(), 0);
  for (Node node = 0; node < graph.NodeLimit(); ++node) {
    if (!visited[node]) {
      internal::DFSVisit(graph, node, visited, enter, leave);
    }
  }
}

template <typename LeaveFn>
void DFSAll(const Graph& graph, std::nullopt_t, LeaveFn leave) {
  std::optional<std::function<void(Node)>> enter;
  DFSAll(graph, enter, leave);
}

bool HasCycle(const Graph& graph);
std::vector<Node> TopologicalOrder(const Graph& graph, bool* has_cycle = nullptr);

}  // namespace util_graph

#endif  // UTIL_GRAPH_GRAPH_H_
