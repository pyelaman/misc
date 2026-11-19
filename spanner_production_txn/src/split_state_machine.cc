#include "spanner_prod/split_state_machine.h"

#include <algorithm>
#include <limits>
#include <sstream>

namespace spanner_prod {

ApplyResult SplitStateMachine::Apply(const LogEntry& entry) {
  if (entry.index != last_applied_ + 1U) {
    return {Status::Error(StatusCode::kFailedPrecondition,
                          "log entries must be applied in strict order"),
            0};
  }
  last_applied_ = entry.index;
  if (const auto existing = operation_results_.find(entry.operation_id);
      existing != operation_results_.end()) {
    return existing->second;
  }
  ApplyResult result = std::visit(
      [this](const auto& command) { return ApplyCommand(command); },
      entry.command);
  operation_results_[entry.operation_id] = result;
  return result;
}

ApplyResult SplitStateMachine::ApplyCommand(const PrepareCommand& command) {
  const auto& prepared = command.prepared;
  if (prepared.transaction.id == 0 || prepared.prepare_timestamp <= 0 ||
      prepared.locks.empty()) {
    return {Status::Error(StatusCode::kInvalidArgument,
                          "invalid durable prepare record"),
            0};
  }
  if (prepared.prepare_timestamp <= paxos_safe_time_) {
    return {Status::Error(
                StatusCode::kFailedPrecondition,
                "prepare timestamp is not above advertised safe time"),
            0};
  }
  if (aborted_.contains(prepared.transaction.id)) {
    return {Status::Error(StatusCode::kAborted,
                          "aborted transaction cannot prepare"),
            0};
  }
  if (const auto committed = committed_.find(prepared.transaction.id);
      committed != committed_.end()) {
    return {Status::Error(StatusCode::kFailedPrecondition,
                          "committed transaction cannot prepare"),
            committed->second};
  }
  const auto [it, inserted] =
      prepared_.try_emplace(prepared.transaction.id, prepared);
  if (!inserted && it->second != prepared) {
    return {Status::Error(StatusCode::kFailedPrecondition,
                          "transaction has a different prepare record"),
            0};
  }
  last_assigned_timestamp_ =
      std::max(last_assigned_timestamp_, prepared.prepare_timestamp);
  return {Status::Ok(), prepared.prepare_timestamp};
}

ApplyResult SplitStateMachine::ApplyCommand(const CommitCommand& command) {
  if (const auto committed = committed_.find(command.transaction_id);
      committed != committed_.end()) {
    if (committed->second != command.commit_timestamp) {
      return {Status::Error(StatusCode::kFailedPrecondition,
                            "commit timestamp changed during replay"),
              committed->second};
    }
    return {Status::Ok(), committed->second};
  }
  if (aborted_.contains(command.transaction_id)) {
    return {Status::Error(StatusCode::kFailedPrecondition,
                          "aborted transaction cannot commit"),
            0};
  }
  const auto prepared = prepared_.find(command.transaction_id);
  if (prepared == prepared_.end()) {
    return {Status::Error(StatusCode::kNotFound,
                          "commit has no durable prepare record"),
            0};
  }
  if (command.commit_timestamp < prepared->second.prepare_timestamp) {
    return {Status::Error(StatusCode::kFailedPrecondition,
                          "commit timestamp precedes prepare timestamp"),
            0};
  }
  const Status applied =
      store_.Apply(command.commit_timestamp, prepared->second.mutations);
  if (!applied.ok()) {
    return {applied, 0};
  }
  committed_[command.transaction_id] = command.commit_timestamp;
  prepared_.erase(prepared);
  last_assigned_timestamp_ =
      std::max(last_assigned_timestamp_, command.commit_timestamp);
  return {Status::Ok(), command.commit_timestamp};
}

ApplyResult SplitStateMachine::ApplyCommand(const AbortCommand& command) {
  if (committed_.contains(command.transaction_id)) {
    return {Status::Error(StatusCode::kFailedPrecondition,
                          "committed transaction cannot abort"),
            committed_.at(command.transaction_id)};
  }
  prepared_.erase(command.transaction_id);
  aborted_.insert(command.transaction_id);
  return {Status::Ok(), 0};
}

ApplyResult SplitStateMachine::ApplyCommand(
    const RecordDecisionCommand& command) {
  const auto [it, inserted] = decisions_.try_emplace(
      command.decision.transaction_id, command.decision);
  if (!inserted && it->second != command.decision) {
    return {Status::Error(StatusCode::kFailedPrecondition,
                          "transaction decision is immutable"),
            it->second.commit_timestamp};
  }
  if (command.decision.kind == DecisionKind::kCommit) {
    last_assigned_timestamp_ = std::max(last_assigned_timestamp_,
                                        command.decision.commit_timestamp);
  }
  return {Status::Ok(), command.decision.commit_timestamp};
}

ApplyResult SplitStateMachine::ApplyCommand(
    const AdvanceSafeTimeCommand& command) {
  if (command.timestamp < paxos_safe_time_) {
    return {Status::Ok(), paxos_safe_time_};
  }
  paxos_safe_time_ = command.timestamp;
  last_assigned_timestamp_ =
      std::max(last_assigned_timestamp_, command.timestamp);
  return {Status::Ok(), safe_time()};
}

Timestamp SplitStateMachine::safe_time() const {
  Timestamp transaction_safe = std::numeric_limits<Timestamp>::max();
  for (const auto& [id, prepared] : prepared_) {
    (void)id;
    transaction_safe =
        std::min(transaction_safe, prepared.prepare_timestamp - 1);
  }
  return std::min(paxos_safe_time_, transaction_safe);
}

std::optional<TransactionDecision> SplitStateMachine::Decision(
    TransactionId transaction_id) const {
  if (const auto it = decisions_.find(transaction_id);
      it != decisions_.end()) {
    return it->second;
  }
  return std::nullopt;
}

StatusOr<std::optional<std::string>> SplitStateMachine::ReadAt(
    const Cell& cell, Timestamp timestamp) const {
  if (timestamp > safe_time()) {
    return Status::Error(StatusCode::kUnavailable,
                         "replica safe time has not reached read timestamp");
  }
  return store_.Read(cell, timestamp);
}

std::string SplitStateMachine::Fingerprint() const {
  std::ostringstream out;
  out << "index=" << last_applied_ << ";safe=" << paxos_safe_time_
      << ";assigned=" << last_assigned_timestamp_ << ';';
  for (const auto& [id, prepared] : prepared_) {
    out << "P" << id << '@' << prepared.prepare_timestamp << ':';
    for (const auto& lock : prepared.locks) {
      out << lock.cell.ToString() << '=' << ToString(lock.mode) << ',';
    }
    out << ';';
  }
  for (const auto& [id, decision] : decisions_) {
    out << "D" << id << '='
        << (decision.kind == DecisionKind::kCommit ? 'C' : 'A') << '@'
        << decision.commit_timestamp << ';';
  }
  for (const auto& [id, timestamp] : committed_) {
    out << "C" << id << '@' << timestamp << ';';
  }
  for (const auto id : aborted_) {
    out << "A" << id << ';';
  }
  out << "data=" << store_.Fingerprint();
  return out.str();
}

}  // namespace spanner_prod

