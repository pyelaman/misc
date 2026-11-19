#include "util_graph/graph_partitioner.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace util_graph {

bool MergeGraph::Init(const Graph& graph, std::string* error) {
  original_.Clear();
  cluster_graph_.Clear();
  clusters_.clear();
  cluster_of_.clear();
  active_.clear();

  for (Node node = 0; node < graph.NodeLimit(); ++node) {
    original_.EnsureNode(node);
  }
  graph.PerEdge([&](Edge e) { original_.AddEdge(e.src, e.dst); });

  if (!cluster_graph_.InitFrom(graph)) {
    if (error != nullptr) {
      *error = "cycle in the original graph";
    }
    return false;
  }

  clusters_.resize(graph.NodeLimit());
  cluster_of_.resize(graph.NodeLimit());
  active_.assign(graph.NodeLimit(), true);
  for (Node node = 0; node < graph.NodeLimit(); ++node) {
    clusters_[node].push_back(node);
    cluster_of_[node] = node;
  }
  return true;
}

std::size_t MergeGraph::ClusterOf(Node node) const {
  if (node >= cluster_of_.size()) {
    throw std::out_of_range("node is outside partitioner");
  }
  return cluster_of_[node];
}

std::vector<std::vector<Node>> MergeGraph::Partitions() const {
  std::vector<std::vector<Node>> result;
  for (std::size_t id = 0; id < clusters_.size(); ++id) {
    if (active_[id]) {
      result.push_back(clusters_[id]);
    }
  }
  return result;
}

bool MergeGraph::BuildClusterGraphWithMerge(std::size_t keep,
                                            std::size_t absorb,
                                            GraphCycles* out,
                                            std::string* error) const {
  Graph candidate;
  for (Node id = 0; id < clusters_.size(); ++id) {
    candidate.EnsureNode(id);
  }

  auto remap = [&](std::size_t cluster) {
    return cluster == absorb ? keep : cluster;
  };

  original_.PerEdge([&](Edge e) {
    std::size_t src_cluster = remap(cluster_of_[e.src]);
    std::size_t dst_cluster = remap(cluster_of_[e.dst]);
    if (src_cluster != dst_cluster) {
      candidate.AddEdge(static_cast<Node>(src_cluster),
                        static_cast<Node>(dst_cluster));
    }
  });

  GraphCycles checked;
  if (!checked.InitFrom(candidate)) {
    if (error != nullptr) {
      std::ostringstream os;
      os << "merge would introduce a cycle in the cluster graph: "
         << keep << " <- " << absorb;
      *error = os.str();
    }
    return false;
  }
  *out = checked;
  return true;
}

bool MergeGraph::MergeClusters(Node a, Node b, std::string* error) {
  if (a >= cluster_of_.size() || b >= cluster_of_.size()) {
    if (error != nullptr) {
      *error = "merge node is outside graph";
    }
    return false;
  }

  std::size_t keep = cluster_of_[a];
  std::size_t absorb = cluster_of_[b];
  if (keep == absorb) {
    return true;
  }
  if (!active_[keep] || !active_[absorb]) {
    if (error != nullptr) {
      *error = "merge references an inactive cluster";
    }
    return false;
  }

  GraphCycles candidate;
  if (!BuildClusterGraphWithMerge(keep, absorb, &candidate, error)) {
    return false;
  }

  clusters_[keep].insert(clusters_[keep].end(), clusters_[absorb].begin(),
                         clusters_[absorb].end());
  std::sort(clusters_[keep].begin(), clusters_[keep].end());
  for (Node node : clusters_[absorb]) {
    cluster_of_[node] = keep;
  }
  clusters_[absorb].clear();
  active_[absorb] = false;
  cluster_graph_ = candidate;
  return true;
}

}  // namespace util_graph
