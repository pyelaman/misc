#include "spanner_lock/lock_table.h"

#include <algorithm>
#include <sstream>
#include <tuple>

namespace spanner_lock {
namespace {

bool OlderThan(const TransactionDescriptor& lhs,
               const TransactionDescriptor& rhs) {
  return std::tie(lhs.age, lhs.id) < std::tie(rhs.age, rhs.id);
}

}  // namespace

std::optional<LockMode> LockTable::HeldMode(TransactionId transaction_id,
                                            const Cell& cell) const {
  const auto entry_it = entries_.find(cell);
  if (entry_it == entries_.end()) {
    return std::nullopt;
  }
  const auto holder_it = entry_it->second.holders.find(transaction_id);
  if (holder_it == entry_it->second.holders.end()) {
    return std::nullopt;
  }
  return holder_it->second.mode;
}

std::vector<LockTable::Holder> LockTable::ConflictingHolders(
    const Entry& entry, TransactionId requester, LockMode requested) const {
  std::vector<Holder> conflicts;
  for (const auto& [holder_id, holder] : entry.holders) {
    if (holder_id != requester && !Compatible(holder.mode, requested)) {
      conflicts.push_back(holder);
    }
  }
  return conflicts;
}

void LockTable::Grant(Entry& entry, const Cell& cell,
                      const TransactionDescriptor& transaction,
                      LockMode mode) {
  entry.holders[transaction.id] = Holder{transaction, mode};
  held_cells_[transaction.id].insert(cell);
  auto waiting_it = waiting_cells_.find(transaction.id);
  if (waiting_it != waiting_cells_.end()) {
    waiting_it->second.erase(cell);
    if (waiting_it->second.empty()) {
      waiting_cells_.erase(waiting_it);
    }
  }
}

void LockTable::Queue(Entry& entry, const Cell& cell,
                      const TransactionDescriptor& transaction,
                      LockMode mode, std::uint64_t sequence) {
  const auto existing = std::find_if(
      entry.waiters.begin(), entry.waiters.end(), [&](const Waiter& waiter) {
        return waiter.transaction.id == transaction.id;
      });
  if (existing == entry.waiters.end()) {
    entry.waiters.push_back(Waiter{transaction, mode, sequence});
  } else {
    const auto combined = CombineModes(existing->mode, mode);
    if (combined.has_value()) {
      existing->mode = *combined;
    }
  }
  std::stable_sort(entry.waiters.begin(), entry.waiters.end(),
                   [](const Waiter& lhs, const Waiter& rhs) {
                     return std::tie(lhs.transaction.age, lhs.transaction.id,
                                     lhs.sequence) <
                            std::tie(rhs.transaction.age, rhs.transaction.id,
                                     rhs.sequence);
                   });
  waiting_cells_[transaction.id].insert(cell);
}

AcquireResult LockTable::Acquire(const TransactionDescriptor& transaction,
                                 const LockRequest& request,
                                 std::uint64_t enqueue_sequence) {
  ++metrics_.acquire_requests;
  transactions_[transaction.id] = transaction;
  if (aborted_.contains(transaction.id)) {
    return AcquireResult{AcquireDisposition::kAborted, {}, {}, 0,
                         "transaction was already wounded or aborted"};
  }

  auto desired = request.mode;
  if (const auto held = HeldMode(transaction.id, request.cell);
      held.has_value()) {
    if (ModeSubsumes(*held, request.mode)) {
      ++metrics_.immediate_grants;
      return AcquireResult{AcquireDisposition::kGranted, {}, {}, 0,
                           "lock already held"};
    }
    const auto combined = CombineModes(*held, request.mode);
    if (!combined.has_value()) {
      return AcquireResult{AcquireDisposition::kAborted, {}, {}, 0,
                           "invalid lock-mode combination"};
    }
    desired = *combined;
  }

  auto& initial_entry = entries_[request.cell];
  const auto conflicts =
      ConflictingHolders(initial_entry, transaction.id, desired);

  std::vector<TransactionId> wounded;
  std::set<Cell> cells_changed_by_wounds;
  bool blocked_by_older = false;
  for (const Holder& holder : conflicts) {
    // A prepared participant may already have a durable commit decision at its
    // coordinator. It is no longer a legal wound victim; requesters wait until
    // the decision releases its locks.
    if (prepared_protected_.contains(holder.transaction.id)) {
      blocked_by_older = true;
    } else if (OlderThan(transaction, holder.transaction)) {
      wounded.push_back(holder.transaction.id);
      const auto changed = RemoveTransaction(holder.transaction.id, true);
      cells_changed_by_wounds.insert(changed.begin(), changed.end());
      ++metrics_.wounds;
    } else {
      blocked_by_older = true;
    }
  }

  auto& entry = entries_[request.cell];
  const auto remaining =
      ConflictingHolders(entry, transaction.id, desired);
  if (!blocked_by_older && remaining.empty()) {
    Grant(entry, request.cell, transaction, desired);
    auto asynchronous_grants = ProcessQueues(cells_changed_by_wounds);
    ++metrics_.immediate_grants;
    return AcquireResult{AcquireDisposition::kGranted, std::move(wounded),
                         std::move(asynchronous_grants), 0,
                         "granted under wound-wait"};
  }

  Queue(entry, request.cell, transaction, desired, enqueue_sequence);
  auto asynchronous_grants = ProcessQueues(cells_changed_by_wounds);
  ++metrics_.queued_requests;
  const auto waiter_it = std::find_if(
      entry.waiters.begin(), entry.waiters.end(), [&](const Waiter& waiter) {
        return waiter.transaction.id == transaction.id;
      });
  const auto position =
      static_cast<std::size_t>(std::distance(entry.waiters.begin(), waiter_it)) +
      1U;
  return AcquireResult{AcquireDisposition::kWaiting, std::move(wounded),
                       std::move(asynchronous_grants), position,
                       "waiting behind an older conflicting transaction"};
}

std::set<Cell> LockTable::RemoveTransaction(TransactionId transaction_id,
                                            bool mark_aborted) {
  if (mark_aborted) {
    aborted_.insert(transaction_id);
  }
  std::set<Cell> changed;
  if (const auto held_it = held_cells_.find(transaction_id);
      held_it != held_cells_.end()) {
    changed.insert(held_it->second.begin(), held_it->second.end());
    for (const Cell& cell : held_it->second) {
      entries_[cell].holders.erase(transaction_id);
      ++metrics_.releases;
    }
    held_cells_.erase(held_it);
  }
  if (const auto waiting_it = waiting_cells_.find(transaction_id);
      waiting_it != waiting_cells_.end()) {
    changed.insert(waiting_it->second.begin(), waiting_it->second.end());
    for (const Cell& cell : waiting_it->second) {
      auto& waiters = entries_[cell].waiters;
      std::erase_if(waiters, [&](const Waiter& waiter) {
        return waiter.transaction.id == transaction_id;
      });
    }
    waiting_cells_.erase(waiting_it);
  }
  transactions_.erase(transaction_id);
  prepared_protected_.erase(transaction_id);
  return changed;
}

Status LockTable::ProtectPrepared(TransactionId transaction_id) {
  if (aborted_.contains(transaction_id)) {
    return Status::Error(StatusCode::kAborted,
                         "aborted transaction cannot be protected");
  }
  if (!transactions_.contains(transaction_id)) {
    return Status::Error(StatusCode::kNotFound,
                         "transaction has no lock-table state");
  }
  prepared_protected_.insert(transaction_id);
  return Status::Ok();
}

std::vector<GrantEvent> LockTable::Abort(TransactionId transaction_id) {
  return ProcessQueues(RemoveTransaction(transaction_id, true));
}

std::vector<GrantEvent> LockTable::ReleaseAll(TransactionId transaction_id) {
  return ProcessQueues(RemoveTransaction(transaction_id, false));
}

std::vector<GrantEvent> LockTable::ProcessQueues(
    const std::set<Cell>& changed_cells) {
  std::vector<GrantEvent> grants;
  for (const Cell& cell : changed_cells) {
    const auto entry_it = entries_.find(cell);
    if (entry_it == entries_.end()) {
      continue;
    }
    auto cell_grants = ProcessQueue(cell, entry_it->second);
    grants.insert(grants.end(), cell_grants.begin(), cell_grants.end());
    if (entry_it->second.holders.empty() &&
        entry_it->second.waiters.empty()) {
      entries_.erase(entry_it);
    }
  }
  return grants;
}

std::vector<GrantEvent> LockTable::ProcessQueue(const Cell& cell,
                                                Entry& entry) {
  std::vector<GrantEvent> grants;
  while (!entry.waiters.empty()) {
    const Waiter waiter = entry.waiters.front();
    if (aborted_.contains(waiter.transaction.id)) {
      entry.waiters.erase(entry.waiters.begin());
      continue;
    }
    if (!ConflictingHolders(entry, waiter.transaction.id, waiter.mode).empty()) {
      break;
    }
    entry.waiters.erase(entry.waiters.begin());
    Grant(entry, cell, waiter.transaction, waiter.mode);
    grants.push_back(GrantEvent{waiter.transaction.id, cell, waiter.mode});
  }
  return grants;
}

bool LockTable::Holds(TransactionId transaction_id, const Cell& cell,
                      LockMode mode) const {
  const auto held = HeldMode(transaction_id, cell);
  return held.has_value() && ModeSubsumes(*held, mode);
}

bool LockTable::HasWaitingRequests(TransactionId transaction_id) const {
  const auto it = waiting_cells_.find(transaction_id);
  return it != waiting_cells_.end() && !it->second.empty();
}

bool LockTable::IsAborted(TransactionId transaction_id) const {
  return aborted_.contains(transaction_id);
}

std::vector<TransactionId> LockTable::Holders(const Cell& cell) const {
  std::vector<TransactionId> holders;
  if (const auto it = entries_.find(cell); it != entries_.end()) {
    for (const auto& [transaction_id, holder] : it->second.holders) {
      (void)holder;
      holders.push_back(transaction_id);
    }
  }
  return holders;
}

std::string LockTable::Fingerprint() const {
  std::ostringstream out;
  for (const auto& [cell, entry] : entries_) {
    if (entry.holders.empty() && entry.waiters.empty()) {
      continue;
    }
    out << cell.ToString() << "{";
    for (const auto& [id, holder] : entry.holders) {
      out << "H:" << id << ":" << ToString(holder.mode) << ";";
    }
    for (const auto& waiter : entry.waiters) {
      out << "W:" << waiter.transaction.id << ":" << ToString(waiter.mode)
          << ":" << waiter.sequence << ";";
    }
    out << "}";
  }
  out << "protected=";
  for (const auto transaction_id : prepared_protected_) {
    out << transaction_id << ',';
  }
  return out.str();
}

}  // namespace spanner_lock
