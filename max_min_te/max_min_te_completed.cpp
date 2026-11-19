#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr double EPS = 1e-9;

struct Edge {
    std::string name;
    double capacityGbps = 0.0;
    double residualGbps = 0.0;
    bool saturated = false;
};

struct Tunnel {
    std::string name;
    std::vector<int> edges;
    double cost = 0.0;
};

struct FlowGroup {
    std::string name;
    std::string source;
    std::string destination;
    std::string trafficClass;
    double demandGbps = 0.0;
    double weight = 1.0;
    std::vector<Tunnel> tunnels;

    double allocatedGbps = 0.0;
    std::vector<double> tunnelRateGbps;
    bool blocked = false;

    FlowGroup(std::string nameValue,
              std::string sourceValue,
              std::string destinationValue,
              std::string trafficClassValue,
              double demandValue,
              double weightValue,
              std::vector<Tunnel> tunnelValues)
        : name(std::move(nameValue)),
          source(std::move(sourceValue)),
          destination(std::move(destinationValue)),
          trafficClass(std::move(trafficClassValue)),
          demandGbps(demandValue),
          weight(weightValue),
          tunnels(std::move(tunnelValues)) {}
};

class Allocator {
  public:
    int addEdge(std::string name, double capacityGbps) {
        if (capacityGbps <= 0.0) {
            throw std::invalid_argument("edge capacity must be positive");
        }

        int id = static_cast<int>(edges_.size());
        edges_.push_back(
            {std::move(name), capacityGbps, capacityGbps, false});
        return id;
    }

    void addFlowGroup(FlowGroup fg) {
        if (fg.demandGbps < 0.0) {
            throw std::invalid_argument("flow-group demand cannot be negative");
        }
        if (fg.weight <= 0.0) {
            throw std::invalid_argument("flow-group weight must be positive");
        }
        if (fg.tunnels.empty() && fg.demandGbps > EPS) {
            throw std::invalid_argument("flow group has no candidate tunnels");
        }

        for (const auto& tunnel : fg.tunnels) {
            if (tunnel.edges.empty()) {
                throw std::invalid_argument("candidate tunnel has no edges");
            }
            if (tunnel.cost < 0.0) {
                throw std::invalid_argument("tunnel cost cannot be negative");
            }

            std::unordered_set<int> seen;
            for (int edgeId : tunnel.edges) {
                if (edgeId < 0 ||
                    edgeId >= static_cast<int>(edges_.size())) {
                    throw std::out_of_range("tunnel references invalid edge");
                }
                if (!seen.insert(edgeId).second) {
                    throw std::invalid_argument(
                        "tunnel contains the same edge more than once");
                }
            }
        }

        fg.allocatedGbps = 0.0;
        fg.tunnelRateGbps.assign(fg.tunnels.size(), 0.0);
        fg.blocked = false;
        flowGroups_.push_back(std::move(fg));
    }

    void allocate() {
        reset();

        // One iteration ends at a demand-completion event or at least one
        // link-saturation event. It therefore does not depend on a small
        // artificial bandwidth quantum.
        while (true) {
            std::vector<int> selectedTunnel(flowGroups_.size(), -1);
            bool haveActiveFlow = false;

            for (std::size_t f = 0; f < flowGroups_.size(); ++f) {
                auto& fg = flowGroups_[f];
                if (fg.demandGbps - fg.allocatedGbps <= EPS) {
                    continue;
                }

                int best = -1;
                for (std::size_t t = 0; t < fg.tunnels.size(); ++t) {
                    if (!tunnelUsable(fg.tunnels[t])) {
                        continue;
                    }

                    if (best < 0 ||
                        fg.tunnels[t].cost < fg.tunnels[best].cost - EPS ||
                        (std::abs(fg.tunnels[t].cost -
                                  fg.tunnels[best].cost) <= EPS &&
                         fg.tunnels[t].name < fg.tunnels[best].name)) {
                        best = static_cast<int>(t);
                    }
                }

                if (best < 0) {
                    // Residual capacities only decrease, so this flow cannot
                    // become feasible in a later iteration.
                    fg.blocked = true;
                    continue;
                }

                selectedTunnel[f] = best;
                haveActiveFlow = true;
            }

            if (!haveActiveFlow) {
                break;
            }

            // normalizedDelta advances every active FG's fair-share level.
            // FG f gains weight[f] * normalizedDelta Gbps.
            double normalizedDelta =
                std::numeric_limits<double>::infinity();

            // The next event may be an FG reaching its demand.
            for (std::size_t f = 0; f < flowGroups_.size(); ++f) {
                if (selectedTunnel[f] < 0) {
                    continue;
                }

                const auto& fg = flowGroups_[f];
                normalizedDelta = std::min(
                    normalizedDelta,
                    (fg.demandGbps - fg.allocatedGbps) / fg.weight);
            }

            // Or it may be a physical edge becoming saturated. edgeGrowth[e]
            // is the capacity consumed on e per unit of normalizedDelta.
            std::vector<double> edgeGrowth(edges_.size(), 0.0);
            for (std::size_t f = 0; f < flowGroups_.size(); ++f) {
                if (selectedTunnel[f] < 0) {
                    continue;
                }

                const auto& fg = flowGroups_[f];
                const auto& tunnel = fg.tunnels[selectedTunnel[f]];
                for (int edgeId : tunnel.edges) {
                    edgeGrowth[edgeId] += fg.weight;
                }
            }

            for (std::size_t e = 0; e < edges_.size(); ++e) {
                if (edgeGrowth[e] > EPS) {
                    normalizedDelta = std::min(
                        normalizedDelta,
                        edges_[e].residualGbps / edgeGrowth[e]);
                }
            }

            if (!std::isfinite(normalizedDelta) ||
                normalizedDelta < -EPS) {
                throw std::runtime_error(
                    "allocator could not find a feasible progress event");
            }
            normalizedDelta = std::max(0.0, normalizedDelta);

            // Commit this event's allocation to FGs, tunnels, and edges.
            for (std::size_t f = 0; f < flowGroups_.size(); ++f) {
                int tunnelId = selectedTunnel[f];
                if (tunnelId < 0) {
                    continue;
                }

                auto& fg = flowGroups_[f];
                double increment = std::min(
                    fg.weight * normalizedDelta,
                    fg.demandGbps - fg.allocatedGbps);

                fg.allocatedGbps += increment;
                fg.tunnelRateGbps[tunnelId] += increment;

                for (int edgeId : fg.tunnels[tunnelId].edges) {
                    edges_[edgeId].residualGbps -= increment;
                    if (edges_[edgeId].residualGbps < EPS) {
                        edges_[edgeId].residualGbps = 0.0;
                    }
                }
            }

            bool eventObserved = false;
            for (auto& edge : edges_) {
                if (!edge.saturated && edge.residualGbps <= EPS) {
                    edge.saturated = true;
                    eventObserved = true;
                }
            }
            for (const auto& fg : flowGroups_) {
                if (fg.demandGbps - fg.allocatedGbps <= EPS) {
                    eventObserved = true;
                }
            }

            if (!eventObserved && normalizedDelta <= EPS) {
                throw std::runtime_error("allocator made no progress");
            }
        }
    }

    void printResult(int bucketCount) const {
        if (bucketCount <= 0) {
            throw std::invalid_argument("bucket count must be positive");
        }

        std::cout << std::fixed << std::setprecision(3);
        std::cout << "Flow-group and tunnel allocations\n";

        for (const auto& fg : flowGroups_) {
            std::cout << "  " << fg.name
                      << " (" << fg.source << "->" << fg.destination
                      << ", class=" << fg.trafficClass
                      << ", weight=" << fg.weight << ")\n"
                      << "    allocated: " << fg.allocatedGbps
                      << "/" << fg.demandGbps << " Gbps";
            if (fg.blocked && fg.allocatedGbps + EPS < fg.demandGbps) {
                std::cout << " [blocked]";
            }
            std::cout << '\n';

            auto buckets = quantize(fg, bucketCount);
            for (std::size_t t = 0; t < fg.tunnels.size(); ++t) {
                if (fg.tunnelRateGbps[t] <= EPS) {
                    continue;
                }

                double fraction =
                    fg.tunnelRateGbps[t] / fg.allocatedGbps;
                std::cout << "      " << std::setw(10)
                          << fg.tunnels[t].name
                          << ": " << std::setw(7)
                          << fg.tunnelRateGbps[t] << " Gbps"
                          << ", fraction=" << std::setw(6)
                          << fraction
                          << ", buckets=" << buckets[t]
                          << "/" << bucketCount << '\n';
            }
        }

        std::cout << "\nLink utilization\n";
        for (const auto& edge : edges_) {
            double used = edge.capacityGbps - edge.residualGbps;
            double utilization = 100.0 * used / edge.capacityGbps;

            std::cout << "  " << std::setw(5) << edge.name
                      << ": " << std::setw(7) << used
                      << "/" << edge.capacityGbps << " Gbps"
                      << " (" << std::fixed << std::setprecision(1)
                      << utilization << "%)\n"
                      << std::setprecision(3);
        }
    }

  private:
    void reset() {
        for (auto& edge : edges_) {
            edge.residualGbps = edge.capacityGbps;
            edge.saturated = false;
        }

        for (auto& fg : flowGroups_) {
            fg.allocatedGbps = 0.0;
            fg.tunnelRateGbps.assign(fg.tunnels.size(), 0.0);
            fg.blocked = false;
        }
    }

    bool tunnelUsable(const Tunnel& tunnel) const {
        for (int edgeId : tunnel.edges) {
            const auto& edge = edges_[edgeId];
            if (edge.saturated || edge.residualGbps <= EPS) {
                return false;
            }
        }
        return true;
    }

    static std::vector<int> quantize(
        const FlowGroup& fg,
        int bucketCount) {

        std::vector<int> buckets(fg.tunnels.size(), 0);
        if (fg.allocatedGbps <= EPS) {
            return buckets;
        }

        std::vector<std::pair<double, int>> remainders;
        int assigned = 0;

        for (std::size_t i = 0; i < fg.tunnels.size(); ++i) {
            double exact =
                bucketCount *
                fg.tunnelRateGbps[i] /
                fg.allocatedGbps;

            int whole = static_cast<int>(std::floor(exact));
            buckets[i] = whole;
            assigned += whole;
            remainders.push_back(
                {exact - whole, static_cast<int>(i)});
        }

        std::sort(
            remainders.begin(),
            remainders.end(),
            [](const auto& a, const auto& b) {
                if (std::abs(a.first - b.first) > EPS) {
                    return a.first > b.first;
                }
                return a.second < b.second;
            });

        for (int i = 0;
             assigned < bucketCount &&
             i < static_cast<int>(remainders.size());
             ++i, ++assigned) {
            ++buckets[remainders[i].second];
        }

        return buckets;
    }

    std::vector<Edge> edges_;
    std::vector<FlowGroup> flowGroups_;
};

}  // namespace

int main() {
    Allocator allocator;

    // Five directed 10-Gbps inter-site links.
    int AB = allocator.addEdge("A-B", 10.0);
    int AC = allocator.addEdge("A-C", 10.0);
    int CB = allocator.addEdge("C-B", 10.0);
    int AD = allocator.addEdge("A-D", 10.0);
    int DC = allocator.addEdge("D-C", 10.0);

    // Higher-priority bulk replication from A to B.
    allocator.addFlowGroup({
        "FG-backup",
        "A", "B", "bulk",
        20.0, 2.0,
        {
            {"A-B",     {AB},         1.0},
            {"A-C-B",   {AC, CB},     2.0},
            {"A-D-C-B", {AD, DC, CB}, 12.0}
        }
    });

    // Lower-priority replication from A to C. Its direct tunnel initially
    // competes with A-C-B; after A-C saturates, both FGs can use A-D-C.
    allocator.addFlowGroup({
        "FG-sync",
        "A", "C", "bulk",
        10.0, 1.0,
        {
            {"A-C",   {AC},     1.0},
            {"A-D-C", {AD, DC}, 2.0}
        }
    });

    allocator.allocate();
    allocator.printResult(16);
}
