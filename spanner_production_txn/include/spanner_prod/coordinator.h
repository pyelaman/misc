#pragma once

#include "spanner_prod/range_directory.h"
#include "spanner_prod/true_time.h"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace spanner_prod {

struct TransactionHandle {
  TransactionId id{0};
};

struct RecoveryToken {
  TransactionId transaction_id{0};
  SplitId coordinator_split{0};
};

struct TransactionAcquireResult {
  Status status;
  bool all_granted{false};
  std::vector<TransactionId> wounded;
};

struct TransactionCommitResult {
  Status status;
  Timestamp commit_timestamp{0};
};

class TransactionCoordinator {
 public:
  TransactionCoordinator(std::shared_ptr<RangeDirectory> directory,
                         std::shared_ptr<ITrueTime> true_time);

  TransactionHandle Begin(Timestamp deadline = 0, std::string tag = {});
  TransactionAcquireResult Acquire(
      TransactionHandle transaction,
      const std::vector<LockRequest>& requests);
  Status BufferWrite(TransactionHandle transaction, Mutation mutation);
  TransactionCommitResult Commit(
      TransactionHandle transaction,
      std::chrono::milliseconds commit_wait_timeout =
          std::chrono::seconds(5));
  Status Abort(TransactionHandle transaction, std::string reason);
  [[nodiscard]] StatusOr<RecoveryToken> GetRecoveryToken(
      TransactionHandle transaction) const;
  Status Recover(TransactionHandle transaction,
                 std::chrono::milliseconds commit_wait_timeout =
                     std::chrono::seconds(5));
  Status Recover(RecoveryToken token,
                 std::chrono::milliseconds commit_wait_timeout =
                     std::chrono::seconds(5));

  [[nodiscard]] StatusOr<std::optional<std::string>> SnapshotRead(
      const Cell& cell, Timestamp timestamp) const;
  [[nodiscard]] StatusOr<std::optional<std::string>> StrongRead(
      const Cell& cell) const;

 private:
  enum class State { kActive, kPreparing, kPrepared, kCommitting, kCommitted, kAborted };
  struct Record {
    TransactionDescriptor descriptor;
    State state{State::kActive};
    std::map<SplitId, std::shared_ptr<SplitGroup>> participants;
  };

  StatusOr<std::shared_ptr<SplitGroup>> EnsureParticipant(
      Record& transaction, const Cell& cell);
  Status AbortLocked(Record& transaction, std::string reason);
  Status ReplayDecisionLocked(Record& transaction,
                              const TransactionDecision& decision,
                              std::chrono::milliseconds commit_wait_timeout);

  std::shared_ptr<RangeDirectory> directory_;
  std::shared_ptr<ITrueTime> true_time_;
  mutable std::mutex mutex_;
  std::map<TransactionId, Record> transactions_;
  TransactionId next_transaction_id_{1};
  TransactionPriority next_priority_{1};
};

}  // namespace spanner_prod
