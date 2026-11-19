#include "spanner_lock/coordinator.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

namespace spanner_lock {
namespace {

CommitTimestamp SystemMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

SystemUncertaintyClock::SystemUncertaintyClock(
    std::chrono::microseconds uncertainty)
    : uncertainty_(uncertainty) {
  if (uncertainty_.count() < 0) {
    throw std::invalid_argument("clock uncertainty cannot be negative");
  }
}

TimeInterval SystemUncertaintyClock::Now() const {
  const auto now = SystemMicros();
  return TimeInterval{now - uncertainty_.count(), now + uncertainty_.count()};
}

void SystemUncertaintyClock::CommitWait(CommitTimestamp timestamp) const {
  // External consistency requires TT.after(s): the earliest possible current
  // time must be greater than the chosen commit timestamp.
  while (Now().earliest <= timestamp) {
    const auto remaining = timestamp - Now().earliest + 1;
    std::this_thread::sleep_for(std::chrono::microseconds(
        std::max<CommitTimestamp>(remaining, 1)));
  }
}

Status InMemoryDecisionStore::PutIfAbsent(const DecisionRecord& record) {
  std::lock_guard lock(mutex_);
  const auto [it, inserted] = records_.try_emplace(record.transaction_id,
                                                   record);
  if (!inserted && it->second != record) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "transaction already has a different decision");
  }
  return Status::Ok();
}

std::optional<DecisionRecord> InMemoryDecisionStore::Get(
    TransactionId transaction_id) const {
  std::lock_guard lock(mutex_);
  if (const auto it = records_.find(transaction_id); it != records_.end()) {
    return it->second;
  }
  return std::nullopt;
}

TransactionCoordinator::TransactionCoordinator(
    std::vector<std::shared_ptr<IReplicatedShard>> shards,
    std::shared_ptr<IDecisionStore> decision_store,
    std::shared_ptr<ICommitClock> clock)
    : shards_(std::move(shards)),
      decision_store_(std::move(decision_store)),
      clock_(std::move(clock)) {
  if (shards_.empty() || decision_store_ == nullptr || clock_ == nullptr) {
    throw std::invalid_argument(
        "at least one shard, a decision store, and a clock are required");
  }
  for (const auto& shard : shards_) {
    if (shard == nullptr || !shards_by_id_.emplace(shard->id(), shard).second) {
      throw std::invalid_argument("shards must be non-null with unique ids");
    }
  }
}

TransactionHandle TransactionCoordinator::Begin(LogicalTick deadline_tick,
                                                std::string tag) {
  std::lock_guard lock(mutex_);
  TransactionDescriptor descriptor{next_transaction_id_++,
                                   next_transaction_age_++, deadline_tick,
                                   std::move(tag)};
  transactions_.emplace(
      descriptor.id,
      CoordinatorRecord{descriptor, TransactionState::kActive, {}, 1});
  return TransactionHandle{descriptor};
}

CommandId TransactionCoordinator::NextCommandId(
    CoordinatorRecord& transaction) {
  return CommandId{transaction.descriptor.id,
                   transaction.next_command_sequence++};
}

IReplicatedShard* TransactionCoordinator::FindShard(ShardId shard_id) const {
  if (const auto it = shards_by_id_.find(shard_id);
      it != shards_by_id_.end()) {
    return it->second.get();
  }
  return nullptr;
}

ShardId TransactionCoordinator::ShardFor(const Cell& cell) const {
  const auto index = CellHash{}(cell) % shards_.size();
  return shards_[index]->id();
}

Status TransactionCoordinator::EnsureBegun(CoordinatorRecord& transaction,
                                            ShardId shard_id) {
  if (transaction.participants.contains(shard_id)) {
    return Status::Ok();
  }
  auto* shard = FindShard(shard_id);
  if (shard == nullptr) {
    return Status::Error(StatusCode::kInternal, "participant shard is absent");
  }
  const auto result = shard->Propose(CommandEnvelope{
      NextCommandId(transaction), BeginCommand{transaction.descriptor}});
  if (result.status.ok()) {
    transaction.participants.insert(shard_id);
  }
  return result.status;
}

CoordinatorAcquireResult TransactionCoordinator::Acquire(
    const TransactionHandle& transaction,
    const std::vector<LockRequest>& requests) {
  std::lock_guard lock(mutex_);
  auto it = transactions_.find(transaction.descriptor.id);
  if (it == transactions_.end() ||
      it->second.descriptor != transaction.descriptor) {
    return {Status::Error(StatusCode::kNotFound,
                          "transaction handle is unknown"),
            false, {}, {}};
  }
  auto& record = it->second;
  if (record.state != TransactionState::kActive) {
    return {Status::Error(StatusCode::kFailedPrecondition,
                          "transaction is not active"),
            false, {}, {}};
  }

  CoordinatorAcquireResult aggregate{Status::Ok(), true, {}, {}};
  std::uint64_t enqueue_sequence = 1;
  for (const auto& request : requests) {
    const ShardId shard_id = ShardFor(request.cell);
    if (const Status status = EnsureBegun(record, shard_id); !status.ok()) {
      aggregate.status = status;
      aggregate.all_granted = false;
      return aggregate;
    }
    auto* shard = FindShard(shard_id);
    const auto result = shard->Propose(CommandEnvelope{
        NextCommandId(record),
        AcquireCommand{record.descriptor, request, enqueue_sequence++}});
    if (!result.status.ok() && result.status.code != StatusCode::kAborted) {
      aggregate.status = result.status;
      aggregate.all_granted = false;
      return aggregate;
    }
    if (result.acquire.has_value()) {
      aggregate.shard_results[shard_id] = *result.acquire;
      aggregate.wounded_transactions.insert(
          aggregate.wounded_transactions.end(),
          result.acquire->wounded_transactions.begin(),
          result.acquire->wounded_transactions.end());
      aggregate.all_granted &=
          result.acquire->disposition == AcquireDisposition::kGranted;
    }
    if (result.status.code == StatusCode::kAborted) {
      record.state = TransactionState::kAborted;
      aggregate.status = result.status;
      aggregate.all_granted = false;
      return aggregate;
    }
  }

  // A wound is local evidence that another transaction must abort globally.
  // A production service would route this through that transaction's home
  // coordinator; this reference coordinator does so when it owns the victim.
  std::sort(aggregate.wounded_transactions.begin(),
            aggregate.wounded_transactions.end());
  aggregate.wounded_transactions.erase(
      std::unique(aggregate.wounded_transactions.begin(),
                  aggregate.wounded_transactions.end()),
      aggregate.wounded_transactions.end());
  for (const auto wounded : aggregate.wounded_transactions) {
    if (auto wounded_it = transactions_.find(wounded);
        wounded_it != transactions_.end() &&
        wounded_it->second.state != TransactionState::kCommitted &&
        wounded_it->second.state != TransactionState::kAborted) {
      (void)AbortLocked(wounded_it->second, "wounded by older transaction");
    }
  }
  return aggregate;
}

Status TransactionCoordinator::AbortLocked(CoordinatorRecord& transaction,
                                           std::string reason) {
  std::vector<ShardId> participants(transaction.participants.begin(),
                                    transaction.participants.end());
  const DecisionRecord decision{transaction.descriptor.id,
                                CoordinatorDecision::kAbort, 0, participants,
                                std::move(reason)};
  if (const Status stored = decision_store_->PutIfAbsent(decision);
      !stored.ok()) {
    return stored;
  }
  transaction.state = TransactionState::kAborted;
  return ReplayDecision(decision, &transaction);
}

Status TransactionCoordinator::Abort(const TransactionHandle& transaction,
                                     std::string reason) {
  std::lock_guard lock(mutex_);
  auto it = transactions_.find(transaction.descriptor.id);
  if (it == transactions_.end() ||
      it->second.descriptor != transaction.descriptor) {
    return Status::Error(StatusCode::kNotFound,
                         "transaction handle is unknown");
  }
  if (it->second.state == TransactionState::kCommitted) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "committed transaction cannot abort");
  }
  return AbortLocked(it->second, std::move(reason));
}

CommitResult TransactionCoordinator::Commit(
    const TransactionHandle& transaction) {
  std::lock_guard lock(mutex_);
  auto it = transactions_.find(transaction.descriptor.id);
  if (it == transactions_.end() ||
      it->second.descriptor != transaction.descriptor) {
    return {Status::Error(StatusCode::kNotFound,
                          "transaction handle is unknown"),
            0};
  }
  auto& record = it->second;
  if (record.state != TransactionState::kActive) {
    return {Status::Error(StatusCode::kFailedPrecondition,
                          "transaction is not active"),
            0};
  }

  record.state = TransactionState::kPreparing;
  for (const auto shard_id : record.participants) {
    const auto prepared = FindShard(shard_id)->Propose(CommandEnvelope{
        NextCommandId(record), PrepareCommand{record.descriptor.id}});
    if (!prepared.status.ok()) {
      record.state = TransactionState::kActive;
      const Status aborted = AbortLocked(
          record, "prepare failed: " + prepared.status.message);
      return {aborted.ok() ? prepared.status : aborted, 0};
    }
  }
  record.state = TransactionState::kPrepared;

  const auto interval = clock_->Now();
  const CommitTimestamp timestamp =
      std::max(interval.latest, last_commit_timestamp_ + 1);
  std::vector<ShardId> participants(record.participants.begin(),
                                    record.participants.end());
  const DecisionRecord decision{record.descriptor.id,
                                CoordinatorDecision::kCommit, timestamp,
                                participants, {}};
  if (const Status stored = decision_store_->PutIfAbsent(decision);
      !stored.ok()) {
    return {stored, 0};
  }
  last_commit_timestamp_ = timestamp;
  record.state = TransactionState::kCommitting;
  const Status replayed = ReplayDecision(decision, &record);
  if (!replayed.ok()) {
    return {Status::Error(
                StatusCode::kInDoubt,
                "commit decision is durable but delivery is incomplete: " +
                    replayed.message),
            timestamp};
  }
  clock_->CommitWait(timestamp);
  return {Status::Ok(), timestamp};
}

Status TransactionCoordinator::ReplayDecision(
    const DecisionRecord& decision, CoordinatorRecord* transaction) {
  Status first_failure = Status::Ok();
  std::uint64_t recovery_sequence = 1;
  for (const auto shard_id : decision.participants) {
    auto* shard = FindShard(shard_id);
    if (shard == nullptr) {
      if (first_failure.ok()) {
        first_failure = Status::Error(StatusCode::kInternal,
                                      "decision names an absent shard");
      }
      continue;
    }
    const CommandId command_id = transaction != nullptr
                                     ? NextCommandId(*transaction)
                                     : CommandId{decision.transaction_id,
                                                 1'000'000 + recovery_sequence++};
    ShardResult result;
    if (decision.decision == CoordinatorDecision::kCommit) {
      result = shard->Propose(CommandEnvelope{
          command_id,
          CommitCommand{decision.transaction_id, decision.commit_timestamp}});
    } else {
      result = shard->Propose(CommandEnvelope{
          command_id, AbortCommand{decision.transaction_id, decision.reason}});
    }
    if (!result.status.ok() && first_failure.ok()) {
      first_failure = result.status;
    }
  }
  if (first_failure.ok() && transaction != nullptr) {
    transaction->state = decision.decision == CoordinatorDecision::kCommit
                             ? TransactionState::kCommitted
                             : TransactionState::kAborted;
  }
  return first_failure;
}

Status TransactionCoordinator::Recover(TransactionId transaction_id) {
  std::lock_guard lock(mutex_);
  const auto decision = decision_store_->Get(transaction_id);
  if (!decision.has_value()) {
    return Status::Error(StatusCode::kNotFound,
                         "no durable decision exists for transaction");
  }
  CoordinatorRecord* record = nullptr;
  if (auto it = transactions_.find(transaction_id); it != transactions_.end()) {
    record = &it->second;
  }
  return ReplayDecision(*decision, record);
}

Status TransactionCoordinator::AdvanceTick(LogicalTick tick) {
  std::lock_guard lock(mutex_);
  Status first_failure = Status::Ok();
  for (const auto& shard : shards_) {
    const auto result = shard->Propose(CommandEnvelope{
        CommandId{0, (static_cast<std::uint64_t>(shard->id()) << 32U) | tick},
        TickCommand{tick}});
    if (!result.status.ok() && first_failure.ok()) {
      first_failure = result.status;
    }
    for (const auto expired : result.expired_transactions) {
      if (auto it = transactions_.find(expired); it != transactions_.end() &&
          it->second.state == TransactionState::kActive) {
        (void)AbortLocked(it->second, "transaction deadline expired");
      }
    }
  }
  return first_failure;
}

}  // namespace spanner_lock
