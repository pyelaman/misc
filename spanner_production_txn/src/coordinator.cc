#include "spanner_prod/coordinator.h"

#include <algorithm>
#include <stdexcept>

namespace spanner_prod {

TransactionCoordinator::TransactionCoordinator(
    std::shared_ptr<RangeDirectory> directory,
    std::shared_ptr<ITrueTime> true_time)
    : directory_(std::move(directory)), true_time_(std::move(true_time)) {
  if (directory_ == nullptr || true_time_ == nullptr) {
    throw std::invalid_argument("directory and TrueTime are required");
  }
}

TransactionHandle TransactionCoordinator::Begin(Timestamp deadline,
                                                std::string tag) {
  std::lock_guard lock(mutex_);
  TransactionDescriptor descriptor{next_transaction_id_++, next_priority_++,
                                   deadline, 0, std::move(tag)};
  transactions_.emplace(descriptor.id,
                        Record{descriptor, State::kActive, {}});
  return {descriptor.id};
}

StatusOr<std::shared_ptr<SplitGroup>>
TransactionCoordinator::EnsureParticipant(Record& transaction,
                                          const Cell& cell) {
  auto lookup = directory_->Lookup(cell);
  if (!lookup.ok()) {
    return lookup.status();
  }
  const auto& group = lookup.value();
  if (transaction.participants.contains(group->id())) {
    return group;
  }
  if (transaction.descriptor.coordinator_split == 0) {
    transaction.descriptor.coordinator_split = group->id();
  }
  if (const Status begun = group->Begin(transaction.descriptor); !begun.ok()) {
    return begun;
  }
  transaction.participants.emplace(group->id(), group);
  return group;
}

TransactionAcquireResult TransactionCoordinator::Acquire(
    TransactionHandle transaction,
    const std::vector<LockRequest>& requests) {
  std::lock_guard lock(mutex_);
  const auto record_it = transactions_.find(transaction.id);
  if (record_it == transactions_.end()) {
    return {Status::Error(StatusCode::kNotFound,
                          "transaction handle is unknown"),
            false, {}};
  }
  auto& record = record_it->second;
  if (record.state != State::kActive) {
    return {Status::Error(StatusCode::kFailedPrecondition,
                          "transaction is not active"),
            false, {}};
  }
  TransactionAcquireResult aggregate{Status::Ok(), true, {}};
  for (const auto& request : requests) {
    auto participant = EnsureParticipant(record, request.cell);
    if (!participant.ok()) {
      aggregate.status = participant.status();
      aggregate.all_granted = false;
      return aggregate;
    }
    auto acquired = participant.value()->Acquire(transaction.id, request);
    if (!acquired.status.ok()) {
      aggregate.status = acquired.status;
      aggregate.all_granted = false;
      return aggregate;
    }
    aggregate.all_granted &=
        acquired.acquire.disposition == AcquireDisposition::kGranted;
    aggregate.wounded.insert(aggregate.wounded.end(),
                             acquired.acquire.wounded.begin(),
                             acquired.acquire.wounded.end());
  }
  std::sort(aggregate.wounded.begin(), aggregate.wounded.end());
  aggregate.wounded.erase(
      std::unique(aggregate.wounded.begin(), aggregate.wounded.end()),
      aggregate.wounded.end());
  for (const auto wounded : aggregate.wounded) {
    if (const auto victim = transactions_.find(wounded);
        victim != transactions_.end() && victim->second.state == State::kActive) {
      (void)AbortLocked(victim->second, "wounded by older transaction");
    }
  }
  return aggregate;
}

Status TransactionCoordinator::BufferWrite(TransactionHandle transaction,
                                            Mutation mutation) {
  std::lock_guard lock(mutex_);
  const auto record_it = transactions_.find(transaction.id);
  if (record_it == transactions_.end() ||
      record_it->second.state != State::kActive) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "transaction is not active");
  }
  auto participant = EnsureParticipant(record_it->second, mutation.cell);
  if (!participant.ok()) {
    return participant.status();
  }
  auto acquired = participant.value()->Acquire(
      transaction.id, {mutation.cell, LockMode::kWriterShared});
  if (!acquired.status.ok()) {
    return acquired.status;
  }
  if (acquired.acquire.disposition != AcquireDisposition::kGranted) {
    return Status::Error(StatusCode::kUnavailable,
                         "writer lock is still waiting");
  }
  for (const auto wounded : acquired.acquire.wounded) {
    if (const auto victim = transactions_.find(wounded);
        victim != transactions_.end() && victim->second.state == State::kActive) {
      (void)AbortLocked(victim->second, "wounded by older writer");
    }
  }
  return participant.value()->BufferMutation(transaction.id,
                                              std::move(mutation));
}

Status TransactionCoordinator::AbortLocked(Record& transaction,
                                           std::string reason) {
  if (transaction.state == State::kCommitted) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "committed transaction cannot abort");
  }
  if (transaction.state == State::kAborted) {
    return Status::Ok();
  }
  if (transaction.participants.empty()) {
    transaction.state = State::kAborted;
    return Status::Ok();
  }
  std::vector<SplitId> participant_ids;
  for (const auto& [id, group] : transaction.participants) {
    (void)group;
    participant_ids.push_back(id);
  }
  const TransactionDecision decision{transaction.descriptor.id,
                                     DecisionKind::kAbort, 0,
                                     participant_ids, reason};
  auto coordinator =
      transaction.participants.at(transaction.descriptor.coordinator_split);
  if (const Status recorded = coordinator->RecordDecision(decision);
      !recorded.ok()) {
    return recorded;
  }
  transaction.state = State::kAborted;
  Status first_failure = Status::Ok();
  for (const auto& [id, group] : transaction.participants) {
    (void)id;
    const Status aborted = group->Abort(transaction.descriptor.id, reason);
    if (!aborted.ok() && first_failure.ok()) {
      first_failure = aborted;
    }
  }
  return first_failure;
}

Status TransactionCoordinator::Abort(TransactionHandle transaction,
                                     std::string reason) {
  std::lock_guard lock(mutex_);
  const auto it = transactions_.find(transaction.id);
  if (it == transactions_.end()) {
    return Status::Error(StatusCode::kNotFound,
                         "transaction handle is unknown");
  }
  return AbortLocked(it->second, std::move(reason));
}

StatusOr<RecoveryToken> TransactionCoordinator::GetRecoveryToken(
    TransactionHandle transaction) const {
  std::lock_guard lock(mutex_);
  const auto it = transactions_.find(transaction.id);
  if (it == transactions_.end() ||
      it->second.descriptor.coordinator_split == 0) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "transaction has no coordinator participant yet");
  }
  return RecoveryToken{transaction.id,
                       it->second.descriptor.coordinator_split};
}

TransactionCommitResult TransactionCoordinator::Commit(
    TransactionHandle transaction,
    std::chrono::milliseconds commit_wait_timeout) {
  std::lock_guard lock(mutex_);
  const auto it = transactions_.find(transaction.id);
  if (it == transactions_.end()) {
    return {Status::Error(StatusCode::kNotFound,
                          "transaction handle is unknown"),
            0};
  }
  auto& record = it->second;
  if (record.state != State::kActive) {
    return {Status::Error(StatusCode::kFailedPrecondition,
                          "transaction is not active"),
            0};
  }
  auto now = true_time_->Now();
  if (!now.ok()) {
    return {now.status(), 0};
  }
  if (record.participants.empty()) {
    const Timestamp timestamp = now.value().latest;
    const Status waited =
        true_time_->CommitWait(timestamp, commit_wait_timeout);
    if (waited.ok()) {
      record.state = State::kCommitted;
    }
    return {waited, timestamp};
  }

  record.state = State::kPreparing;
  Timestamp commit_timestamp = now.value().latest;
  for (const auto& [id, group] : record.participants) {
    (void)id;
    auto prepared = group->Prepare(record.descriptor.id, commit_timestamp);
    if (!prepared.ok()) {
      record.state = State::kActive;
      const Status aborted = AbortLocked(
          record, "prepare failed: " + prepared.status().message);
      return {aborted.ok() ? prepared.status() : aborted, 0};
    }
    commit_timestamp = std::max(commit_timestamp, prepared.value());
  }
  record.state = State::kPrepared;
  auto coordinator =
      record.participants.at(record.descriptor.coordinator_split);
  commit_timestamp =
      std::max(commit_timestamp, coordinator->last_assigned_timestamp() + 1);
  std::vector<SplitId> participant_ids;
  for (const auto& [id, group] : record.participants) {
    (void)group;
    participant_ids.push_back(id);
  }
  const TransactionDecision decision{record.descriptor.id,
                                     DecisionKind::kCommit,
                                     commit_timestamp, participant_ids, {}};
  const Status recorded = coordinator->RecordDecision(decision);
  if (!recorded.ok()) {
    record.state = State::kPrepared;
    return {Status::Error(StatusCode::kInDoubt,
                          "coordinator decision delivery is uncertain: " +
                              recorded.message),
            commit_timestamp};
  }
  record.state = State::kCommitting;
  const Status waited =
      true_time_->CommitWait(commit_timestamp, commit_wait_timeout);
  if (!waited.ok()) {
    return {Status::Error(StatusCode::kInDoubt,
                          "commit is durable but commit wait failed: " +
                              waited.message),
            commit_timestamp};
  }
  const Status replayed =
      ReplayDecisionLocked(record, decision, commit_wait_timeout);
  return {replayed.ok()
              ? replayed
              : Status::Error(StatusCode::kInDoubt,
                              "commit decision delivery incomplete: " +
                                  replayed.message),
          commit_timestamp};
}

Status TransactionCoordinator::ReplayDecisionLocked(
    Record& transaction, const TransactionDecision& decision,
    std::chrono::milliseconds commit_wait_timeout) {
  if (decision.kind == DecisionKind::kCommit) {
    if (const Status waited = true_time_->CommitWait(
            decision.commit_timestamp, commit_wait_timeout);
        !waited.ok()) {
      return waited;
    }
  }
  Status first_failure = Status::Ok();
  for (const auto& [id, group] : transaction.participants) {
    (void)id;
    const Status delivered =
        decision.kind == DecisionKind::kCommit
            ? group->Commit(decision.transaction_id,
                            decision.commit_timestamp)
            : group->Abort(decision.transaction_id, decision.reason);
    if (!delivered.ok() && first_failure.ok()) {
      first_failure = delivered;
    }
  }
  if (!first_failure.ok()) {
    return first_failure;
  }
  if (decision.kind == DecisionKind::kCommit) {
    for (const auto& [id, group] : transaction.participants) {
      (void)id;
      (void)group->AdvanceSafeTime(decision.commit_timestamp);
    }
    transaction.state = State::kCommitted;
  } else {
    transaction.state = State::kAborted;
  }
  return Status::Ok();
}

Status TransactionCoordinator::Recover(
    TransactionHandle transaction,
    std::chrono::milliseconds commit_wait_timeout) {
  std::lock_guard lock(mutex_);
  const auto it = transactions_.find(transaction.id);
  if (it == transactions_.end() || it->second.participants.empty()) {
    return Status::Error(StatusCode::kNotFound,
                         "transaction recovery metadata is unavailable");
  }
  auto& record = it->second;
  auto coordinator =
      record.participants.at(record.descriptor.coordinator_split);
  (void)coordinator->RecoverPendingConsensus();
  const auto decision = coordinator->Decision(transaction.id);
  if (!decision.has_value()) {
    return AbortLocked(record, "no durable coordinator decision");
  }
  return ReplayDecisionLocked(record, *decision, commit_wait_timeout);
}

Status TransactionCoordinator::Recover(
    RecoveryToken token,
    std::chrono::milliseconds commit_wait_timeout) {
  std::lock_guard lock(mutex_);
  std::map<SplitId, std::shared_ptr<SplitGroup>> groups;
  for (const auto& group : directory_->groups()) {
    groups.emplace(group->id(), group);
  }
  const auto coordinator_it = groups.find(token.coordinator_split);
  if (coordinator_it == groups.end()) {
    return Status::Error(StatusCode::kNotFound,
                         "recovery coordinator split is unavailable");
  }
  (void)coordinator_it->second->RecoverPendingConsensus();
  auto decision = coordinator_it->second->Decision(token.transaction_id);

  Record recovered{{token.transaction_id, token.transaction_id, 0,
                    token.coordinator_split, "recovered"},
                   State::kPrepared, {}};
  if (decision.has_value()) {
    for (const auto split_id : decision->participants) {
      const auto group = groups.find(split_id);
      if (group == groups.end()) {
        return Status::Error(StatusCode::kUnavailable,
                             "decision participant split is unavailable");
      }
      recovered.participants.emplace(split_id, group->second);
    }
  } else {
    // No decision means commit was never made durable. Discover prepared
    // participants, record abort at the coordinator, then replay it.
    for (const auto& [split_id, group] : groups) {
      if (group->Prepared().contains(token.transaction_id)) {
        recovered.participants.emplace(split_id, group);
      }
    }
    if (!recovered.participants.contains(token.coordinator_split)) {
      recovered.participants.emplace(token.coordinator_split,
                                     coordinator_it->second);
    }
    std::vector<SplitId> participants;
    for (const auto& [split_id, group] : recovered.participants) {
      (void)group;
      participants.push_back(split_id);
    }
    TransactionDecision abort{token.transaction_id, DecisionKind::kAbort, 0,
                              participants,
                              "recovery found no durable commit decision"};
    if (const Status recorded =
            coordinator_it->second->RecordDecision(abort);
        !recorded.ok()) {
      return recorded;
    }
    decision = std::move(abort);
  }
  const Status replayed =
      ReplayDecisionLocked(recovered, *decision, commit_wait_timeout);
  if (replayed.ok()) {
    transactions_[token.transaction_id] = std::move(recovered);
  }
  return replayed;
}

StatusOr<std::optional<std::string>> TransactionCoordinator::SnapshotRead(
    const Cell& cell, Timestamp timestamp) const {
  auto group = directory_->Lookup(cell);
  if (!group.ok()) {
    return group.status();
  }
  return group.value()->ReadAt(cell, timestamp);
}

StatusOr<std::optional<std::string>> TransactionCoordinator::StrongRead(
    const Cell& cell) const {
  auto now = true_time_->Now();
  if (!now.ok()) {
    return now.status();
  }
  auto group = directory_->Lookup(cell);
  if (!group.ok()) {
    return group.status();
  }
  if (const Status advanced =
          group.value()->AdvanceSafeTime(now.value().latest);
      !advanced.ok()) {
    return advanced;
  }
  return group.value()->ReadAt(cell, now.value().latest);
}

}  // namespace spanner_prod
