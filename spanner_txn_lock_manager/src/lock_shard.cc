#include "spanner_lock/lock_shard.h"

#include <algorithm>
#include <sstream>
#include <type_traits>

namespace spanner_lock {

LockShardStateMachine::LockShardStateMachine(
    std::size_t deduplication_window)
    : deduplication_window_(deduplication_window) {}

ShardResult LockShardStateMachine::Apply(LogIndex index,
                                         const CommandEnvelope& envelope) {
  if (index != last_applied_ + 1U) {
    return ShardResult{Status::Error(
        StatusCode::kFailedPrecondition,
        "log index must be exactly one greater than last_applied")};
  }
  last_applied_ = index;
  ++metrics_.commands_applied;

  if (const auto duplicate = results_.find(envelope.command_id);
      duplicate != results_.end()) {
    ++metrics_.duplicate_commands;
    return duplicate->second;
  }

  ShardResult result = std::visit(
      [this](const auto& command) { return ApplyNew(command); },
      envelope.command);
  Remember(envelope.command_id, result);
  return result;
}

ShardResult LockShardStateMachine::ApplyNew(const BeginCommand& command) {
  if (command.transaction.id == 0 || command.transaction.age == 0) {
    return ShardResult{Status::Error(StatusCode::kInvalidArgument,
                                     "transaction id and age must be nonzero")};
  }
  const auto [it, inserted] = transactions_.try_emplace(
      command.transaction.id,
      TransactionRecord{command.transaction, TransactionState::kActive, 0});
  if (!inserted && it->second.descriptor != command.transaction) {
    return ShardResult{Status::Error(
        StatusCode::kFailedPrecondition,
        "transaction id was reused with a different descriptor")};
  }
  return ShardResult{Status::Ok()};
}

ShardResult LockShardStateMachine::ApplyNew(const AcquireCommand& command) {
  auto transaction_it = transactions_.find(command.transaction.id);
  if (transaction_it == transactions_.end()) {
    return ShardResult{Status::Error(StatusCode::kNotFound,
                                     "transaction was not begun on shard")};
  }
  if (transaction_it->second.state != TransactionState::kActive) {
    return ShardResult{Status::Error(
        StatusCode::kFailedPrecondition,
        "locks can only be acquired by an active transaction")};
  }
  if (transaction_it->second.descriptor != command.transaction) {
    return ShardResult{Status::Error(StatusCode::kFailedPrecondition,
                                     "transaction descriptor changed")};
  }

  AcquireResult acquire = lock_table_.Acquire(
      command.transaction, command.request, command.enqueue_sequence);
  for (const TransactionId wounded : acquire.wounded_transactions) {
    if (auto wounded_it = transactions_.find(wounded);
        wounded_it != transactions_.end()) {
      wounded_it->second.state = TransactionState::kAborted;
    }
  }
  if (acquire.disposition == AcquireDisposition::kAborted) {
    transaction_it->second.state = TransactionState::kAborted;
    return ShardResult{Status::Error(StatusCode::kAborted, acquire.detail),
                       acquire};
  }
  return ShardResult{Status::Ok(), acquire};
}

ShardResult LockShardStateMachine::ApplyNew(const PrepareCommand& command) {
  auto it = transactions_.find(command.transaction_id);
  if (it == transactions_.end()) {
    return ShardResult{Status::Error(StatusCode::kNotFound,
                                     "transaction was not begun on shard")};
  }
  if (it->second.state == TransactionState::kPrepared) {
    return ShardResult{Status::Ok()};
  }
  if (it->second.state != TransactionState::kActive) {
    return ShardResult{Status::Error(StatusCode::kFailedPrecondition,
                                     "transaction is not active")};
  }
  if (lock_table_.HasWaitingRequests(command.transaction_id)) {
    return ShardResult{Status::Error(
        StatusCode::kFailedPrecondition,
        "transaction cannot prepare while lock requests are waiting")};
  }
  if (const Status protected_status =
          lock_table_.ProtectPrepared(command.transaction_id);
      !protected_status.ok()) {
    return ShardResult{protected_status};
  }
  it->second.state = TransactionState::kPrepared;
  ++metrics_.prepares;
  return ShardResult{Status::Ok()};
}

ShardResult LockShardStateMachine::ApplyNew(const CommitCommand& command) {
  auto it = transactions_.find(command.transaction_id);
  if (it == transactions_.end()) {
    return ShardResult{Status::Error(StatusCode::kNotFound,
                                     "transaction was not begun on shard")};
  }
  if (it->second.state == TransactionState::kCommitted) {
    if (it->second.commit_timestamp != command.commit_timestamp) {
      return ShardResult{Status::Error(StatusCode::kFailedPrecondition,
                                       "commit timestamp changed")};
    }
    return ShardResult{Status::Ok(), std::nullopt, {}, {},
                       command.commit_timestamp};
  }
  if (it->second.state != TransactionState::kPrepared) {
    return ShardResult{Status::Error(StatusCode::kFailedPrecondition,
                                     "only prepared transactions may commit")};
  }
  it->second.state = TransactionState::kCommitted;
  it->second.commit_timestamp = command.commit_timestamp;
  auto grants = lock_table_.ReleaseAll(command.transaction_id);
  ++metrics_.commits;
  return ShardResult{Status::Ok(), std::nullopt, std::move(grants), {},
                     command.commit_timestamp};
}

ShardResult LockShardStateMachine::ApplyNew(const AbortCommand& command) {
  auto it = transactions_.find(command.transaction_id);
  if (it == transactions_.end()) {
    return ShardResult{Status::Ok()};
  }
  if (it->second.state == TransactionState::kCommitted) {
    return ShardResult{Status::Error(StatusCode::kFailedPrecondition,
                                     "committed transaction cannot abort")};
  }
  if (it->second.state == TransactionState::kAborted) {
    return ShardResult{Status::Ok()};
  }
  it->second.state = TransactionState::kAborted;
  auto grants = lock_table_.Abort(command.transaction_id);
  ++metrics_.aborts;
  return ShardResult{Status::Ok(), std::nullopt, std::move(grants)};
}

ShardResult LockShardStateMachine::ApplyNew(const TickCommand& command) {
  std::vector<TransactionId> expired;
  std::vector<GrantEvent> grants;
  for (auto& [id, record] : transactions_) {
    if (record.state == TransactionState::kActive &&
        record.descriptor.deadline_tick != 0 &&
        record.descriptor.deadline_tick <= command.tick) {
      record.state = TransactionState::kAborted;
      auto released = lock_table_.Abort(id);
      grants.insert(grants.end(), released.begin(), released.end());
      expired.push_back(id);
      ++metrics_.aborts;
    }
  }
  return ShardResult{Status::Ok(), std::nullopt, std::move(grants),
                     std::move(expired)};
}

void LockShardStateMachine::Remember(const CommandId& id,
                                     const ShardResult& result) {
  results_[id] = result;
  result_order_.push_back(id);
  while (result_order_.size() > deduplication_window_) {
    results_.erase(result_order_.front());
    result_order_.pop_front();
  }
}

std::optional<TransactionState> LockShardStateMachine::TransactionStatus(
    TransactionId id) const {
  if (const auto it = transactions_.find(id); it != transactions_.end()) {
    return it->second.state;
  }
  return std::nullopt;
}

std::string LockShardStateMachine::Fingerprint() const {
  std::ostringstream out;
  out << "index=" << last_applied_ << ";";
  for (const auto& [id, record] : transactions_) {
    out << "txn=" << id << ":" << record.descriptor.age << ":"
        << ToString(record.state) << ":" << record.commit_timestamp << ";";
  }
  out << "locks=" << lock_table_.Fingerprint();
  return out.str();
}

}  // namespace spanner_lock
