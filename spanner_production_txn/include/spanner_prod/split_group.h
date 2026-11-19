#pragma once

#include "spanner_prod/lock_manager.h"
#include "spanner_prod/persistent_replica.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>

namespace spanner_prod {

struct GroupAcquireResult {
  Status status;
  AcquireResult acquire;
};

class SplitGroup {
 public:
  SplitGroup(SplitId split_id, std::filesystem::path root,
             std::size_t replicas = 3);

  Status Begin(const TransactionDescriptor& transaction);
  GroupAcquireResult Acquire(TransactionId transaction_id,
                             const LockRequest& request);
  Status BufferMutation(TransactionId transaction_id, Mutation mutation);
  StatusOr<Timestamp> Prepare(TransactionId transaction_id,
                              Timestamp lower_bound);
  Status Commit(TransactionId transaction_id, Timestamp commit_timestamp);
  Status Abort(TransactionId transaction_id, std::string reason);
  Status RecordDecision(const TransactionDecision& decision);
  Status AdvanceSafeTime(Timestamp timestamp);

  Status ElectLeader(std::size_t replica);
  Status SetReplicaLive(std::size_t replica, bool live);
  void FailAfterQuorumAppendOnce();
  Status RecoverPendingConsensus();

  [[nodiscard]] SplitId id() const { return consensus_.split_id(); }
  [[nodiscard]] Timestamp safe_time() const { return consensus_.safe_time(); }
  [[nodiscard]] Timestamp last_assigned_timestamp() const {
    return consensus_.last_assigned_timestamp();
  }
  [[nodiscard]] std::optional<TransactionDecision> Decision(
      TransactionId transaction_id) const {
    return consensus_.Decision(transaction_id);
  }
  [[nodiscard]] std::map<TransactionId, PreparedTransaction> Prepared() const {
    return consensus_.Prepared();
  }
  [[nodiscard]] StatusOr<std::optional<std::string>> ReadAt(
      const Cell& cell, Timestamp timestamp) const {
    return consensus_.ReadAt(cell, timestamp);
  }
  [[nodiscard]] std::string Fingerprint() const;

 private:
  struct ActiveTransaction {
    TransactionDescriptor descriptor;
    std::vector<Mutation> mutations;
  };

  std::uint64_t NextOperationId(TransactionId transaction_id);
  Status RestorePreparedLocks();

  mutable std::mutex mutex_;
  PersistentConsensusGroup consensus_;
  LockManager locks_;
  std::map<TransactionId, ActiveTransaction> active_;
  std::uint64_t next_sequence_{1};
  std::uint64_t next_operation_sequence_{1};
};

}  // namespace spanner_prod
