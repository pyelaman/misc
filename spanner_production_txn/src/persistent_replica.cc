#include "spanner_prod/persistent_replica.h"

#include "spanner_prod/codec.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

namespace spanner_prod {
namespace {

constexpr std::uint32_t kWalMagic = 0x5350574cU;  // SPWL
constexpr std::uint8_t kEntryRecord = 1;
constexpr std::uint8_t kCommitRecord = 2;
constexpr std::size_t kHeaderSize = 13;

void Put32(std::string& output, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) {
    output.push_back(static_cast<char>((value >> (i * 8U)) & 0xffU));
  }
}

void Put64(std::string& output, std::uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) {
    output.push_back(static_cast<char>((value >> (i * 8U)) & 0xffU));
  }
}

std::uint32_t Get32(std::string_view bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(
                 static_cast<unsigned char>(bytes[offset + i]))
             << (i * 8U);
  }
  return value;
}

std::uint64_t Get64(std::string_view bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(
                 static_cast<unsigned char>(bytes[offset + i]))
             << (i * 8U);
  }
  return value;
}

Status WriteAll(int fd, std::string_view bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written =
        ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status::Error(StatusCode::kUnavailable,
                           "WAL write failed: " +
                               std::string(std::strerror(errno)));
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(fd) != 0) {
    return Status::Error(StatusCode::kUnavailable,
                         "WAL fsync failed: " +
                             std::string(std::strerror(errno)));
  }
  return Status::Ok();
}

std::string EncodeIndex(LogIndex index) {
  std::string bytes;
  Put64(bytes, index);
  return bytes;
}

}  // namespace

PersistentReplica::PersistentReplica(std::filesystem::path directory)
    : directory_(std::move(directory)), wal_path_(directory_ / "replica.wal") {
  std::filesystem::create_directories(directory_);
  wal_fd_ = ::open(wal_path_.c_str(), O_CREAT | O_RDWR | O_APPEND, 0640);
  if (wal_fd_ < 0) {
    throw std::runtime_error("cannot open replica WAL: " +
                             std::string(std::strerror(errno)));
  }
  const Status recovered = Recover();
  if (!recovered.ok()) {
    ::close(wal_fd_);
    wal_fd_ = -1;
    throw std::runtime_error(recovered.message);
  }
}

PersistentReplica::~PersistentReplica() {
  if (wal_fd_ >= 0) {
    ::close(wal_fd_);
  }
}

Status PersistentReplica::AppendRecord(std::uint8_t type,
                                       std::string_view payload) {
  if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "WAL record is too large");
  }
  std::string record;
  record.reserve(kHeaderSize + payload.size());
  Put32(record, kWalMagic);
  record.push_back(static_cast<char>(type));
  Put32(record, static_cast<std::uint32_t>(payload.size()));
  Put32(record, Checksum(payload));
  record.append(payload);
  return WriteAll(wal_fd_, record);
}

Status PersistentReplica::Append(const LogEntry& entry) {
  if (entry.index <= log_.size()) {
    return log_[entry.index - 1] == entry
               ? Status::Ok()
               : Status::Error(StatusCode::kFailedPrecondition,
                               "log index already contains another entry");
  }
  if (entry.index != log_.size() + 1U) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "replica log append has a gap");
  }
  const std::string payload = EncodeLogEntry(entry);
  if (const Status written = AppendRecord(kEntryRecord, payload);
      !written.ok()) {
    return written;
  }
  log_.push_back(entry);
  return Status::Ok();
}

StatusOr<ApplyResult> PersistentReplica::CommitThrough(LogIndex index) {
  if (index > log_.size()) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "cannot commit beyond replica log");
  }
  if (index <= committed_index_) {
    return ApplyResult{Status::Ok(), 0};
  }
  if (const Status persisted =
          AppendRecord(kCommitRecord, EncodeIndex(index));
      !persisted.ok()) {
    return persisted;
  }
  ApplyResult last{Status::Ok(), 0};
  while (state_.last_applied() < index) {
    last = state_.Apply(log_[state_.last_applied()]);
  }
  committed_index_ = index;
  return last;
}

Status PersistentReplica::Recover() {
  std::ifstream input(wal_path_, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(input)),
                    std::istreambuf_iterator<char>());
  std::size_t offset = 0;
  LogIndex durable_commit = 0;
  while (offset < bytes.size()) {
    if (bytes.size() - offset < kHeaderSize) {
      if (::ftruncate(wal_fd_, static_cast<off_t>(offset)) != 0) {
        return Status::Error(StatusCode::kUnavailable,
                             "failed to truncate partial WAL tail");
      }
      break;
    }
    const std::uint32_t magic = Get32(bytes, offset);
    const auto type = static_cast<std::uint8_t>(bytes[offset + 4]);
    const std::uint32_t length = Get32(bytes, offset + 5);
    const std::uint32_t checksum = Get32(bytes, offset + 9);
    if (magic != kWalMagic) {
      return Status::Error(StatusCode::kCorruption, "invalid WAL magic");
    }
    if (bytes.size() - offset - kHeaderSize < length) {
      if (::ftruncate(wal_fd_, static_cast<off_t>(offset)) != 0) {
        return Status::Error(StatusCode::kUnavailable,
                             "failed to truncate partial WAL payload");
      }
      break;
    }
    const std::string_view payload(bytes.data() + offset + kHeaderSize,
                                   length);
    if (Checksum(payload) != checksum) {
      return Status::Error(StatusCode::kCorruption,
                           "WAL checksum mismatch");
    }
    if (type == kEntryRecord) {
      auto decoded = DecodeLogEntry(payload);
      if (!decoded.ok()) {
        return decoded.status();
      }
      if (decoded.value().index != log_.size() + 1U) {
        return Status::Error(StatusCode::kCorruption,
                             "WAL log indexes are not contiguous");
      }
      log_.push_back(std::move(decoded.value()));
    } else if (type == kCommitRecord) {
      if (payload.size() != sizeof(std::uint64_t)) {
        return Status::Error(StatusCode::kCorruption,
                             "invalid WAL commit record");
      }
      durable_commit = std::max(durable_commit, Get64(payload, 0));
    } else {
      return Status::Error(StatusCode::kCorruption,
                           "unknown WAL record type");
    }
    offset += kHeaderSize + length;
  }
  if (durable_commit > log_.size()) {
    return Status::Error(StatusCode::kCorruption,
                         "WAL commit index exceeds log length");
  }
  while (state_.last_applied() < durable_commit) {
    (void)state_.Apply(log_[state_.last_applied()]);
  }
  committed_index_ = durable_commit;
  return Status::Ok();
}

std::string PersistentReplica::Fingerprint() const {
  return "commit=" + std::to_string(committed_index_) + ";" +
         state_.Fingerprint();
}

PersistentConsensusGroup::PersistentConsensusGroup(
    SplitId split_id, std::filesystem::path root, std::size_t replica_count)
    : split_id_(split_id), root_(std::move(root)), live_(replica_count, true) {
  if (replica_count == 0 || replica_count % 2 == 0) {
    throw std::invalid_argument("replica count must be a nonzero odd number");
  }
  std::filesystem::create_directories(root_);
  for (std::size_t i = 0; i < replica_count; ++i) {
    replicas_.push_back(std::make_unique<PersistentReplica>(
        root_ / ("replica-" + std::to_string(i))));
  }
  const Status recovered = RecoverCanonicalLog();
  if (!recovered.ok()) {
    throw std::runtime_error(recovered.message);
  }
}

Status PersistentConsensusGroup::RecoverCanonicalLog() {
  std::map<std::string, std::pair<std::size_t, LogEntry>> votes;
  std::size_t index = 0;
  while (true) {
    votes.clear();
    for (const auto& replica : replicas_) {
      if (replica->log().size() <= index) {
        continue;
      }
      const auto& entry = replica->log()[index];
      const std::string encoded = EncodeLogEntry(entry);
      auto& vote = votes[encoded];
      ++vote.first;
      vote.second = entry;
    }
    const auto winner = std::find_if(
        votes.begin(), votes.end(), [&](const auto& value) {
          return value.second.first >= quorum_size();
        });
    if (winner == votes.end()) {
      break;
    }
    canonical_log_.push_back(winner->second.second);
    term_ = std::max(term_, winner->second.second.term);
    ++index;
  }
  committed_index_ = static_cast<LogIndex>(canonical_log_.size());
  for (std::size_t i = 0; i < replicas_.size(); ++i) {
    const Status caught_up = CatchUp(i);
    if (!caught_up.ok()) {
      return caught_up;
    }
  }
  return Status::Ok();
}

Status PersistentConsensusGroup::CatchUp(std::size_t replica) {
  for (std::size_t i = replicas_[replica]->log().size();
       i < canonical_log_.size(); ++i) {
    if (const Status appended = replicas_[replica]->Append(canonical_log_[i]);
        !appended.ok()) {
      return appended;
    }
  }
  auto committed = replicas_[replica]->CommitThrough(committed_index_);
  return committed.ok() ? Status::Ok() : committed.status();
}

ApplyResult PersistentConsensusGroup::Propose(
    std::uint64_t operation_id, ReplicatedCommand command) {
  std::lock_guard lock(mutex_);
  const auto live_count =
      static_cast<std::size_t>(std::count(live_.begin(), live_.end(), true));
  if (!live_[leader_] || live_count < quorum_size()) {
    return {Status::Error(StatusCode::kUnavailable,
                          "split group has no live leader quorum"),
            0};
  }
  if (committed_index_ != canonical_log_.size()) {
    return {Status::Error(StatusCode::kInDoubt,
                          "previous quorum append awaits recovery"),
            0};
  }
  LogEntry entry{term_, static_cast<LogIndex>(canonical_log_.size()) + 1U,
                 operation_id, std::move(command)};
  std::size_t appended = 0;
  for (std::size_t i = 0; i < replicas_.size(); ++i) {
    if (!live_[i]) {
      continue;
    }
    if (replicas_[i]->Append(entry).ok()) {
      ++appended;
    }
  }
  if (appended < quorum_size()) {
    return {Status::Error(StatusCode::kUnavailable,
                          "command did not reach a durable quorum"),
            0};
  }
  canonical_log_.push_back(entry);
  if (fail_after_quorum_once_) {
    fail_after_quorum_once_ = false;
    return {Status::Error(StatusCode::kInDoubt,
                          "entry reached quorum before injected leader loss"),
            0};
  }

  std::optional<ApplyResult> canonical;
  std::size_t committed = 0;
  for (std::size_t i = 0; i < replicas_.size(); ++i) {
    if (!live_[i]) {
      continue;
    }
    auto result = replicas_[i]->CommitThrough(entry.index);
    if (!result.ok()) {
      continue;
    }
    ++committed;
    if (!canonical.has_value()) {
      canonical = result.value();
    } else if (*canonical != result.value()) {
      throw std::logic_error("replicas applied different command results");
    }
  }
  if (committed < quorum_size()) {
    return {Status::Error(StatusCode::kInDoubt,
                          "quorum append committed but apply acknowledgement failed"),
            0};
  }
  committed_index_ = entry.index;
  return *canonical;
}

Status PersistentConsensusGroup::RecoverPending() {
  std::lock_guard lock(mutex_);
  if (canonical_log_.empty() || committed_index_ == canonical_log_.size()) {
    return Status::Ok();
  }
  const LogIndex target = static_cast<LogIndex>(canonical_log_.size());
  std::size_t committed = 0;
  for (std::size_t i = 0; i < replicas_.size(); ++i) {
    if (!live_[i]) {
      continue;
    }
    auto result = replicas_[i]->CommitThrough(target);
    if (result.ok()) {
      ++committed;
    }
  }
  if (committed < quorum_size()) {
    return Status::Error(StatusCode::kUnavailable,
                         "cannot recover pending entry without quorum");
  }
  committed_index_ = target;
  return Status::Ok();
}

Status PersistentConsensusGroup::SetReplicaLive(std::size_t replica,
                                                 bool live) {
  std::lock_guard lock(mutex_);
  if (replica >= replicas_.size()) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "replica index is out of range");
  }
  if (live) {
    const Status caught_up = CatchUp(replica);
    if (!caught_up.ok()) {
      return caught_up;
    }
  }
  live_[replica] = live;
  return Status::Ok();
}

Status PersistentConsensusGroup::ElectLeader(std::size_t replica) {
  std::lock_guard lock(mutex_);
  if (replica >= replicas_.size() || !live_[replica]) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "leader candidate is not live");
  }
  if (static_cast<std::size_t>(
          std::count(live_.begin(), live_.end(), true)) < quorum_size()) {
    return Status::Error(StatusCode::kUnavailable,
                         "cannot elect leader without quorum");
  }
  ++term_;
  leader_ = replica;
  return CatchUp(replica);
}

void PersistentConsensusGroup::FailAfterQuorumAppendOnce() {
  std::lock_guard lock(mutex_);
  fail_after_quorum_once_ = true;
}

Term PersistentConsensusGroup::term() const {
  std::lock_guard lock(mutex_);
  return term_;
}

std::size_t PersistentConsensusGroup::quorum_size() const {
  return replicas_.size() / 2U + 1U;
}

std::size_t PersistentConsensusGroup::leader() const {
  std::lock_guard lock(mutex_);
  return leader_;
}

LogIndex PersistentConsensusGroup::last_log_index() const {
  std::lock_guard lock(mutex_);
  return static_cast<LogIndex>(canonical_log_.size());
}

const SplitStateMachine& PersistentConsensusGroup::LeaderState() const {
  return replicas_[leader_]->state();
}

std::string PersistentConsensusGroup::Fingerprint() const {
  std::lock_guard lock(mutex_);
  std::ostringstream out;
  out << "split=" << split_id_ << ";term=" << term_ << ";leader=" << leader_
      << ";commit=" << committed_index_ << ';';
  std::optional<std::string> canonical;
  for (std::size_t i = 0; i < replicas_.size(); ++i) {
    if (!live_[i]) {
      continue;
    }
    const auto fingerprint = replicas_[i]->Fingerprint();
    if (!canonical.has_value()) {
      canonical = fingerprint;
    } else if (*canonical != fingerprint) {
      out << "DIVERGED;";
    }
  }
  return out.str() + canonical.value_or("no-live-replica");
}

std::map<TransactionId, PreparedTransaction>
PersistentConsensusGroup::Prepared() const {
  std::lock_guard lock(mutex_);
  return LeaderState().prepared();
}

std::optional<TransactionDecision> PersistentConsensusGroup::Decision(
    TransactionId transaction_id) const {
  std::lock_guard lock(mutex_);
  return LeaderState().Decision(transaction_id);
}

Timestamp PersistentConsensusGroup::safe_time() const {
  std::lock_guard lock(mutex_);
  return LeaderState().safe_time();
}

Timestamp PersistentConsensusGroup::last_assigned_timestamp() const {
  std::lock_guard lock(mutex_);
  return LeaderState().last_assigned_timestamp();
}

StatusOr<std::optional<std::string>> PersistentConsensusGroup::ReadAt(
    const Cell& cell, Timestamp timestamp) const {
  std::lock_guard lock(mutex_);
  return LeaderState().ReadAt(cell, timestamp);
}

}  // namespace spanner_prod
