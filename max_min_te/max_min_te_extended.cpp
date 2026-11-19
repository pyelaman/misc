#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

constexpr double EPS = 1e-9;

struct LinkMember {
    std::string name;
    double capacityGbps;
    bool up = true;
};

struct Trunk {
    std::string name;
    std::string source;
    std::string destination;
    std::vector<LinkMember> members;
    double headroomFraction = 0.0;
    double residualGbps = 0.0;

    double rawCapacityGbps() const {
        double total = 0.0;
        for (const auto& member : members) {
            if (member.up) {
                total += member.capacityGbps;
            }
        }
        return total;
    }

    double usableCapacityGbps() const {
        return rawCapacityGbps() * (1.0 - headroomFraction);
    }
};

// Sparse tunnel-to-trunk membership: only trunk IDs used by the path are
// stored. A production implementation would use compact integer arrays or a
// CSR-like representation for the complete tunnel catalog.
struct Tunnel {
    std::string name;
    std::vector<int> trunks;
    double cost = 0.0;
};

class Topology {
  public:
    int addTrunk(std::string name,
                 std::string source,
                 std::string destination,
                 std::vector<LinkMember> members,
                 double headroomFraction) {
        if (members.empty()) {
            throw std::invalid_argument("trunk must contain a physical link");
        }
        if (headroomFraction < 0.0 || headroomFraction >= 1.0) {
            throw std::invalid_argument("headroom must be in [0, 1)");
        }
        for (const auto& member : members) {
            if (member.capacityGbps <= 0.0) {
                throw std::invalid_argument(
                    "physical-link capacity must be positive");
            }
        }

        int id = static_cast<int>(trunks_.size());
        trunks_.push_back({std::move(name),
                           std::move(source),
                           std::move(destination),
                           std::move(members),
                           headroomFraction,
                           0.0});
        invalidateTunnelCache();
        return id;
    }

    const std::vector<Tunnel>& candidateTunnels(
        const std::string& source,
        const std::string& destination,
        std::size_t maximumTunnels = 4,
        std::size_t maximumHops = 6) {

        auto key = std::make_pair(source, destination);
        auto found = tunnelCache_.find(key);
        if (found != tunnelCache_.end() &&
            found->second.topologyVersion == version_ &&
            found->second.maximumTunnels == maximumTunnels) {
            ++cacheHits_;
            return found->second.tunnels;
        }

        ++cacheMisses_;
        std::vector<Tunnel> candidates;
        std::vector<int> path;
        std::set<std::string> visited{source};
        enumeratePaths(source,
                       destination,
                       maximumHops,
                       path,
                       visited,
                       candidates);

        std::sort(candidates.begin(), candidates.end(),
                  [](const Tunnel& a, const Tunnel& b) {
                      if (std::abs(a.cost - b.cost) > EPS) {
                          return a.cost < b.cost;
                      }
                      return a.name < b.name;
                  });
        if (candidates.size() > maximumTunnels) {
            candidates.resize(maximumTunnels);
        }

        auto& entry = tunnelCache_[key];
        entry = {version_, maximumTunnels, std::move(candidates)};
        return entry.tunnels;
    }

    void setMemberState(int trunkId, std::size_t memberId, bool up) {
        validateTrunk(trunkId);
        auto& trunk = trunks_[trunkId];
        if (memberId >= trunk.members.size()) {
            throw std::out_of_range("invalid physical-link member ID");
        }
        if (trunk.members[memberId].up == up) {
            return;
        }
        trunk.members[memberId].up = up;
        invalidateTunnelCache();
    }

    void setHeadroom(int trunkId, double fraction) {
        validateTrunk(trunkId);
        if (fraction < 0.0 || fraction >= 1.0) {
            throw std::invalid_argument("headroom must be in [0, 1)");
        }
        trunks_[trunkId].headroomFraction = fraction;
        invalidateTunnelCache();
    }

    void resetResidualCapacity() {
        for (auto& trunk : trunks_) {
            trunk.residualGbps = trunk.usableCapacityGbps();
        }
    }

    void consume(int trunkId, double rateGbps) {
        validateTrunk(trunkId);
        trunks_[trunkId].residualGbps -= rateGbps;
        if (trunks_[trunkId].residualGbps < EPS) {
            trunks_[trunkId].residualGbps = 0.0;
        }
    }

    bool tunnelUsable(const Tunnel& tunnel) const {
        for (int trunkId : tunnel.trunks) {
            validateTrunk(trunkId);
            if (trunks_[trunkId].residualGbps <= EPS) {
                return false;
            }
        }
        return true;
    }

    const Trunk& trunk(int id) const {
        validateTrunk(id);
        return trunks_[id];
    }

    const std::vector<Trunk>& trunks() const { return trunks_; }
    std::size_t version() const { return version_; }
    std::size_t cacheHits() const { return cacheHits_; }
    std::size_t cacheMisses() const { return cacheMisses_; }

  private:
    struct CacheEntry {
        std::size_t topologyVersion = 0;
        std::size_t maximumTunnels = 0;
        std::vector<Tunnel> tunnels;
    };

    void validateTrunk(int id) const {
        if (id < 0 || id >= static_cast<int>(trunks_.size())) {
            throw std::out_of_range("invalid trunk ID");
        }
    }

    void invalidateTunnelCache() {
        ++version_;
        tunnelCache_.clear();
    }

    void enumeratePaths(const std::string& current,
                        const std::string& destination,
                        std::size_t hopsRemaining,
                        std::vector<int>& path,
                        std::set<std::string>& visited,
                        std::vector<Tunnel>& output) const {
        if (current == destination) {
            std::string name;
            double cost = 0.0;
            for (std::size_t i = 0; i < path.size(); ++i) {
                if (i != 0) {
                    name += "+";
                }
                name += trunks_[path[i]].name;
                // Hop count dominates; the inverse-capacity term prefers a
                // wider path when two paths have the same hop count.
                cost += 1.0 +
                        0.001 / std::max(EPS,
                                         trunks_[path[i]].usableCapacityGbps());
            }
            output.push_back({std::move(name), path, cost});
            return;
        }
        if (hopsRemaining == 0) {
            return;
        }

        for (std::size_t id = 0; id < trunks_.size(); ++id) {
            const auto& trunk = trunks_[id];
            if (trunk.source != current ||
                trunk.usableCapacityGbps() <= EPS ||
                visited.count(trunk.destination) != 0) {
                continue;
            }

            path.push_back(static_cast<int>(id));
            visited.insert(trunk.destination);
            enumeratePaths(trunk.destination,
                           destination,
                           hopsRemaining - 1,
                           path,
                           visited,
                           output);
            visited.erase(trunk.destination);
            path.pop_back();
        }
    }

    std::vector<Trunk> trunks_;
    std::map<std::pair<std::string, std::string>, CacheEntry> tunnelCache_;
    std::size_t version_ = 0;
    std::size_t cacheHits_ = 0;
    std::size_t cacheMisses_ = 0;
};

struct BandwidthSegment {
    // Cumulative upper rate for this segment.
    double upToGbps;
    double weight;
};

class BandwidthFunction {
  public:
    BandwidthFunction() = default;

    explicit BandwidthFunction(std::vector<BandwidthSegment> segments)
        : segments_(std::move(segments)) {
        double previous = 0.0;
        for (const auto& segment : segments_) {
            if (segment.upToGbps <= previous || segment.weight <= 0.0) {
                throw std::invalid_argument("invalid bandwidth function");
            }
            previous = segment.upToGbps;
        }
        if (segments_.empty()) {
            throw std::invalid_argument("empty bandwidth function");
        }
    }

    double weightAt(double allocatedGbps) const {
        return currentSegment(allocatedGbps).weight;
    }

    double nextBreakpoint(double allocatedGbps,
                          double demandGbps) const {
        return std::min(currentSegment(allocatedGbps).upToGbps,
                        demandGbps);
    }

  private:
    const BandwidthSegment& currentSegment(double allocatedGbps) const {
        for (const auto& segment : segments_) {
            if (allocatedGbps < segment.upToGbps - EPS) {
                return segment;
            }
        }
        return segments_.back();
    }

    std::vector<BandwidthSegment> segments_;
};

struct ConnectionDemand {
    std::string connectionId;
    std::string application;
    std::string source;
    std::string destination;
    std::string trafficClass;
    double demandGbps;
};

struct FlowKey {
    std::string application;
    std::string source;
    std::string destination;
    std::string trafficClass;

    bool operator<(const FlowKey& other) const {
        return std::tie(application, source, destination, trafficClass) <
               std::tie(other.application,
                        other.source,
                        other.destination,
                        other.trafficClass);
    }

    bool operator==(const FlowKey& other) const {
        return application == other.application &&
               source == other.source &&
               destination == other.destination &&
               trafficClass == other.trafficClass;
    }
};

std::string flowName(const FlowKey& key) {
    return key.application + ":" + key.source + "->" + key.destination +
           ":" + key.trafficClass;
}

struct FlowGroup {
    FlowKey key;
    double demandGbps = 0.0;
    BandwidthFunction bandwidthFunction;
    std::vector<Tunnel> tunnels;
    double allocatedGbps = 0.0;
    std::vector<double> tunnelRateGbps;
    bool blocked = false;
};

class TrafficEngineeringController {
  public:
    explicit TrafficEngineeringController(Topology& topology)
        : topology_(topology) {}

    void setApplicationProfile(std::string application,
                               BandwidthFunction function) {
        profiles_[std::move(application)] = std::move(function);
    }

    void addConnection(ConnectionDemand demand) {
        if (demand.demandGbps < 0.0) {
            throw std::invalid_argument("connection demand cannot be negative");
        }
        connections_[demand.connectionId] = std::move(demand);
    }

    void initialize() {
        synchronizeAggregates();
        refreshAllTunnels();
        std::set<std::size_t> all;
        for (std::size_t i = 0; i < flowGroups_.size(); ++i) {
            all.insert(i);
        }
        recomputeSet(all);
        rebuildDependencyIndex();
        lastAffectedCount_ = all.size();
    }

    void updateConnectionDemand(const std::string& connectionId,
                                double demandGbps) {
        auto found = connections_.find(connectionId);
        if (found == connections_.end()) {
            throw std::out_of_range("unknown connection demand");
        }
        if (demandGbps < 0.0) {
            throw std::invalid_argument("connection demand cannot be negative");
        }

        FlowKey changed{found->second.application,
                        found->second.source,
                        found->second.destination,
                        found->second.trafficClass};
        found->second.demandGbps = demandGbps;
        synchronizeAggregates();

        std::set<std::size_t> seeds;
        for (std::size_t i = 0; i < flowGroups_.size(); ++i) {
            if (flowGroups_[i].key == changed) {
                seeds.insert(i);
            }
        }
        // Demand changes do not invalidate the topology-versioned cache.
        // Refreshing only the changed pair demonstrates a cache hit and
        // avoids regenerating paths for unrelated site pairs.
        for (std::size_t id : seeds) {
            flowGroups_[id].tunnels = topology_.candidateTunnels(
                flowGroups_[id].key.source,
                flowGroups_[id].key.destination,
                4);
            flowGroups_[id].tunnelRateGbps.resize(
                flowGroups_[id].tunnels.size(), 0.0);
        }
        auto affected = resourceClosure(std::move(seeds));
        recomputeSet(affected);
        rebuildDependencyIndex();
        lastAffectedCount_ = affected.size();
    }

    void topologyChanged(const std::set<int>& changedTrunks) {
        std::vector<std::set<int>> oldMembership;
        std::vector<std::vector<std::vector<int>>> oldPaths;
        oldMembership.reserve(flowGroups_.size());
        oldPaths.reserve(flowGroups_.size());
        for (const auto& fg : flowGroups_) {
            oldMembership.push_back(candidateResources(fg));
            std::vector<std::vector<int>> paths;
            for (const auto& tunnel : fg.tunnels) {
                paths.push_back(tunnel.trunks);
            }
            oldPaths.push_back(std::move(paths));
        }

        refreshAllTunnels();
        std::set<std::size_t> seeds;
        for (std::size_t i = 0; i < flowGroups_.size(); ++i) {
            auto newMembership = candidateResources(flowGroups_[i]);
            std::vector<std::vector<int>> newPaths;
            for (const auto& tunnel : flowGroups_[i].tunnels) {
                newPaths.push_back(tunnel.trunks);
            }
            if (oldPaths[i] != newPaths) {
                seeds.insert(i);
            }
            for (int trunkId : changedTrunks) {
                if (oldMembership[i].count(trunkId) != 0 ||
                    newMembership.count(trunkId) != 0) {
                    seeds.insert(i);
                }
            }
        }

        auto affected = resourceClosure(std::move(seeds));
        recomputeSet(affected);
        rebuildDependencyIndex();
        lastAffectedCount_ = affected.size();
    }

    const std::vector<FlowGroup>& flowGroups() const {
        return flowGroups_;
    }

    std::size_t lastAffectedCount() const { return lastAffectedCount_; }

    void printResult() const {
        std::cout << std::fixed << std::setprecision(3);
        std::cout << "Flow groups (aggregated from " << connections_.size()
                  << " connections)\n";
        for (const auto& fg : flowGroups_) {
            std::cout << "  " << flowName(fg.key)
                      << " = " << fg.allocatedGbps
                      << "/" << fg.demandGbps << " Gbps";
            if (fg.blocked && fg.allocatedGbps + EPS < fg.demandGbps) {
                std::cout << " [blocked]";
            }
            std::cout << '\n';
            for (std::size_t t = 0; t < fg.tunnels.size(); ++t) {
                if (fg.tunnelRateGbps[t] > EPS) {
                    std::cout << "      " << fg.tunnels[t].name
                              << " = " << fg.tunnelRateGbps[t]
                              << " Gbps\n";
                }
            }
        }

        std::cout << "Trunk utilization (usable capacity excludes headroom)\n";
        for (const auto& trunk : topology_.trunks()) {
            double usable = trunk.usableCapacityGbps();
            double used = usable - trunk.residualGbps;
            double usableUtil = usable > EPS ? 100.0 * used / usable : 0.0;
            double rawUtil = trunk.rawCapacityGbps() > EPS
                                 ? 100.0 * used / trunk.rawCapacityGbps()
                                 : 0.0;
            std::cout << "  " << std::setw(4) << trunk.name
                      << ": " << used << "/" << usable
                      << " Gbps usable (" << usableUtil << "%), raw="
                      << rawUtil << "%, reserve="
                      << 100.0 * trunk.headroomFraction << "%\n";
        }
        std::cout << "Tunnel cache: hits=" << topology_.cacheHits()
                  << ", misses=" << topology_.cacheMisses() << "\n";
    }

  private:
    std::map<FlowKey, double> aggregateDemand() const {
        std::map<FlowKey, double> aggregated;
        for (const auto& [id, connection] : connections_) {
            (void)id;
            FlowKey key{connection.application,
                        connection.source,
                        connection.destination,
                        connection.trafficClass};
            aggregated[key] += connection.demandGbps;
        }
        return aggregated;
    }

    void synchronizeAggregates() {
        auto aggregated = aggregateDemand();
        std::map<FlowKey, FlowGroup> previous;
        for (auto& fg : flowGroups_) {
            previous.emplace(fg.key, std::move(fg));
        }

        flowGroups_.clear();
        for (const auto& [key, demand] : aggregated) {
            auto profile = profiles_.find(key.application);
            if (profile == profiles_.end()) {
                throw std::runtime_error("application has no bandwidth profile");
            }

            auto old = previous.find(key);
            if (old != previous.end()) {
                old->second.demandGbps = demand;
                flowGroups_.push_back(std::move(old->second));
            } else {
                FlowGroup fg;
                fg.key = key;
                fg.demandGbps = demand;
                fg.bandwidthFunction = profile->second;
                flowGroups_.push_back(std::move(fg));
            }
        }
    }

    void refreshAllTunnels() {
        for (auto& fg : flowGroups_) {
            fg.tunnels = topology_.candidateTunnels(
                fg.key.source, fg.key.destination, 4);
            fg.tunnelRateGbps.resize(fg.tunnels.size(), 0.0);
        }
    }

    static bool intersects(const std::set<int>& a,
                           const std::set<int>& b) {
        for (int value : a) {
            if (b.count(value) != 0) {
                return true;
            }
        }
        return false;
    }

    static std::set<int> candidateResources(const FlowGroup& fg) {
        std::set<int> resources;
        for (const auto& tunnel : fg.tunnels) {
            resources.insert(tunnel.trunks.begin(), tunnel.trunks.end());
        }
        return resources;
    }

    std::set<std::size_t> resourceClosure(
        std::set<std::size_t> affected) const {
        std::set<int> resources;
        for (std::size_t id : affected) {
            auto candidate = candidateResources(flowGroups_[id]);
            resources.insert(candidate.begin(), candidate.end());
        }

        bool changed;
        do {
            changed = false;
            for (std::size_t i = 0; i < flowGroups_.size(); ++i) {
                if (affected.count(i) != 0) {
                    continue;
                }
                auto candidate = candidateResources(flowGroups_[i]);
                if (intersects(resources, candidate)) {
                    affected.insert(i);
                    resources.insert(candidate.begin(), candidate.end());
                    changed = true;
                }
            }
        } while (changed);

        return affected;
    }

    void recomputeSet(const std::set<std::size_t>& affected) {
        topology_.resetResidualCapacity();

        // Preserve allocations for disconnected, unaffected resource
        // components and account for their existing trunk load.
        for (std::size_t i = 0; i < flowGroups_.size(); ++i) {
            auto& fg = flowGroups_[i];
            if (affected.count(i) != 0) {
                fg.allocatedGbps = 0.0;
                fg.tunnelRateGbps.assign(fg.tunnels.size(), 0.0);
                fg.blocked = false;
                continue;
            }
            for (std::size_t t = 0; t < fg.tunnels.size(); ++t) {
                for (int trunkId : fg.tunnels[t].trunks) {
                    topology_.consume(trunkId, fg.tunnelRateGbps[t]);
                }
            }
        }

        while (true) {
            std::map<std::size_t, int> selected;
            for (std::size_t id : affected) {
                auto& fg = flowGroups_[id];
                if (fg.demandGbps - fg.allocatedGbps <= EPS) {
                    continue;
                }

                int best = -1;
                for (std::size_t t = 0; t < fg.tunnels.size(); ++t) {
                    if (!topology_.tunnelUsable(fg.tunnels[t])) {
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
                    fg.blocked = true;
                } else {
                    selected[id] = best;
                }
            }
            if (selected.empty()) {
                break;
            }

            double delta = std::numeric_limits<double>::infinity();
            std::vector<double> trunkGrowth(topology_.trunks().size(), 0.0);

            for (const auto& [id, tunnelId] : selected) {
                const auto& fg = flowGroups_[id];
                double weight = fg.bandwidthFunction.weightAt(
                    fg.allocatedGbps);
                double endpoint = fg.bandwidthFunction.nextBreakpoint(
                    fg.allocatedGbps, fg.demandGbps);
                delta = std::min(
                    delta, (endpoint - fg.allocatedGbps) / weight);
                for (int trunkId : fg.tunnels[tunnelId].trunks) {
                    trunkGrowth[trunkId] += weight;
                }
            }

            for (std::size_t t = 0; t < trunkGrowth.size(); ++t) {
                if (trunkGrowth[t] > EPS) {
                    delta = std::min(
                        delta,
                        topology_.trunk(static_cast<int>(t)).residualGbps /
                            trunkGrowth[t]);
                }
            }
            if (!std::isfinite(delta) || delta < -EPS) {
                throw std::runtime_error("no feasible allocation event");
            }
            delta = std::max(0.0, delta);

            bool progress = false;
            for (const auto& [id, tunnelId] : selected) {
                auto& fg = flowGroups_[id];
                double weight = fg.bandwidthFunction.weightAt(
                    fg.allocatedGbps);
                double endpoint = fg.bandwidthFunction.nextBreakpoint(
                    fg.allocatedGbps, fg.demandGbps);
                double increment = std::min(weight * delta,
                                            endpoint - fg.allocatedGbps);
                if (increment > EPS) {
                    progress = true;
                }
                fg.allocatedGbps += increment;
                fg.tunnelRateGbps[tunnelId] += increment;
                for (int trunkId : fg.tunnels[tunnelId].trunks) {
                    topology_.consume(trunkId, increment);
                }
            }
            if (!progress && delta <= EPS) {
                throw std::runtime_error("allocator made no progress");
            }
        }
    }

    void rebuildDependencyIndex() {
        trunkToFlowGroups_.clear();
        for (std::size_t i = 0; i < flowGroups_.size(); ++i) {
            for (int trunkId : candidateResources(flowGroups_[i])) {
                trunkToFlowGroups_[trunkId].insert(i);
            }
        }
    }

    Topology& topology_;
    std::map<std::string, BandwidthFunction> profiles_;
    std::map<std::string, ConnectionDemand> connections_;
    std::vector<FlowGroup> flowGroups_;
    std::map<int, std::set<std::size_t>> trunkToFlowGroups_;
    std::size_t lastAffectedCount_ = 0;
};

std::vector<int> quantize(const FlowGroup& fg, int bucketCount) {
    std::vector<int> buckets(fg.tunnels.size(), 0);
    if (fg.allocatedGbps <= EPS) {
        return buckets;
    }

    std::vector<std::pair<double, int>> remainders;
    int assigned = 0;
    for (std::size_t i = 0; i < fg.tunnels.size(); ++i) {
        double exact = bucketCount * fg.tunnelRateGbps[i] /
                       fg.allocatedGbps;
        int whole = static_cast<int>(std::floor(exact));
        buckets[i] = whole;
        assigned += whole;
        remainders.push_back(
            {exact - whole, static_cast<int>(i)});
    }
    std::sort(remainders.begin(), remainders.end(),
              [](const auto& a, const auto& b) {
                  if (std::abs(a.first - b.first) > EPS) {
                      return a.first > b.first;
                  }
                  return a.second < b.second;
              });
    for (int i = 0; assigned < bucketCount; ++i, ++assigned) {
        ++buckets[remainders[i].second];
    }
    return buckets;
}

class TransactionalDataplane {
  public:
    void install(const std::vector<FlowGroup>& flowGroups,
                 int bucketCount) {
        std::size_t candidateVersion = activeVersion_ + 1;
        std::cout << "Staging dataplane transaction v"
                  << candidateVersion << "\n";

        for (const auto& fg : flowGroups) {
            auto buckets = quantize(fg, bucketCount);
            int total = 0;
            for (int value : buckets) {
                total += value;
            }
            if (fg.allocatedGbps > EPS && total != bucketCount) {
                throw std::runtime_error("invalid ECMP bucket plan");
            }

            std::cout << "  group " << flowName(fg.key) << ":";
            for (std::size_t t = 0; t < buckets.size(); ++t) {
                if (buckets[t] > 0) {
                    std::cout << " [" << fg.tunnels[t].name
                              << "=" << buckets[t] << "]";
                }
            }
            std::cout << "\n";

            // This models an aggregate token-bucket/policer installed at the
            // source-site edge, rather than one policer per connection.
            double burstGbit = std::max(0.1, fg.allocatedGbps * 0.10);
            std::cout << "  source policer " << fg.key.source
                      << "/" << flowName(fg.key)
                      << ": rate=" << fg.allocatedGbps
                      << " Gbps, burst=" << burstGbit << " Gbit\n";
        }

        // All validation and staging completed. A real controller would now
        // atomically switch an ingress classifier to candidateVersion.
        activeVersion_ = candidateVersion;
        std::cout << "Committed dataplane transaction v"
                  << activeVersion_ << "\n";
    }

  private:
    std::size_t activeVersion_ = 0;
};

}  // namespace

int main() {
    Topology topology;

    auto members = [](const std::string& prefix) {
        // Two physical 6.25-Gbps links aggregate into a 12.5-Gbps trunk.
        return std::vector<LinkMember>{{prefix + "-1", 6.25, true},
                                       {prefix + "-2", 6.25, true}};
    };

    // Twenty percent is reserved for failures and bursts, leaving 10 Gbps
    // usable on each 12.5-Gbps physical trunk.
    int AB = topology.addTrunk("A-B", "A", "B", members("AB"), 0.20);
    int AC = topology.addTrunk("A-C", "A", "C", members("AC"), 0.20);
    int CB = topology.addTrunk("C-B", "C", "B", members("CB"), 0.20);
    int AD = topology.addTrunk("A-D", "A", "D", members("AD"), 0.20);
    int DC = topology.addTrunk("D-C", "D", "C", members("DC"), 0.20);
    int EF = topology.addTrunk("E-F", "E", "F", members("EF"), 0.20);
    (void)AC;
    (void)CB;
    (void)AD;
    (void)DC;
    (void)EF;

    TrafficEngineeringController controller(topology);
    controller.setApplicationProfile(
        "backup",
        BandwidthFunction({{5.0, 4.0},
                           {std::numeric_limits<double>::infinity(), 2.0}}));
    controller.setApplicationProfile(
        "sync",
        BandwidthFunction({{3.0, 2.0},
                           {std::numeric_limits<double>::infinity(), 1.0}}));
    controller.setApplicationProfile(
        "analytics",
        BandwidthFunction({
            {std::numeric_limits<double>::infinity(), 1.0}}));

    // Six individual connections become three aggregate flow groups.
    controller.addConnection(
        {"backup-shard-1", "backup", "A", "B", "bulk", 12.0});
    controller.addConnection(
        {"backup-shard-2", "backup", "A", "B", "bulk", 8.0});
    controller.addConnection(
        {"sync-table-1", "sync", "A", "C", "bulk", 4.0});
    controller.addConnection(
        {"sync-table-2", "sync", "A", "C", "bulk", 6.0});
    controller.addConnection(
        {"analytics-1", "analytics", "E", "F", "batch", 1.5});
    controller.addConnection(
        {"analytics-2", "analytics", "E", "F", "batch", 2.5});

    TransactionalDataplane dataplane;

    std::cout << "=== Initial computation ===\n";
    controller.initialize();
    controller.printResult();
    std::cout << "Recomputed FGs: " << controller.lastAffectedCount()
              << "\n";
    dataplane.install(controller.flowGroups(), 16);

    std::cout << "\n=== Demand update: backup-shard-2 8 -> 6 Gbps ===\n";
    controller.updateConnectionDemand("backup-shard-2", 6.0);
    controller.printResult();
    std::cout << "Recomputed FGs: " << controller.lastAffectedCount()
              << " (the independent E->F FG was preserved)\n";
    dataplane.install(controller.flowGroups(), 16);

    std::cout << "\n=== Topology update: one A-B member fails ===\n";
    topology.setMemberState(AB, 0, false);
    controller.topologyChanged({AB});
    controller.printResult();
    std::cout << "Recomputed FGs: " << controller.lastAffectedCount()
              << "\n";
    dataplane.install(controller.flowGroups(), 16);
}
