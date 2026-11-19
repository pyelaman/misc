#pragma once

#include "spanner_prod/mvcc_store.h"
#include "spanner_prod/types.h"

#include <map>
#include <set>
#include <string>

namespace spanner_prod {

struct ApplyResult {
  Status status;
  Timestamp timestamp{0};
  bool operator==(const ApplyResult&) const = default;
};

class SplitStateMachine {
 public:
  ApplyResult Apply(const LogEntry& entry);

  [[nodiscard]] LogIndex last_applied() const { return last_applied_; }
  [[nodiscard]] Timestamp safe_time() const;
  [[nodiscard]] Timestamp last_assigned_timestamp() const {
    return last_assigned_timestamp_;
  }
  [[nodiscard]] const std::map<TransactionId, PreparedTransaction>& prepared()
      const {
    return prepared_;
  }
  [[nodiscard]] std::optional<TransactionDecision> Decision(
      TransactionId transaction_id) const;
  [[nodiscard]] StatusOr<std::optional<std::string>> ReadAt(
      const Cell& cell, Timestamp timestamp) const;
  [[nodiscard]] std::string Fingerprint() const;

 private:
  ApplyResult ApplyCommand(const PrepareCommand& command);
  ApplyResult ApplyCommand(const CommitCommand& command);
  ApplyResult ApplyCommand(const AbortCommand& command);
  ApplyResult ApplyCommand(const RecordDecisionCommand& command);
  ApplyResult ApplyCommand(const AdvanceSafeTimeCommand& command);

  LogIndex last_applied_{0};
  Timestamp paxos_safe_time_{0};
  Timestamp last_assigned_timestamp_{0};
  std::map<TransactionId, PreparedTransaction> prepared_;
  std::map<TransactionId, TransactionDecision> decisions_;
  std::map<TransactionId, Timestamp> committed_;
  std::set<TransactionId> aborted_;
  std::map<std::uint64_t, ApplyResult> operation_results_;
  MvccStore store_;
};

}  // namespace spanner_prod

