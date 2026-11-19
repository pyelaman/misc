#pragma once

#include "spanner_lock/lock_shard.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace spanner_lock {

class IReplicatedShard {
 public:
  virtual ~IReplicatedShard() = default;
  virtual ShardResult Propose(const CommandEnvelope& envelope) = 0;
  [[nodiscard]] virtual ShardId id() const = 0;
  [[nodiscard]] virtual std::string Fingerprint() const = 0;
};

// A deterministic consensus harness, not a Raft/Paxos implementation. It
// provides the same state-machine boundary a production consensus library
// needs: quorum admission, ordered log indexes, deterministic apply, follower
// catch-up, and fail-stop replica injection.
class InMemoryReplicatedShard final : public IReplicatedShard {
 public:
  explicit InMemoryReplicatedShard(ShardId id, std::size_t replica_count = 3);

  ShardResult Propose(const CommandEnvelope& envelope) override;
  [[nodiscard]] ShardId id() const override { return id_; }
  [[nodiscard]] std::string Fingerprint() const override;

  Status SetReplicaLive(std::size_t replica, bool live);
  void FailNextProposal();
  [[nodiscard]] std::size_t live_replicas() const;
  [[nodiscard]] std::size_t quorum_size() const;
  [[nodiscard]] std::size_t log_size() const;

 private:
  struct LogEntry {
    LogIndex index;
    CommandEnvelope envelope;
  };

  void CatchUp(std::size_t replica);

  ShardId id_;
  mutable std::mutex mutex_;
  std::vector<LockShardStateMachine> replicas_;
  std::vector<bool> live_;
  std::vector<LogEntry> log_;
  bool fail_next_proposal_{false};
};

}  // namespace spanner_lock

