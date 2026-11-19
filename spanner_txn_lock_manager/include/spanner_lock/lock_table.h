#pragma once

#include "spanner_lock/types.h"

#include <deque>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace spanner_lock {

// Deterministic, single-threaded lock-table state machine. Thread safety and
// replication are supplied by ReplicatedLockShard.
class LockTable {
 public:
  AcquireResult Acquire(const TransactionDescriptor& transaction,
                        const LockRequest& request,
                        std::uint64_t enqueue_sequence);

  std::vector<GrantEvent> ReleaseAll(TransactionId transaction_id);
  std::vector<GrantEvent> Abort(TransactionId transaction_id);
  Status ProtectPrepared(TransactionId transaction_id);

  [[nodiscard]] bool Holds(TransactionId transaction_id,
                           const Cell& cell,
                           LockMode mode) const;
  [[nodiscard]] bool HasWaitingRequests(TransactionId transaction_id) const;
  [[nodiscard]] bool IsAborted(TransactionId transaction_id) const;
  [[nodiscard]] std::vector<TransactionId> Holders(const Cell& cell) const;
  [[nodiscard]] std::string Fingerprint() const;
  [[nodiscard]] const LockMetrics& metrics() const { return metrics_; }

 private:
  struct Holder {
    TransactionDescriptor transaction;
    LockMode mode;
  };

  struct Waiter {
    TransactionDescriptor transaction;
    LockMode mode;
    std::uint64_t sequence;
  };

  struct Entry {
    std::map<TransactionId, Holder> holders;
    std::vector<Waiter> waiters;
  };

  [[nodiscard]] std::optional<LockMode> HeldMode(TransactionId transaction_id,
                                                 const Cell& cell) const;
  [[nodiscard]] std::vector<Holder> ConflictingHolders(
      const Entry& entry, TransactionId requester, LockMode requested) const;
  void Grant(Entry& entry, const Cell& cell,
             const TransactionDescriptor& transaction, LockMode mode);
  void Queue(Entry& entry, const Cell& cell,
             const TransactionDescriptor& transaction, LockMode mode,
             std::uint64_t sequence);
  std::set<Cell> RemoveTransaction(TransactionId transaction_id,
                                   bool mark_aborted);
  std::vector<GrantEvent> ProcessQueues(const std::set<Cell>& changed_cells);
  std::vector<GrantEvent> ProcessQueue(const Cell& cell, Entry& entry);

  std::map<Cell, Entry> entries_;
  std::map<TransactionId, std::set<Cell>> held_cells_;
  std::map<TransactionId, std::set<Cell>> waiting_cells_;
  std::map<TransactionId, TransactionDescriptor> transactions_;
  std::set<TransactionId> aborted_;
  std::set<TransactionId> prepared_protected_;
  LockMetrics metrics_;
};

}  // namespace spanner_lock
