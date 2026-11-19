#include "spanner_prod/split_group.h"

#include <algorithm>

namespace spanner_prod {

SplitGroup::SplitGroup(SplitId split_id, std::filesystem::path root,
                       std::size_t replicas)
    : consensus_(split_id, std::move(root), replicas) {
  next_operation_sequence_ = consensus_.last_log_index() + 1;
  const Status restored = RestorePreparedLocks();
  if (!restored.ok()) {
    throw std::runtime_error(restored.message);
  }
}

std::uint64_t SplitGroup::NextOperationId(TransactionId transaction_id) {
  (void)transaction_id;
  return next_operation_sequence_++;
}

Status SplitGroup::Begin(const TransactionDescriptor& transaction) {
  std::lock_guard lock(mutex_);
  if (transaction.id == 0 || transaction.priority == 0) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "transaction id and priority must be nonzero");
  }
  const auto [it, inserted] = active_.try_emplace(
      transaction.id, ActiveTransaction{transaction, {}});
  if (!inserted && it->second.descriptor != transaction) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "transaction descriptor changed");
  }
  return Status::Ok();
}

GroupAcquireResult SplitGroup::Acquire(TransactionId transaction_id,
                                       const LockRequest& request) {
  std::lock_guard lock(mutex_);
  const auto transaction = active_.find(transaction_id);
  if (transaction == active_.end()) {
    return {Status::Error(StatusCode::kNotFound,
                          "transaction is not active at split"),
            {AcquireDisposition::kAborted, {}, 0, {}}};
  }
  AcquireResult acquired = locks_.Acquire(
      transaction->second.descriptor, request, next_sequence_++);
  for (const auto wounded : acquired.wounded) {
    active_.erase(wounded);
  }
  if (acquired.disposition == AcquireDisposition::kAborted) {
    active_.erase(transaction_id);
    return {Status::Error(StatusCode::kAborted, acquired.detail), acquired};
  }
  return {Status::Ok(), acquired};
}

Status SplitGroup::BufferMutation(TransactionId transaction_id,
                                  Mutation mutation) {
  std::lock_guard lock(mutex_);
  const auto transaction = active_.find(transaction_id);
  if (transaction == active_.end()) {
    return Status::Error(StatusCode::kNotFound,
                         "transaction is not active at split");
  }
  if (!locks_.Holds(transaction_id,
                    {mutation.cell, LockMode::kWriterShared})) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "write mutation has no writer lock");
  }
  auto& mutations = transaction->second.mutations;
  const auto existing = std::find_if(
      mutations.begin(), mutations.end(), [&](const Mutation& value) {
        return value.cell == mutation.cell;
      });
  if (existing == mutations.end()) {
    mutations.push_back(std::move(mutation));
  } else {
    *existing = std::move(mutation);
  }
  return Status::Ok();
}

StatusOr<Timestamp> SplitGroup::Prepare(TransactionId transaction_id,
                                        Timestamp lower_bound) {
  std::lock_guard lock(mutex_);
  const auto transaction = active_.find(transaction_id);
  if (transaction == active_.end()) {
    if (const auto prepared = consensus_.Prepared();
        prepared.contains(transaction_id)) {
      return prepared.at(transaction_id).prepare_timestamp;
    }
    return Status::Error(StatusCode::kNotFound,
                         "transaction is not active at split");
  }
  if (locks_.HasWaiters(transaction_id)) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "cannot prepare while a lock is waiting");
  }
  const auto held = locks_.HeldLocks(transaction_id);
  if (held.empty()) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "cannot prepare without held locks");
  }
  const Timestamp prepare_timestamp =
      std::max({lower_bound, consensus_.last_assigned_timestamp() + 1,
                consensus_.safe_time() + 1});
  PreparedTransaction prepared{transaction->second.descriptor,
                               prepare_timestamp, held,
                               transaction->second.mutations};
  if (const Status protected_status = locks_.ProtectPrepared(transaction_id);
      !protected_status.ok()) {
    return protected_status;
  }
  const ApplyResult result = consensus_.Propose(
      NextOperationId(transaction_id), PrepareCommand{prepared});
  if (!result.status.ok()) {
    return result.status;
  }
  active_.erase(transaction);
  return prepare_timestamp;
}

Status SplitGroup::Commit(TransactionId transaction_id,
                          Timestamp commit_timestamp) {
  std::lock_guard lock(mutex_);
  const ApplyResult result = consensus_.Propose(
      NextOperationId(transaction_id),
      CommitCommand{transaction_id, commit_timestamp});
  if (result.status.ok()) {
    locks_.Release(transaction_id);
    active_.erase(transaction_id);
  }
  return result.status;
}

Status SplitGroup::Abort(TransactionId transaction_id, std::string reason) {
  std::lock_guard lock(mutex_);
  const ApplyResult result = consensus_.Propose(
      NextOperationId(transaction_id),
      AbortCommand{transaction_id, std::move(reason)});
  if (result.status.ok()) {
    locks_.Release(transaction_id, true);
    active_.erase(transaction_id);
  }
  return result.status;
}

Status SplitGroup::RecordDecision(const TransactionDecision& decision) {
  std::lock_guard lock(mutex_);
  return consensus_
      .Propose(NextOperationId(decision.transaction_id),
               RecordDecisionCommand{decision})
      .status;
}

Status SplitGroup::AdvanceSafeTime(Timestamp timestamp) {
  std::lock_guard lock(mutex_);
  return consensus_
      .Propose(NextOperationId(0), AdvanceSafeTimeCommand{timestamp})
      .status;
}

Status SplitGroup::RestorePreparedLocks() {
  try {
    locks_.ResetAndRestore(consensus_.Prepared());
  } catch (const std::exception& error) {
    return Status::Error(StatusCode::kCorruption, error.what());
  }
  return Status::Ok();
}

Status SplitGroup::ElectLeader(std::size_t replica) {
  std::lock_guard lock(mutex_);
  if (const Status recovered = consensus_.RecoverPending(); !recovered.ok()) {
    return recovered;
  }
  if (const Status elected = consensus_.ElectLeader(replica); !elected.ok()) {
    return elected;
  }
  active_.clear();
  return RestorePreparedLocks();
}

Status SplitGroup::SetReplicaLive(std::size_t replica, bool live) {
  return consensus_.SetReplicaLive(replica, live);
}

void SplitGroup::FailAfterQuorumAppendOnce() {
  consensus_.FailAfterQuorumAppendOnce();
}

Status SplitGroup::RecoverPendingConsensus() {
  std::lock_guard lock(mutex_);
  const Status recovered = consensus_.RecoverPending();
  if (!recovered.ok()) {
    return recovered;
  }
  return RestorePreparedLocks();
}

std::string SplitGroup::Fingerprint() const {
  std::lock_guard lock(mutex_);
  return consensus_.Fingerprint() + ";leader-locks=" + locks_.Fingerprint();
}

}  // namespace spanner_prod
