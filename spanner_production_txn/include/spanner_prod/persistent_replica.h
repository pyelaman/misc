#pragma once

#include "spanner_prod/split_state_machine.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace spanner_prod {

// One durable replica. The WAL contains command records followed by explicit
// commit-index records. Only committed entries are applied during recovery.
class PersistentReplica {
 public:
  explicit PersistentReplica(std::filesystem::path directory);
  ~PersistentReplica();

  PersistentReplica(const PersistentReplica&) = delete;
  PersistentReplica& operator=(const PersistentReplica&) = delete;

  Status Append(const LogEntry& entry);
  StatusOr<ApplyResult> CommitThrough(LogIndex index);

  [[nodiscard]] LogIndex committed_index() const { return committed_index_; }
  [[nodiscard]] const std::vector<LogEntry>& log() const { return log_; }
  [[nodiscard]] const SplitStateMachine& state() const { return state_; }
  [[nodiscard]] std::string Fingerprint() const;

 private:
  Status Recover();
  Status AppendRecord(std::uint8_t type, std::string_view payload);

  std::filesystem::path directory_;
  std::filesystem::path wal_path_;
  int wal_fd_{-1};
  std::vector<LogEntry> log_;
  LogIndex committed_index_{0};
  SplitStateMachine state_;
};

// Persistent in-process consensus harness. It models the integration contract
// and crash boundaries of a per-split Paxos/Raft group. It is intentionally not
// a networked consensus implementation.
class PersistentConsensusGroup {
 public:
  PersistentConsensusGroup(SplitId split_id, std::filesystem::path root,
                           std::size_t replica_count = 3);

  ApplyResult Propose(std::uint64_t operation_id,
                      ReplicatedCommand command);
  Status SetReplicaLive(std::size_t replica, bool live);
  Status ElectLeader(std::size_t replica);
  void FailAfterQuorumAppendOnce();
  Status RecoverPending();

  [[nodiscard]] SplitId split_id() const { return split_id_; }
  [[nodiscard]] Term term() const;
  [[nodiscard]] std::size_t quorum_size() const;
  [[nodiscard]] std::size_t leader() const;
  [[nodiscard]] LogIndex last_log_index() const;
  [[nodiscard]] std::string Fingerprint() const;
  [[nodiscard]] std::map<TransactionId, PreparedTransaction> Prepared() const;
  [[nodiscard]] std::optional<TransactionDecision> Decision(
      TransactionId transaction_id) const;
  [[nodiscard]] Timestamp safe_time() const;
  [[nodiscard]] Timestamp last_assigned_timestamp() const;
  [[nodiscard]] StatusOr<std::optional<std::string>> ReadAt(
      const Cell& cell, Timestamp timestamp) const;

 private:
  Status CatchUp(std::size_t replica);
  Status RecoverCanonicalLog();
  [[nodiscard]] const SplitStateMachine& LeaderState() const;

  SplitId split_id_;
  std::filesystem::path root_;
  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<PersistentReplica>> replicas_;
  std::vector<bool> live_;
  std::vector<LogEntry> canonical_log_;
  LogIndex committed_index_{0};
  Term term_{1};
  std::size_t leader_{0};
  bool fail_after_quorum_once_{false};
};

}  // namespace spanner_prod
