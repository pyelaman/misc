#pragma once

#include "spanner_lock/replication.h"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace spanner_lock {

struct TimeInterval {
  CommitTimestamp earliest{0};
  CommitTimestamp latest{0};
};

// The production substitution point for TrueTime. Timestamps are expressed in
// microseconds in the supplied implementation.
class ICommitClock {
 public:
  virtual ~ICommitClock() = default;
  [[nodiscard]] virtual TimeInterval Now() const = 0;
  virtual void CommitWait(CommitTimestamp timestamp) const = 0;
};

class SystemUncertaintyClock final : public ICommitClock {
 public:
  explicit SystemUncertaintyClock(
      std::chrono::microseconds uncertainty = std::chrono::microseconds{0});
  [[nodiscard]] TimeInterval Now() const override;
  void CommitWait(CommitTimestamp timestamp) const override;

 private:
  std::chrono::microseconds uncertainty_;
};

enum class CoordinatorDecision { kCommit, kAbort };

struct DecisionRecord {
  TransactionId transaction_id{0};
  CoordinatorDecision decision{CoordinatorDecision::kAbort};
  CommitTimestamp commit_timestamp{0};
  std::vector<ShardId> participants;
  std::string reason;

  bool operator==(const DecisionRecord&) const = default;
};

// A real deployment must back this interface with a replicated/durable record.
// The in-memory implementation is deterministic and testable, but intentionally
// does not pretend that process memory is durable.
class IDecisionStore {
 public:
  virtual ~IDecisionStore() = default;
  virtual Status PutIfAbsent(const DecisionRecord& record) = 0;
  [[nodiscard]] virtual std::optional<DecisionRecord> Get(
      TransactionId transaction_id) const = 0;
};

class InMemoryDecisionStore final : public IDecisionStore {
 public:
  Status PutIfAbsent(const DecisionRecord& record) override;
  [[nodiscard]] std::optional<DecisionRecord> Get(
      TransactionId transaction_id) const override;

 private:
  mutable std::mutex mutex_;
  std::map<TransactionId, DecisionRecord> records_;
};

struct TransactionHandle {
  TransactionDescriptor descriptor;
};

struct CoordinatorAcquireResult {
  Status status;
  bool all_granted{false};
  std::map<ShardId, AcquireResult> shard_results;
  std::vector<TransactionId> wounded_transactions;
};

struct CommitResult {
  Status status;
  CommitTimestamp commit_timestamp{0};
};

// A compact transaction coordinator showing the important production
// boundaries: stable transaction age, deterministic sharding, idempotent shard
// commands, prepare, durable decision, commit/abort replay, and commit wait.
class TransactionCoordinator {
 public:
  TransactionCoordinator(std::vector<std::shared_ptr<IReplicatedShard>> shards,
                         std::shared_ptr<IDecisionStore> decision_store,
                         std::shared_ptr<ICommitClock> clock);

  TransactionHandle Begin(LogicalTick deadline_tick = 0,
                          std::string tag = {});
  CoordinatorAcquireResult Acquire(
      const TransactionHandle& transaction,
      const std::vector<LockRequest>& requests);
  CommitResult Commit(const TransactionHandle& transaction);
  Status Abort(const TransactionHandle& transaction, std::string reason);
  Status Recover(TransactionId transaction_id);
  Status AdvanceTick(LogicalTick tick);

  [[nodiscard]] ShardId ShardFor(const Cell& cell) const;

 private:
  struct CoordinatorRecord {
    TransactionDescriptor descriptor;
    TransactionState state{TransactionState::kActive};
    std::set<ShardId> participants;
    std::uint64_t next_command_sequence{1};
  };

  [[nodiscard]] CommandId NextCommandId(CoordinatorRecord& transaction);
  Status EnsureBegun(CoordinatorRecord& transaction, ShardId shard_id);
  Status AbortLocked(CoordinatorRecord& transaction, std::string reason);
  Status ReplayDecision(const DecisionRecord& decision,
                        CoordinatorRecord* transaction);
  [[nodiscard]] IReplicatedShard* FindShard(ShardId shard_id) const;

  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<IReplicatedShard>> shards_;
  std::map<ShardId, std::shared_ptr<IReplicatedShard>> shards_by_id_;
  std::shared_ptr<IDecisionStore> decision_store_;
  std::shared_ptr<ICommitClock> clock_;
  std::map<TransactionId, CoordinatorRecord> transactions_;
  TransactionId next_transaction_id_{1};
  TransactionAge next_transaction_age_{1};
  CommitTimestamp last_commit_timestamp_{0};
};

}  // namespace spanner_lock
