#ifndef UTIL_GRAPH_GRAPH_PARTITIONER_H_
#define UTIL_GRAPH_GRAPH_PARTITIONER_H_

#include <string>
#include <vector>

#include "util_graph/graph.h"
#include "util_graph/graph_cycles.h"

namespace util_graph {

class MergeGraph {
 public:
  bool Init(const Graph& graph, std::string* error = nullptr);
  bool MergeClusters(Node a, Node b, std::string* error = nullptr);

  const Graph& original_graph() const { return original_; }
  const GraphCycles& cluster_graph() const { return cluster_graph_; }

  std::vector<std::vector<Node>> Partitions() const;
  std::size_t ClusterOf(Node node) const;

 private:
  bool BuildClusterGraphWithMerge(std::size_t keep, std::size_t absorb,
                                  GraphCycles* out,
                                  std::string* error) const;

  Graph original_;
  GraphCycles cluster_graph_;
  std::vector<std::vector<Node>> clusters_;
  std::vector<std::size_t> cluster_of_;
  std::vector<bool> active_;
};

}  // namespace util_graph

#endif  // UTIL_GRAPH_GRAPH_PARTITIONER_H_
