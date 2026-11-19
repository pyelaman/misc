#include "spanner_lock/replication.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace spanner_lock {

InMemoryReplicatedShard::InMemoryReplicatedShard(ShardId id,
                                                 std::size_t replica_count)
    : id_(id), replicas_(replica_count), live_(replica_count, true) {
  if (replica_count == 0 || replica_count % 2 == 0) {
    throw std::invalid_argument("replica count must be a nonzero odd number");
  }
}

ShardResult InMemoryReplicatedShard::Propose(
    const CommandEnvelope& envelope) {
  std::lock_guard lock(mutex_);
  if (fail_next_proposal_) {
    fail_next_proposal_ = false;
    return ShardResult{Status::Error(StatusCode::kUnavailable,
                                     "injected proposal failure")};
  }
  if (static_cast<std::size_t>(
          std::count(live_.begin(), live_.end(), true)) < quorum_size()) {
    return ShardResult{Status::Error(StatusCode::kUnavailable,
                                     "replica group has no quorum")};
  }

  const LogIndex index = static_cast<LogIndex>(log_.size()) + 1U;
  log_.push_back(LogEntry{index, envelope});
  std::optional<ShardResult> canonical;
  for (std::size_t i = 0; i < replicas_.size(); ++i) {
    if (!live_[i]) {
      continue;
    }
    ShardResult result = replicas_[i].Apply(index, envelope);
    if (!canonical.has_value()) {
      canonical = result;
    } else if (result != *canonical) {
      throw std::logic_error("replicas produced different command results");
    }
  }
  return *canonical;
}

Status InMemoryReplicatedShard::SetReplicaLive(std::size_t replica, bool live) {
  std::lock_guard lock(mutex_);
  if (replica >= replicas_.size()) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "replica index is out of range");
  }
  live_[replica] = live;
  if (live) {
    CatchUp(replica);
  }
  return Status::Ok();
}

void InMemoryReplicatedShard::CatchUp(std::size_t replica) {
  auto& state_machine = replicas_[replica];
  for (std::size_t i = static_cast<std::size_t>(state_machine.last_applied());
       i < log_.size(); ++i) {
    state_machine.Apply(log_[i].index, log_[i].envelope);
  }
}

void InMemoryReplicatedShard::FailNextProposal() {
  std::lock_guard lock(mutex_);
  fail_next_proposal_ = true;
}

std::size_t InMemoryReplicatedShard::live_replicas() const {
  std::lock_guard lock(mutex_);
  return static_cast<std::size_t>(
      std::count(live_.begin(), live_.end(), true));
}

std::size_t InMemoryReplicatedShard::quorum_size() const {
  return replicas_.size() / 2U + 1U;
}

std::size_t InMemoryReplicatedShard::log_size() const {
  std::lock_guard lock(mutex_);
  return log_.size();
}

std::string InMemoryReplicatedShard::Fingerprint() const {
  std::lock_guard lock(mutex_);
  std::ostringstream out;
  out << "shard=" << id_ << ";log=" << log_.size() << ";";
  std::optional<std::string> canonical;
  for (std::size_t i = 0; i < replicas_.size(); ++i) {
    if (!live_[i]) {
      continue;
    }
    const auto fingerprint = replicas_[i].Fingerprint();
    if (!canonical.has_value()) {
      canonical = fingerprint;
    } else if (*canonical != fingerprint) {
      out << "DIVERGED;";
    }
  }
  out << canonical.value_or("no-live-replica");
  return out.str();
}

}  // namespace spanner_lock
