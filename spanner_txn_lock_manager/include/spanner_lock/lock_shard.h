#pragma once

#include "spanner_lock/lock_table.h"

#include <deque>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace spanner_lock {

struct BeginCommand {
  TransactionDescriptor transaction;
};

struct AcquireCommand {
  TransactionDescriptor transaction;
  LockRequest request;
  std::uint64_t enqueue_sequence{0};
};

struct PrepareCommand {
  TransactionId transaction_id{0};
};

struct CommitCommand {
  TransactionId transaction_id{0};
  CommitTimestamp commit_timestamp{0};
};

struct AbortCommand {
  TransactionId transaction_id{0};
  std::string reason;
};

struct TickCommand {
  LogicalTick tick{0};
};

using ShardCommand =
    std::variant<BeginCommand, AcquireCommand, PrepareCommand, CommitCommand,
                 AbortCommand, TickCommand>;

struct CommandEnvelope {
  CommandId command_id;
  ShardCommand command;
};

struct ShardResult {
  Status status;
  std::optional<AcquireResult> acquire;
  std::vector<GrantEvent> grants;
  std::vector<TransactionId> expired_transactions;
  CommitTimestamp commit_timestamp{0};

  explicit ShardResult(
      Status result_status = Status::Ok(),
      std::optional<AcquireResult> acquire_result = std::nullopt,
      std::vector<GrantEvent> grant_events = {},
      std::vector<TransactionId> expired = {},
      CommitTimestamp timestamp = 0)
      : status(std::move(result_status)),
        acquire(std::move(acquire_result)),
        grants(std::move(grant_events)),
        expired_transactions(std::move(expired)),
        commit_timestamp(timestamp) {}

  bool operator==(const ShardResult&) const = default;
};

struct ShardMetrics {
  std::uint64_t commands_applied{0};
  std::uint64_t duplicate_commands{0};
  std::uint64_t prepares{0};
  std::uint64_t commits{0};
  std::uint64_t aborts{0};

  bool operator==(const ShardMetrics&) const = default;
};

class LockShardStateMachine {
 public:
  explicit LockShardStateMachine(std::size_t deduplication_window = 10000);

  ShardResult Apply(LogIndex index, const CommandEnvelope& envelope);

  [[nodiscard]] LogIndex last_applied() const { return last_applied_; }
  [[nodiscard]] std::string Fingerprint() const;
  [[nodiscard]] const LockTable& lock_table() const { return lock_table_; }
  [[nodiscard]] const ShardMetrics& metrics() const { return metrics_; }
  [[nodiscard]] std::optional<TransactionState> TransactionStatus(
      TransactionId id) const;

 private:
  struct TransactionRecord {
    TransactionDescriptor descriptor;
    TransactionState state{TransactionState::kActive};
    CommitTimestamp commit_timestamp{0};
  };

  ShardResult ApplyNew(const BeginCommand& command);
  ShardResult ApplyNew(const AcquireCommand& command);
  ShardResult ApplyNew(const PrepareCommand& command);
  ShardResult ApplyNew(const CommitCommand& command);
  ShardResult ApplyNew(const AbortCommand& command);
  ShardResult ApplyNew(const TickCommand& command);
  void Remember(const CommandId& id, const ShardResult& result);

  LogIndex last_applied_{0};
  std::size_t deduplication_window_;
  std::map<TransactionId, TransactionRecord> transactions_;
  LockTable lock_table_;
  std::unordered_map<CommandId, ShardResult, CommandIdHash> results_;
  std::deque<CommandId> result_order_;
  ShardMetrics metrics_;
};

}  // namespace spanner_lock
