#ifndef UTIL_GRAPH_GRAPH_CYCLES_H_
#define UTIL_GRAPH_GRAPH_CYCLES_H_

#include <cstdint>
#include <limits>
#include <vector>

#include "util_graph/graph.h"

namespace util_graph {

class GraphCycles : public Graph {
 public:
  using Rank = std::uint32_t;
  static constexpr Rank kMissingNodeRank = std::numeric_limits<Rank>::max();

  GraphCycles() = default;
  ~GraphCycles() override = default;

  bool InitFrom(const Graph& src);
  bool AddEdgeChecked(Node src, Node dst);
  bool CheckInvariants() const;

  const std::vector<Rank>& ranks() const { return rank_; }
  std::vector<Node> RankedNodes() const;

 private:
  bool HasPath(Node start, Node target) const;
  bool RecomputeRanks();

  std::vector<Rank> rank_;
};

}  // namespace util_graph

#endif  // UTIL_GRAPH_GRAPH_CYCLES_H_
