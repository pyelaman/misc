#pragma once

#include "spanner_prod/types.h"

#include <map>
#include <optional>
#include <set>
#include <vector>

namespace spanner_prod {

enum class AcquireDisposition { kGranted, kWaiting, kAborted };

struct AcquireResult {
  AcquireDisposition disposition{AcquireDisposition::kGranted};
  std::vector<TransactionId> wounded;
  std::size_t queue_position{0};
  std::string detail;
};

// Leader-local lock manager. Active locks are intentionally volatile. Only the
// lock footprints of prepared transactions are reconstructed after failover.
class LockManager {
 public:
  AcquireResult Acquire(const TransactionDescriptor& transaction,
                        const LockRequest& request,
                        std::uint64_t sequence);
  Status ProtectPrepared(TransactionId transaction_id);
  Status RestorePrepared(const PreparedTransaction& prepared);
  void Release(TransactionId transaction_id, bool mark_aborted = false);
  void ResetAndRestore(
      const std::map<TransactionId, PreparedTransaction>& prepared);

  [[nodiscard]] bool Holds(TransactionId transaction_id,
                           const LockRequest& request) const;
  [[nodiscard]] bool HasWaiters(TransactionId transaction_id) const;
  [[nodiscard]] std::vector<LockRequest> HeldLocks(
      TransactionId transaction_id) const;
  [[nodiscard]] bool IsAborted(TransactionId transaction_id) const;
  [[nodiscard]] std::string Fingerprint() const;

 private:
  struct Holder {
    TransactionDescriptor transaction;
    LockMode mode;
  };
  struct Waiter {
    TransactionDescriptor transaction;
    LockMode mode;
    std::uint64_t sequence{0};
  };
  struct Entry {
    std::map<TransactionId, Holder> holders;
    std::vector<Waiter> waiters;
  };

  [[nodiscard]] std::optional<LockMode> HeldMode(TransactionId transaction_id,
                                                 const Cell& cell) const;
  [[nodiscard]] std::vector<Holder> Conflicts(const Entry& entry,
                                               TransactionId requester,
                                               LockMode mode) const;
  void Grant(Entry& entry, const Cell& cell,
             const TransactionDescriptor& transaction, LockMode mode);
  void ProcessQueue(const Cell& cell);

  std::map<Cell, Entry> entries_;
  std::map<TransactionId, std::set<Cell>> held_cells_;
  std::map<TransactionId, std::set<Cell>> waiting_cells_;
  std::set<TransactionId> protected_prepared_;
  std::set<TransactionId> aborted_;
};

}  // namespace spanner_prod

