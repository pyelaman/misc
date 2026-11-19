#include <cstdlib>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "util_graph/graph.h"
#include "util_graph/graph_cycles.h"
#include "util_graph/graph_partitioner.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
  if (!condition) {
    ++g_failures;
    std::cerr << "FAIL: " << message << '\n';
  } else {
    std::cout << "PASS: " << message << '\n';
  }
}

std::string FormatPartitions(const std::vector<std::vector<util_graph::Node>>& parts) {
  std::ostringstream os;
  os << "[";
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) {
      os << ", ";
    }
    os << "{";
    for (std::size_t j = 0; j < parts[i].size(); ++j) {
      if (j != 0) {
        os << ",";
      }
      os << parts[i][j];
    }
    os << "}";
  }
  os << "]";
  return os.str();
}

util_graph::Graph BuildForwardDag(util_graph::Node nodes) {
  util_graph::Graph graph;
  for (util_graph::Node node = 0; node < nodes; ++node) {
    graph.EnsureNode(node);
  }

  for (util_graph::Node node = 0; node + 1 < nodes; ++node) {
    graph.AddEdge(node, node + 1);
  }
  for (util_graph::Node node = 0; node + 7 < nodes; ++node) {
    graph.AddEdge(node, node + 7);
  }
  for (util_graph::Node node = 0; node + 128 < nodes; node += 3) {
    graph.AddEdge(node, node + 128);
  }
  return graph;
}

bool OrderRespectsEdges(const util_graph::Graph& graph,
                        const std::vector<util_graph::Node>& order) {
  if (order.size() != graph.NodeLimit()) {
    return false;
  }

  std::vector<std::size_t> position(graph.NodeLimit(), graph.NodeLimit());
  for (std::size_t idx = 0; idx < order.size(); ++idx) {
    if (order[idx] >= graph.NodeLimit()) {
      return false;
    }
    position[order[idx]] = idx;
  }

  bool ok = true;
  graph.PerEdge([&](util_graph::Edge edge) {
    if (position[edge.src] >= position[edge.dst]) {
      ok = false;
    }
  });
  return ok;
}

template <typename Fn>
void TimedCheck(const std::string& message, Fn fn) {
  auto start = std::chrono::steady_clock::now();
  bool result = fn();
  auto end = std::chrono::steady_clock::now();
  auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  Check(result, message + " (" + std::to_string(elapsed_ms) + " ms)");
}

void TestAcyclicInit() {
  util_graph::Graph graph;
  graph.AddEdge(0, 1);
  graph.AddEdge(0, 2);
  graph.AddEdge(1, 3);
  graph.AddEdge(2, 3);

  util_graph::GraphCycles checked;
  Check(checked.InitFrom(graph), "GraphCycles::InitFrom accepts a DAG");
  Check(checked.CheckInvariants(), "DAG ranks satisfy every edge ordering");
  Check(checked.EdgeCount() == 4, "DAG edge count preserved");
}

void TestCycleInitRejects() {
  util_graph::Graph graph;
  graph.AddEdge(0, 1);
  graph.AddEdge(1, 2);
  graph.AddEdge(2, 0);

  util_graph::GraphCycles checked;
  Check(!checked.InitFrom(graph), "GraphCycles::InitFrom rejects an initial cycle");
}

void TestIncrementalCycleRejects() {
  util_graph::GraphCycles graph;
  Check(graph.AddEdgeChecked(0, 1), "incremental add 0->1 succeeds");
  Check(graph.AddEdgeChecked(1, 2), "incremental add 1->2 succeeds");
  Check(!graph.AddEdgeChecked(2, 0), "incremental add 2->0 is rejected");
  Check(!graph.HasEdge(2, 0), "rejected cycle edge is not inserted");
  Check(graph.CheckInvariants(), "incremental graph remains invariant-safe");
}

void TestPartitionerInitAndMerge() {
  util_graph::Graph graph;
  graph.AddEdge(0, 2);
  graph.AddEdge(1, 3);

  util_graph::MergeGraph merge_graph;
  std::string error;
  Check(merge_graph.Init(graph, &error), "MergeGraph::Init accepts DAG");
  Check(merge_graph.Partitions().size() == 4, "partitioner starts with one cluster per node");

  Check(merge_graph.MergeClusters(0, 3, &error), "merge 0 and 3 succeeds");
  std::cout << "Partitions after merge 0/3: "
            << FormatPartitions(merge_graph.Partitions()) << '\n';

  Check(!merge_graph.MergeClusters(1, 2, &error),
        "merge 1 and 2 is rejected because it creates a cluster cycle");
  std::cout << "Rejected merge reason: " << error << '\n';
}

void TestPartitionerRejectsCyclicInput() {
  util_graph::Graph graph;
  graph.AddEdge(0, 1);
  graph.AddEdge(1, 0);

  util_graph::MergeGraph merge_graph;
  std::string error;
  Check(!merge_graph.Init(graph, &error), "MergeGraph::Init rejects cyclic input");
  std::cout << "Rejected init reason: " << error << '\n';
}

void TestScaleAcyclicInitAndTopoOrder() {
  constexpr util_graph::Node kNodes = 4096;
  util_graph::Graph graph = BuildForwardDag(kNodes);
  const std::size_t expected_edges = graph.EdgeCount();

  util_graph::GraphCycles checked;
  TimedCheck("scale DAG InitFrom accepts 4096-node graph", [&] {
    return checked.InitFrom(graph);
  });
  Check(checked.NodeLimit() == kNodes, "scale DAG node count preserved");
  Check(checked.EdgeCount() == expected_edges, "scale DAG edge count preserved");
  Check(checked.CheckInvariants(), "scale DAG rank invariants hold");

  bool has_cycle = true;
  std::vector<util_graph::Node> order =
      util_graph::TopologicalOrder(checked, &has_cycle);
  Check(!has_cycle, "scale DAG topological sort reports no cycle");
  Check(OrderRespectsEdges(checked, order),
        "scale DAG topological order respects every edge");
}

void TestScaleCycleInitRejectsBackEdge() {
  constexpr util_graph::Node kNodes = 4096;
  util_graph::Graph graph = BuildForwardDag(kNodes);
  graph.AddEdge(kNodes - 1, 0);

  util_graph::GraphCycles checked;
  TimedCheck("scale InitFrom rejects one back edge in 4096-node graph", [&] {
    return !checked.InitFrom(graph);
  });
  Check(checked.Empty(), "failed scale InitFrom leaves GraphCycles empty");
}

void TestScaleIncrementalAddsAndRejectsCycles() {
  constexpr util_graph::Node kNodes = 768;
  util_graph::GraphCycles graph;

  bool all_forward_edges_accepted = true;
  TimedCheck("scale incremental forward-edge insertion succeeds", [&] {
    for (util_graph::Node node = 0; node + 1 < kNodes; ++node) {
      all_forward_edges_accepted =
          graph.AddEdgeChecked(node, node + 1) && all_forward_edges_accepted;
    }
    for (util_graph::Node node = 0; node + 11 < kNodes; node += 2) {
      all_forward_edges_accepted =
          graph.AddEdgeChecked(node, node + 11) && all_forward_edges_accepted;
    }
    return all_forward_edges_accepted;
  });

  Check(graph.NodeLimit() == kNodes, "scale incremental node count reached");
  Check(graph.CheckInvariants(), "scale incremental rank invariants hold");
  Check(!graph.AddEdgeChecked(kNodes - 1, 0),
        "scale incremental rejects full-span back edge");
  Check(!graph.AddEdgeChecked(kNodes / 2, kNodes / 2 - 100),
        "scale incremental rejects mid-graph back edge");
  Check(!graph.HasEdge(kNodes - 1, 0),
        "scale rejected full-span back edge is not inserted");
  Check(graph.CheckInvariants(),
        "scale incremental graph remains invariant-safe after rejections");
}

void TestScalePartitionerMergesAndRejectsContractionCycle() {
  constexpr util_graph::Node kNodes = 512;
  util_graph::Graph graph;
  for (util_graph::Node node = 0; node + 1 < kNodes; ++node) {
    graph.AddEdge(node, node + 1);
  }

  util_graph::MergeGraph merge_graph;
  std::string error;
  Check(merge_graph.Init(graph, &error), "scale partitioner initializes chain DAG");

  bool all_pair_merges_accepted = true;
  TimedCheck("scale partitioner accepts adjacent pair contractions", [&] {
    for (util_graph::Node node = 0; node + 1 < kNodes; node += 2) {
      all_pair_merges_accepted =
          merge_graph.MergeClusters(node, node + 1, &error) &&
          all_pair_merges_accepted;
    }
    return all_pair_merges_accepted;
  });

  Check(merge_graph.Partitions().size() == kNodes / 2,
        "scale partitioner has expected cluster count after pair merges");
  Check(merge_graph.cluster_graph().CheckInvariants(),
        "scale partitioner cluster graph remains acyclic after pair merges");
  Check(!merge_graph.MergeClusters(0, kNodes - 1, &error),
        "scale partitioner rejects contraction that would cycle quotient graph");
  std::cout << "Scale rejected merge reason: " << error << '\n';
}

}  // namespace

int main() {
  TestAcyclicInit();
  TestCycleInitRejects();
  TestIncrementalCycleRejects();
  TestPartitionerInitAndMerge();
  TestPartitionerRejectsCyclicInput();
  TestScaleAcyclicInitAndTopoOrder();
  TestScaleCycleInitRejectsBackEdge();
  TestScaleIncrementalAddsAndRejectsCycles();
  TestScalePartitionerMergesAndRejectsContractionCycle();

  if (g_failures != 0) {
    std::cerr << g_failures << " test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "All graph cycle/partition tests passed\n";
  return EXIT_SUCCESS;
}
