#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace b4 {

constexpr double kEps = 1e-9;

static void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

struct CurvePoint { double share; double bandwidthGbps; };

// Monotone, piecewise-linear application policy bandwidth=f(fair_share).
class BandwidthFunction {
  public:
    explicit BandwidthFunction(std::vector<CurvePoint> points)
        : points_(std::move(points)) {
        require(points_.size() >= 2, "bandwidth function needs two points");
        require(std::abs(points_.front().share) <= kEps,
                "bandwidth function must start at fair share zero");
        for (std::size_t i = 0; i < points_.size(); ++i) {
            require(points_[i].share >= 0.0 && points_[i].bandwidthGbps >= 0.0,
                    "negative bandwidth-function coordinate");
            if (i) {
                require(points_[i].share > points_[i - 1].share,
                        "fair-share breakpoints must increase");
                require(points_[i].bandwidthGbps + kEps >=
                            points_[i - 1].bandwidthGbps,
                        "bandwidth function must be monotone");
            }
        }
    }

    double evaluate(double share) const {
        require(share >= -kEps, "negative fair share");
        for (std::size_t i = 1; i < points_.size(); ++i) {
            if (share <= points_[i].share + kEps) {
                const auto& a = points_[i - 1];
                const auto& b = points_[i];
                double x = std::max(a.share, share);
                double ratio = (x - a.share) / (b.share - a.share);
                return a.bandwidthGbps + ratio *
                    (b.bandwidthGbps - a.bandwidthGbps);
            }
        }
        return points_.back().bandwidthGbps;
    }

    double slopeAt(double share) const {
        for (std::size_t i = 1; i < points_.size(); ++i) {
            if (share < points_[i].share - kEps) {
                return (points_[i].bandwidthGbps -
                        points_[i - 1].bandwidthGbps) /
                       (points_[i].share - points_[i - 1].share);
            }
        }
        return 0.0;
    }

    double nextBreakpoint(double share) const {
        for (const auto& p : points_) {
            if (p.share > share + kEps) return p.share;
        }
        return std::numeric_limits<double>::infinity();
    }

    double minimumGbps() const { return points_.front().bandwidthGbps; }
    double maximumGbps() const { return points_.back().bandwidthGbps; }

  private:
    std::vector<CurvePoint> points_;
};

struct ApplicationDemand {
    std::string id;
    std::string application;
    std::string source;
    std::string destination;
    std::string qos;
    double demandGbps;
    int admissionPriority;
    BandwidthFunction policy;
};

struct FlowKey {
    std::string source, destination, qos;
    bool operator<(const FlowKey& o) const {
        return std::tie(source, destination, qos) <
               std::tie(o.source, o.destination, o.qos);
    }
};

static std::string nameOf(const FlowKey& k) {
    return k.source + "->" + k.destination + ":" + k.qos;
}

struct PhysicalLink { std::string id; double capacityGbps; bool up; };

struct Trunk {
    int id;
    std::string name, source, destination;
    std::vector<PhysicalLink> members;
    double headroomFraction;
    double residualGbps = 0.0;

    double rawCapacity() const {
        double n = 0.0;
        for (const auto& m : members) if (m.up) n += m.capacityGbps;
        return n;
    }
    double capacity() const { return rawCapacity() * (1.0-headroomFraction); }
};

struct Tunnel {
    std::string id;
    std::vector<int> trunks; // Sparse tunnel-to-resource membership.
    double cost;
};

// Southbound topology discovery feeds this bounded, versioned graph.
class DiscoveredTopology {
  public:
    int upsertTrunk(std::string name, std::string source,
                    std::string destination,
                    std::vector<PhysicalLink> members, double headroom) {
        require(!members.empty(), "trunk has no physical members");
        require(headroom >= 0.0 && headroom < 1.0, "invalid headroom");
        for (const auto& m : members)
            require(m.capacityGbps > 0.0, "non-positive member capacity");
        auto found = trunkByName_.find(name);
        if (found != trunkByName_.end()) {
            int id = found->second;
            trunks_[id] = {id, std::move(name), std::move(source),
                           std::move(destination), std::move(members),
                           headroom, 0.0};
            changed();
            return id;
        }
        int id = static_cast<int>(trunks_.size());
        trunkByName_[name] = id;
        trunks_.push_back({id, std::move(name), std::move(source),
                           std::move(destination), std::move(members),
                           headroom, 0.0});
        changed();
        return id;
    }

    void setMemberState(int trunk, const std::string& member, bool up) {
        validate(trunk);
        for (auto& m : trunks_[trunk].members) {
            if (m.id == member) {
                if (m.up != up) { m.up = up; changed(); }
                return;
            }
        }
        throw std::out_of_range("unknown physical member " + member);
    }

    const std::vector<Tunnel>& tunnels(const std::string& source,
                                       const std::string& destination,
                                       std::size_t k = 4,
                                       std::size_t maxHops = 6) {
        CacheKey key{source, destination, k, maxHops, version_};
        auto found = cache_.find(key);
        if (found != cache_.end()) return found->second;

        struct State {
            double cost; std::string node; std::vector<int> path;
            std::set<std::string> visited;
        };
        auto later = [](const State& a, const State& b) {
            if (std::abs(a.cost-b.cost) > kEps) return a.cost > b.cost;
            return a.path > b.path;
        };
        std::priority_queue<State,std::vector<State>,decltype(later)> q(later);
        q.push({0.0, source, {}, {source}});
        std::vector<Tunnel> out;
        std::size_t expansions = 0;
        const std::size_t expansionLimit = 100000;
        while (!q.empty() && out.size() < k) {
            State s = q.top(); q.pop();
            require(++expansions <= expansionLimit,
                    "candidate path search expansion limit exceeded");
            if (s.node == destination) {
                std::ostringstream id;
                for (std::size_t i=0;i<s.path.size();++i) {
                    if (i) id << '+';
                    id << trunks_[s.path[i]].name;
                }
                out.push_back({id.str(), std::move(s.path), s.cost});
                continue;
            }
            if (s.path.size() >= maxHops) continue;
            for (const auto& t : trunks_) {
                if (t.source != s.node || t.capacity() <= kEps ||
                    s.visited.count(t.destination)) continue;
                State n = s;
                n.node = t.destination;
                n.path.push_back(t.id);
                n.visited.insert(t.destination);
                n.cost += 1.0 + 1.0/std::max(1.0,t.capacity());
                q.push(std::move(n));
            }
        }
        return cache_.emplace(std::move(key),std::move(out)).first->second;
    }

    void resetResidual() { for (auto& t : trunks_) t.residualGbps=t.capacity(); }
    void consume(int id, double rate) {
        validate(id); require(rate >= -kEps, "negative resource consumption");
        trunks_[id].residualGbps -= rate;
        require(trunks_[id].residualGbps >= -1e-7, "trunk overloaded");
        trunks_[id].residualGbps=std::max(0.0,trunks_[id].residualGbps);
    }
    bool usable(const Tunnel& p) const {
        for (int id:p.trunks) if (trunks_.at(id).residualGbps<=kEps) return false;
        return true;
    }
    const Trunk& trunk(int id) const { validate(id); return trunks_[id]; }
    const std::vector<Trunk>& trunks() const { return trunks_; }
    std::uint64_t version() const { return version_; }

  private:
    struct CacheKey {
        std::string s,d; std::size_t k,h; std::uint64_t version;
        bool operator<(const CacheKey& o) const {
            return std::tie(s,d,k,h,version)<std::tie(o.s,o.d,o.k,o.h,o.version);
        }
    };
    void validate(int id) const {
        if (id<0 || id>=static_cast<int>(trunks_.size()))
            throw std::out_of_range("invalid trunk ID");
    }
    void changed() { ++version_; cache_.clear(); }
    std::vector<Trunk> trunks_;
    std::map<std::string,int> trunkByName_;
    std::map<CacheKey,std::vector<Tunnel>> cache_;
    std::uint64_t version_=0;
};

struct ApplicationSlice {
    ApplicationDemand demand;
    bool admitted = true;
};

struct FlowGroup {
    FlowKey key;
    std::vector<ApplicationSlice> applications;
    std::vector<Tunnel> tunnels;
    std::vector<double> tunnelRates;
    double fairShare=0.0;
    double allocatedGbps=0.0;
    bool blocked=false;

    double bandwidthAt(double share) const {
        double total=0.0;
        for (const auto& a:applications) if (a.admitted)
            total += std::min(a.demand.demandGbps,a.demand.policy.evaluate(share));
        return total;
    }
    double slopeAt(double share) const {
        double total=0.0;
        for (const auto& a:applications) if (a.admitted &&
            a.demand.policy.evaluate(share)<a.demand.demandGbps-kEps)
            total += a.demand.policy.slopeAt(share);
        return total;
    }
    double nextBreakpoint(double share) const {
        double next=std::numeric_limits<double>::infinity();
        for (const auto& a:applications) if (a.admitted &&
            a.demand.policy.evaluate(share)<a.demand.demandGbps-kEps)
            next=std::min(next,a.demand.policy.nextBreakpoint(share));
        return next;
    }
    double minimum() const {
        double total=0.0;
        for (const auto& a:applications) if (a.admitted)
            total += std::min(a.demand.demandGbps,a.demand.policy.minimumGbps());
        return total;
    }
};

struct AdmissionDecision { std::string demandId; bool admitted; std::string reason; };

class TrafficEngineer {
  public:
    explicit TrafficEngineer(DiscoveredTopology& topology):topology_(topology){}

    std::vector<AdmissionDecision> compute(
        const std::vector<ApplicationDemand>& demands) {
        aggregate(demands);
        std::vector<AdmissionDecision> decisions;
        // Unreachable applications are rejected before consuming resources.
        for (auto& [key,fg]:groups_) {
            const auto& paths=topology_.tunnels(key.source,key.destination);
            fg.tunnels=paths;
            fg.tunnelRates.assign(paths.size(),0.0);
            if (paths.empty()) for (auto& app:fg.applications) {
                app.admitted=false;
                decisions.push_back({app.demand.id,false,"no candidate tunnel"});
            }
        }

        // Iteratively remove the lowest-priority minimum guarantee that cannot
        // be honored, then recompute so rejected traffic cannot harm survivors.
        while (true) {
            allocate();
            FlowGroup* victimGroup=nullptr;
            ApplicationSlice* victim=nullptr;
            for (auto& [key,fg]:groups_) {
                (void)key;
                if (fg.allocatedGbps+kEps>=fg.minimum()) continue;
                for (auto& candidate:fg.applications) {
                    if (!candidate.admitted) continue;
                    // Larger numeric values are less important and are
                    // removed first when guarantees cannot all be honored.
                    if (!victim || candidate.demand.admissionPriority>
                                       victim->demand.admissionPriority) {
                        victim=&candidate; victimGroup=&fg;
                    }
                }
            }
            if (!victim) break;
            require(victimGroup && victim->admitted,
                    "admission loop cannot select a victim");
            victim->admitted=false;
            decisions.push_back({victim->demand.id,false,
                                 "minimum guarantee is infeasible"});
        }
        for (const auto& [key,fg]:groups_) {
            (void)key;
            for (const auto& app:fg.applications) if (app.admitted)
                decisions.push_back({app.demand.id,true,"admitted"});
        }
        std::sort(decisions.begin(),decisions.end(),[](const auto&a,const auto&b){
            return a.demandId<b.demandId;
        });
        return decisions;
    }

    const std::map<FlowKey,FlowGroup>& groups() const { return groups_; }

  private:
    void aggregate(const std::vector<ApplicationDemand>& demands) {
        groups_.clear(); std::set<std::string> ids;
        for (const auto& d:demands) {
            require(ids.insert(d.id).second,"duplicate demand ID");
            require(d.source!=d.destination,"local demand belongs outside WAN TE");
            require(d.demandGbps>=0.0,"negative demand");
            FlowKey key{d.source,d.destination,d.qos};
            auto& fg=groups_[key]; fg.key=key;
            fg.applications.push_back({d,true});
        }
    }

    void allocate() {
        topology_.resetResidual();
        for (auto& [key,fg]:groups_) {
            (void)key; fg.fairShare=0; fg.allocatedGbps=0; fg.blocked=false;
            fg.tunnelRates.assign(fg.tunnels.size(),0.0);
        }
        // Minimum values at fair share zero are real guarantees.
        for (auto& [key,fg]:groups_) {
            (void)key; double minimum=fg.minimum();
            if (minimum<=kEps) continue;
            double remaining=minimum;
            while (remaining>kEps) {
                int p=bestPath(fg);
                if (p<0) { fg.blocked=true; break; }
                double portion=std::min(remaining,pathResidual(fg.tunnels[p]));
                if (portion<=kEps) { fg.blocked=true; break; }
                add(fg,p,portion); fg.allocatedGbps+=portion;
                remaining-=portion;
            }
        }
        for (std::size_t iteration=0;iteration<100000;++iteration) {
            std::map<FlowGroup*,int> active;
            for (auto& [key,fg]:groups_) {
                (void)key;
                if (fg.slopeAt(fg.fairShare)<=kEps) continue;
                int p=bestPath(fg);
                if (p<0) fg.blocked=true; else active[&fg]=p;
            }
            if (active.empty()) return;
            double delta=std::numeric_limits<double>::infinity();
            std::vector<double> growth(topology_.trunks().size(),0.0);
            for (const auto& [fg,p]:active) {
                delta=std::min(delta,fg->nextBreakpoint(fg->fairShare)-fg->fairShare);
                double slope=fg->slopeAt(fg->fairShare);
                for (int t:fg->tunnels[p].trunks) growth[t]+=slope;
            }
            for (std::size_t t=0;t<growth.size();++t) if (growth[t]>kEps)
                delta=std::min(delta,topology_.trunk(static_cast<int>(t)).residualGbps/growth[t]);
            require(std::isfinite(delta) && delta>=-kEps,"invalid allocation event");
            if (delta<=kEps) {
                // Zero residual paths will become unusable in the next pass.
                for (const auto& [fg,p]:active) (void)fg,(void)p;
                bool any=false;
                for (double g:growth) if (g>kEps) any=true;
                require(any,"allocator made no progress");
                delta=0.0;
            }
            bool progress=false;
            for (const auto& [fg,p]:active) {
                double inc=fg->slopeAt(fg->fairShare)*delta;
                fg->fairShare+=delta;
                if (inc>kEps) { add(*fg,p,inc); fg->allocatedGbps+=inc; progress=true; }
            }
            if (!progress && delta<=kEps) {
                // Consume numerical dust to force selection of alternate paths.
                for (std::size_t t=0;t<growth.size();++t)
                    if (growth[t]>kEps && topology_.trunk(static_cast<int>(t)).residualGbps<=kEps) progress=true;
                require(progress,"zero-progress allocation loop");
            }
        }
        throw std::runtime_error("allocation iteration limit exceeded");
    }

    int bestPath(const FlowGroup& fg) const {
        int best=-1;
        for (std::size_t i=0;i<fg.tunnels.size();++i)
            if (topology_.usable(fg.tunnels[i]) &&
                (best<0 || fg.tunnels[i].cost<fg.tunnels[best].cost))
                best=static_cast<int>(i);
        return best;
    }
    double pathResidual(const Tunnel& p) const {
        double r=std::numeric_limits<double>::infinity();
        for (int t:p.trunks) r=std::min(r,topology_.trunk(t).residualGbps);
        return r;
    }
    void add(FlowGroup& fg,int path,double rate) {
        for (int t:fg.tunnels[path].trunks) topology_.consume(t,rate);
        fg.tunnelRates[path]+=rate;
    }

    DiscoveredTopology& topology_;
    std::map<FlowKey,FlowGroup> groups_;
};

struct QuantizedGroup {
    FlowKey key; double rateGbps; std::vector<Tunnel> tunnels;
    std::vector<int> buckets;
};

// Joint quantizer: every bucket placement is checked against all trunk
// capacities, then the completed plan is independently validated.
class GlobalQuantizer {
  public:
    explicit GlobalQuantizer(const DiscoveredTopology& t):topology_(t){}
    std::vector<QuantizedGroup> quantize(
        const std::map<FlowKey,FlowGroup>& groups,int bucketCount) const {
        require(bucketCount>0,"bucket count must be positive");
        std::vector<QuantizedGroup> result;
        std::vector<double> load(topology_.trunks().size(),0.0);
        for (const auto& [key,fg]:groups) {
            if (fg.allocatedGbps<=kEps) continue;
            QuantizedGroup q{key,fg.allocatedGbps,fg.tunnels,
                             std::vector<int>(fg.tunnels.size(),0)};
            result.push_back(std::move(q));
        }
        // Place the most capacity-sensitive groups first.
        std::sort(result.begin(),result.end(),[](const auto&a,const auto&b){
            return a.rateGbps>b.rateGbps;
        });
        for (auto& q:result) {
            for (int b=0;b<bucketCount;++b) {
                int best=-1; double bestPenalty=std::numeric_limits<double>::infinity();
                const auto& fg=groups.at(q.key);
                for (std::size_t p=0;p<q.tunnels.size();++p) {
                    double quantum=q.rateGbps/bucketCount;
                    bool feasible=true;
                    for (int t:q.tunnels[p].trunks)
                        if (load[t]+quantum>topology_.trunk(t).capacity()+1e-7)
                            feasible=false;
                    if (!feasible) continue;
                    double target=bucketCount*fg.tunnelRates[p]/q.rateGbps;
                    double before=q.buckets[p]-target;
                    double after=q.buckets[p]+1-target;
                    double penalty=after*after-before*before;
                    if (penalty<bestPenalty-kEps) { best=static_cast<int>(p);bestPenalty=penalty; }
                }
                require(best>=0,"no capacity-feasible global bucket placement");
                ++q.buckets[best];
                double quantum=q.rateGbps/bucketCount;
                for (int t:q.tunnels[best].trunks) load[t]+=quantum;
            }
        }
        for (std::size_t t=0;t<load.size();++t)
            require(load[t]<=topology_.trunk(static_cast<int>(t)).capacity()+1e-7,
                    "quantized plan overloads trunk");
        std::sort(result.begin(),result.end(),[](const auto&a,const auto&b){return a.key<b.key;});
        return result;
    }
  private: const DiscoveredTopology& topology_;
};

enum class ObjectKind { Tunnel, Group, Policer, Classifier, Activation };
struct Operation {
    ObjectKind kind; std::string device,id,payload;
};
struct PolicySnapshot {
    std::uint64_t version=0, topologyVersion=0;
    std::vector<QuantizedGroup> groups;
    std::vector<Operation> operations;
};

class PolicyStore {
  public:
    virtual ~PolicyStore()=default;
    virtual void saveDesired(const PolicySnapshot&)=0;
    virtual void saveStaged(const PolicySnapshot&)=0;
    virtual void activate(const PolicySnapshot&)=0;
    virtual void clearStaged()=0;
    virtual std::optional<PolicySnapshot> active() const=0;
};
class InMemoryPolicyStore final:public PolicyStore {
  public:
    void saveDesired(const PolicySnapshot&p)override{desired_=p;}
    void saveStaged(const PolicySnapshot&p)override{staged_=p;}
    void activate(const PolicySnapshot&p)override{active_=p;staged_.reset();}
    void clearStaged()override{staged_.reset();}
    std::optional<PolicySnapshot> active()const override{return active_;}
  private:
    std::optional<PolicySnapshot> desired_,staged_,active_;
};

struct Ack { bool success; bool retryable; std::string detail; };
class DeviceAdapter {
  public:
    virtual ~DeviceAdapter()=default;
    virtual Ack prepare(std::uint64_t,const Operation&)=0;
    virtual Ack commit(std::uint64_t,const std::string& device)=0;
    virtual void abort(std::uint64_t,const std::string& device) noexcept=0;
    virtual void rollback(const std::string& device,
                          std::uint64_t previousVersion) noexcept=0;
    virtual std::uint64_t activeVersion(const std::string& device)const=0;
};

// Deterministic test backend. A production OpenFlow/gNMI adapter implements
// the same idempotent contract and persists device acknowledgements.
class SimulatedDeviceAdapter final:public DeviceAdapter {
  public:
    void failNextPrepare(const std::string& device){failOnce_.insert(device);}
    void failNextCommit(const std::string& device){failCommitOnce_.insert(device);}
    Ack prepare(std::uint64_t v,const Operation&o)override{
        if (failOnce_.erase(o.device)) return {false,true,"transient failure"};
        prepared_[{o.device,v}].push_back(o); return {true,false,"prepared"};
    }
    Ack commit(std::uint64_t v,const std::string& d)override{
        if (failCommitOnce_.erase(d)) return {false,true,"transient commit failure"};
        if (!prepared_.count({d,v})) return {false,false,"nothing prepared"};
        active_[d]=v; prepared_.erase({d,v}); return {true,false,"committed"};
    }
    void abort(std::uint64_t v,const std::string&d)noexcept override{
        prepared_.erase({d,v});
    }
    void rollback(const std::string&d,std::uint64_t v)noexcept override {
        active_[d]=v;
    }
    std::uint64_t activeVersion(const std::string&d)const override{
        auto i=active_.find(d);return i==active_.end()?0:i->second;
    }
  private:
    std::set<std::string> failOnce_;
    std::set<std::string> failCommitOnce_;
    std::map<std::pair<std::string,std::uint64_t>,std::vector<Operation>> prepared_;
    std::map<std::string,std::uint64_t> active_;
};

class DeploymentCoordinator {
  public:
    DeploymentCoordinator(PolicyStore&s,DeviceAdapter&d,int retries=3)
        :store_(s),devices_(d),retries_(retries){require(retries_>=0,"negative retries");}

    void deploy(PolicySnapshot policy) {
        auto active=store_.active();
        const std::uint64_t previousVersion=active?active->version:0;
        policy.version=active?active->version+1:1;
        store_.saveDesired(policy); store_.saveStaged(policy);
        std::set<std::string> touched;
        try {
            // Operations are already sorted by dependency kind.
            for (const auto& op:policy.operations) {
                touched.insert(op.device); Ack ack{false,false,""};
                for (int n=0;n<=retries_;++n) {
                    ack=devices_.prepare(policy.version,op);
                    if (ack.success) break;
                    if (!ack.retryable) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1u<<n));
                }
                require(ack.success,"prepare failed for "+op.device+"/"+op.id+": "+ack.detail);
            }
            std::set<std::string> committed;
            for (const auto& d:touched) {
                Ack ack{false,false,""};
                for (int n=0;n<=retries_;++n) {
                    ack=devices_.commit(policy.version,d);
                    if (ack.success) break;
                    if (!ack.retryable) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1u<<n));
                }
                if (!ack.success) {
                    for (const auto& done:committed)
                        devices_.rollback(done,previousVersion);
                    throw std::runtime_error("commit failed for "+d+": "+ack.detail);
                }
                committed.insert(d);
            }
            store_.activate(policy);
        } catch (...) {
            for (const auto& d:touched) devices_.abort(policy.version,d);
            store_.clearStaged();
            throw;
        }
    }

    bool reconciled(const std::set<std::string>& devices) const {
        auto active=store_.active(); if (!active) return devices.empty();
        for (const auto& d:devices)
            if (devices_.activeVersion(d)!=active->version) return false;
        return true;
    }
  private: PolicyStore&store_;DeviceAdapter&devices_;int retries_;
};

static PolicySnapshot buildPolicy(const DiscoveredTopology& topology,
                                  const std::vector<QuantizedGroup>& groups) {
    PolicySnapshot p; p.topologyVersion=topology.version();p.groups=groups;
    std::set<std::pair<std::string,std::string>> tunnels;
    for (const auto& g:groups) {
        for (std::size_t i=0;i<g.tunnels.size();++i) if (g.buckets[i])
            tunnels.insert({g.key.source,g.tunnels[i].id});
    }
    for (const auto& [device,t]:tunnels)
        p.operations.push_back({ObjectKind::Tunnel,device,"tunnel/"+t,t});
    for (const auto& g:groups) {
        std::ostringstream split;
        for (std::size_t i=0;i<g.buckets.size();++i) if(g.buckets[i])
            split<<g.tunnels[i].id<<'='<<g.buckets[i]<<';';
        std::string n=nameOf(g.key);
        p.operations.push_back({ObjectKind::Group,g.key.source,"group/"+n,split.str()});
        p.operations.push_back({ObjectKind::Policer,g.key.source,"policer/"+n,
                                std::to_string(g.rateGbps)});
        p.operations.push_back({ObjectKind::Classifier,g.key.source,"classifier/"+n,n});
    }
    std::set<std::string> devices;
    for(const auto&o:p.operations)devices.insert(o.device);
    for(const auto&d:devices)p.operations.push_back({ObjectKind::Activation,d,"activate",""});
    std::stable_sort(p.operations.begin(),p.operations.end(),[](const auto&a,const auto&b){
        return static_cast<int>(a.kind)<static_cast<int>(b.kind);
    });
    return p;
}

static BandwidthFunction curve(double minimum,double preferred,double maximum) {
    return BandwidthFunction({{0.0,minimum},{1.0,preferred},{4.0,maximum}});
}

static void printPlan(const TrafficEngineer& te,const std::vector<AdmissionDecision>& a,
                      const PolicySnapshot& p) {
    std::cout<<"Admission decisions\n";
    for(const auto&d:a)std::cout<<"  "<<d.demandId<<": "
        <<(d.admitted?"admit":"reject")<<" ("<<d.reason<<")\n";
    std::cout<<std::fixed<<std::setprecision(3)<<"Flow groups\n";
    for(const auto&[key,fg]:te.groups()){
        std::cout<<"  "<<nameOf(key)<<" share="<<fg.fairShare
                 <<" rate="<<fg.allocatedGbps<<" Gbps\n";
        for(std::size_t i=0;i<fg.tunnels.size();++i)if(fg.tunnelRates[i]>kEps)
            std::cout<<"    "<<fg.tunnels[i].id<<" "<<fg.tunnelRates[i]<<" Gbps\n";
    }
    std::cout<<"Deployment objects: "<<p.operations.size()<<"\n";
}

static DiscoveredTopology demoTopology() {
    DiscoveredTopology t;
    auto members=[](const std::string&n){return std::vector<PhysicalLink>{
        {n+"-1",6.25,true},{n+"-2",6.25,true}};};
    t.upsertTrunk("A-B","A","B",members("AB"),0.20);
    t.upsertTrunk("A-C","A","C",members("AC"),0.20);
    t.upsertTrunk("C-B","C","B",members("CB"),0.20);
    t.upsertTrunk("A-D","A","D",members("AD"),0.20);
    t.upsertTrunk("D-C","D","C",members("DC"),0.20);
    return t;
}

static std::vector<ApplicationDemand> demoDemands() {
    return {
        {"backup","backup","A","B","bulk",15.0,10,curve(2,8,15)},
        {"index","index","A","B","bulk",8.0,20,curve(1,4,8)},
        {"sync","sync","A","C","bulk",10.0,10,curve(1,5,10)}};
}

static void selfTest() {
    {
        BandwidthFunction f({{0,1},{1,3},{3,5}});
        require(std::abs(f.evaluate(.5)-2)<kEps,"curve interpolation");
        require(std::abs(f.slopeAt(.5)-2)<kEps,"curve slope");
    }
    auto topology=demoTopology(); TrafficEngineer te(topology);
    auto decisions=te.compute(demoDemands());
    require(std::all_of(decisions.begin(),decisions.end(),
                        [](const auto&d){return d.admitted;}),"demo admission");
    for(const auto&[key,fg]:te.groups()){
        (void)key;require(fg.allocatedGbps+1e-7>=fg.minimum(),"minimum guarantee");
        double sum=0;for(double r:fg.tunnelRates)sum+=r;
        require(std::abs(sum-fg.allocatedGbps)<1e-7,"tunnel rate conservation");
    }
    GlobalQuantizer q(topology);auto quantized=q.quantize(te.groups(),16);
    auto policy=buildPolicy(topology,quantized);
    for(const auto&g:quantized){
        int buckets=0;for(int n:g.buckets)buckets+=n;
        require(buckets==16,"bucket conservation");
    }
    for(std::size_t i=1;i<policy.operations.size();++i)
        require(static_cast<int>(policy.operations[i-1].kind)<=
                static_cast<int>(policy.operations[i].kind),
                "deployment dependency order");
    InMemoryPolicyStore store;SimulatedDeviceAdapter device;
    device.failNextPrepare("A");DeploymentCoordinator deploy(store,device,3);
    deploy.deploy(policy);
    require(store.active()&&store.active()->version==1,"active TED version");
    require(deploy.reconciled({"A"}),"device reconciliation");
    // Unreachable demand must be rejected without disturbing admitted traffic.
    auto ds=demoDemands();ds.push_back({"bad","bad","X","Y","bulk",1,99,curve(0,1,1)});
    decisions=te.compute(ds);
    auto bad=std::find_if(decisions.begin(),decisions.end(),[](const auto&d){return d.demandId=="bad";});
    require(bad!=decisions.end()&&!bad->admitted,"unreachable admission rejection");
    // A minimum guarantee larger than the only trunk is rejected, while a
    // higher-priority feasible guarantee survives the recomputation.
    DiscoveredTopology narrow;
    narrow.upsertTrunk("P-Q","P","Q",{{"pq",2.0,true}},0.0);
    TrafficEngineer narrowTe(narrow);
    std::vector<ApplicationDemand> constrained{
        {"critical","critical","P","Q","bulk",1.0,1,curve(1,1,1)},
        {"optional","optional","P","Q","bulk",3.0,99,curve(3,3,3)}};
    auto constrainedDecisions=narrowTe.compute(constrained);
    auto critical=std::find_if(constrainedDecisions.begin(),constrainedDecisions.end(),
        [](const auto&d){return d.demandId=="critical";});
    auto optional=std::find_if(constrainedDecisions.begin(),constrainedDecisions.end(),
        [](const auto&d){return d.demandId=="optional";});
    require(critical!=constrainedDecisions.end()&&critical->admitted,
            "high-priority admission preservation");
    require(optional!=constrainedDecisions.end()&&!optional->admitted,
            "infeasible minimum rejection");
    std::cout<<"All self-tests passed\n";
}

} // namespace b4

int main(int argc,char**argv) {
    try {
        if(argc==2&&std::string(argv[1])=="--self-test"){b4::selfTest();return 0;}
        auto topology=b4::demoTopology();b4::TrafficEngineer te(topology);
        auto decisions=te.compute(b4::demoDemands());
        b4::GlobalQuantizer quantizer(topology);
        auto groups=quantizer.quantize(te.groups(),16);
        auto policy=b4::buildPolicy(topology,groups);
        b4::InMemoryPolicyStore store;b4::SimulatedDeviceAdapter devices;
        b4::DeploymentCoordinator coordinator(store,devices,3);
        coordinator.deploy(policy);b4::printPlan(te,decisions,*store.active());
        return 0;
    } catch(const std::exception&e) {
        std::cerr<<"fatal: "<<e.what()<<'\n';return 1;
    }
}
