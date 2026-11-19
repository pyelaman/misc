#include "spanner_prod/lock_manager.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace spanner_prod {
namespace {

bool Older(const TransactionDescriptor& lhs,
           const TransactionDescriptor& rhs) {
  return std::tie(lhs.priority, lhs.id) < std::tie(rhs.priority, rhs.id);
}

}  // namespace

std::optional<LockMode> LockManager::HeldMode(TransactionId transaction_id,
                                               const Cell& cell) const {
  const auto entry_it = entries_.find(cell);
  if (entry_it == entries_.end()) {
    return std::nullopt;
  }
  const auto holder_it = entry_it->second.holders.find(transaction_id);
  return holder_it == entry_it->second.holders.end()
             ? std::nullopt
             : std::optional<LockMode>{holder_it->second.mode};
}

std::vector<LockManager::Holder> LockManager::Conflicts(
    const Entry& entry, TransactionId requester, LockMode mode) const {
  std::vector<Holder> conflicts;
  for (const auto& [id, holder] : entry.holders) {
    if (id != requester && !Compatible(holder.mode, mode)) {
      conflicts.push_back(holder);
    }
  }
  return conflicts;
}

void LockManager::Grant(Entry& entry, const Cell& cell,
                        const TransactionDescriptor& transaction,
                        LockMode mode) {
  entry.holders[transaction.id] = Holder{transaction, mode};
  held_cells_[transaction.id].insert(cell);
  if (auto it = waiting_cells_.find(transaction.id);
      it != waiting_cells_.end()) {
    it->second.erase(cell);
    if (it->second.empty()) {
      waiting_cells_.erase(it);
    }
  }
}

AcquireResult LockManager::Acquire(const TransactionDescriptor& transaction,
                                   const LockRequest& request,
                                   std::uint64_t sequence) {
  if (aborted_.contains(transaction.id)) {
    return {AcquireDisposition::kAborted, {}, 0,
            "transaction has already been aborted"};
  }
  LockMode desired = request.mode;
  if (const auto held = HeldMode(transaction.id, request.cell);
      held.has_value()) {
    if (ModeSubsumes(*held, request.mode)) {
      return {AcquireDisposition::kGranted, {}, 0, "lock already held"};
    }
    const auto combined = CombineModes(*held, request.mode);
    if (!combined.has_value()) {
      return {AcquireDisposition::kAborted, {}, 0,
              "unsupported lock upgrade"};
    }
    desired = *combined;
  }

  auto& entry = entries_[request.cell];
  const auto conflicts = Conflicts(entry, transaction.id, desired);
  std::vector<TransactionId> wounded;
  bool must_wait = false;
  for (const auto& holder : conflicts) {
    if (protected_prepared_.contains(holder.transaction.id)) {
      must_wait = true;
    } else if (Older(transaction, holder.transaction)) {
      wounded.push_back(holder.transaction.id);
    } else {
      must_wait = true;
    }
  }
  for (const auto victim : wounded) {
    Release(victim, true);
  }

  auto& refreshed = entries_[request.cell];
  if (!must_wait && Conflicts(refreshed, transaction.id, desired).empty()) {
    Grant(refreshed, request.cell, transaction, desired);
    return {AcquireDisposition::kGranted, std::move(wounded), 0,
            "granted by wound-wait"};
  }

  const auto waiter = std::find_if(
      refreshed.waiters.begin(), refreshed.waiters.end(),
      [&](const Waiter& value) { return value.transaction.id == transaction.id; });
  if (waiter == refreshed.waiters.end()) {
    refreshed.waiters.push_back({transaction, desired, sequence});
  } else {
    const auto combined = CombineModes(waiter->mode, desired);
    if (combined.has_value()) {
      waiter->mode = *combined;
    }
  }
  std::stable_sort(refreshed.waiters.begin(), refreshed.waiters.end(),
                   [](const Waiter& lhs, const Waiter& rhs) {
                     return std::tie(lhs.transaction.priority,
                                     lhs.transaction.id, lhs.sequence) <
                            std::tie(rhs.transaction.priority,
                                     rhs.transaction.id, rhs.sequence);
                   });
  waiting_cells_[transaction.id].insert(request.cell);
  const auto position =
      static_cast<std::size_t>(std::distance(
          refreshed.waiters.begin(),
          std::find_if(refreshed.waiters.begin(), refreshed.waiters.end(),
                       [&](const Waiter& value) {
                         return value.transaction.id == transaction.id;
                       }))) +
      1U;
  return {AcquireDisposition::kWaiting, std::move(wounded), position,
          "waiting for an older or prepared holder"};
}

Status LockManager::ProtectPrepared(TransactionId transaction_id) {
  if (!held_cells_.contains(transaction_id)) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "prepared transaction holds no locks");
  }
  if (HasWaiters(transaction_id)) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "transaction still has waiting locks");
  }
  protected_prepared_.insert(transaction_id);
  return Status::Ok();
}

Status LockManager::RestorePrepared(const PreparedTransaction& prepared) {
  for (const auto& request : prepared.locks) {
    auto& entry = entries_[request.cell];
    if (!Conflicts(entry, prepared.transaction.id, request.mode).empty()) {
      return Status::Error(StatusCode::kCorruption,
                           "prepared lock footprints conflict during recovery");
    }
    Grant(entry, request.cell, prepared.transaction, request.mode);
  }
  protected_prepared_.insert(prepared.transaction.id);
  return Status::Ok();
}

void LockManager::Release(TransactionId transaction_id, bool mark_aborted) {
  std::set<Cell> changed;
  if (const auto it = held_cells_.find(transaction_id);
      it != held_cells_.end()) {
    changed.insert(it->second.begin(), it->second.end());
    for (const auto& cell : it->second) {
      entries_[cell].holders.erase(transaction_id);
    }
    held_cells_.erase(it);
  }
  if (const auto it = waiting_cells_.find(transaction_id);
      it != waiting_cells_.end()) {
    changed.insert(it->second.begin(), it->second.end());
    for (const auto& cell : it->second) {
      auto& waiters = entries_[cell].waiters;
      std::erase_if(waiters, [transaction_id](const Waiter& waiter) {
        return waiter.transaction.id == transaction_id;
      });
    }
    waiting_cells_.erase(it);
  }
  protected_prepared_.erase(transaction_id);
  if (mark_aborted) {
    aborted_.insert(transaction_id);
  }
  for (const auto& cell : changed) {
    ProcessQueue(cell);
  }
}

void LockManager::ProcessQueue(const Cell& cell) {
  const auto entry_it = entries_.find(cell);
  if (entry_it == entries_.end()) {
    return;
  }
  auto& entry = entry_it->second;
  while (!entry.waiters.empty()) {
    const Waiter waiter = entry.waiters.front();
    if (aborted_.contains(waiter.transaction.id)) {
      entry.waiters.erase(entry.waiters.begin());
      continue;
    }
    if (!Conflicts(entry, waiter.transaction.id, waiter.mode).empty()) {
      break;
    }
    entry.waiters.erase(entry.waiters.begin());
    Grant(entry, cell, waiter.transaction, waiter.mode);
  }
  if (entry.holders.empty() && entry.waiters.empty()) {
    entries_.erase(entry_it);
  }
}

void LockManager::ResetAndRestore(
    const std::map<TransactionId, PreparedTransaction>& prepared) {
  entries_.clear();
  held_cells_.clear();
  waiting_cells_.clear();
  protected_prepared_.clear();
  aborted_.clear();
  for (const auto& [id, transaction] : prepared) {
    (void)id;
    const Status status = RestorePrepared(transaction);
    if (!status.ok()) {
      throw std::logic_error(status.message);
    }
  }
}

bool LockManager::Holds(TransactionId transaction_id,
                        const LockRequest& request) const {
  const auto mode = HeldMode(transaction_id, request.cell);
  return mode.has_value() && ModeSubsumes(*mode, request.mode);
}

bool LockManager::HasWaiters(TransactionId transaction_id) const {
  return waiting_cells_.contains(transaction_id);
}

std::vector<LockRequest> LockManager::HeldLocks(
    TransactionId transaction_id) const {
  std::vector<LockRequest> result;
  if (const auto it = held_cells_.find(transaction_id);
      it != held_cells_.end()) {
    for (const auto& cell : it->second) {
      result.push_back({cell, *HeldMode(transaction_id, cell)});
    }
  }
  return result;
}

bool LockManager::IsAborted(TransactionId transaction_id) const {
  return aborted_.contains(transaction_id);
}

std::string LockManager::Fingerprint() const {
  std::ostringstream out;
  for (const auto& [cell, entry] : entries_) {
    out << cell.ToString() << '{';
    for (const auto& [id, holder] : entry.holders) {
      out << "H" << id << ':' << ToString(holder.mode) << ';';
    }
    for (const auto& waiter : entry.waiters) {
      out << "W" << waiter.transaction.id << ':' << ToString(waiter.mode)
          << ';';
    }
    out << '}';
  }
  return out.str();
}

}  // namespace spanner_prod
