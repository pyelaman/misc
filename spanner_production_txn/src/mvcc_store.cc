#include "spanner_prod/mvcc_store.h"

#include <sstream>

namespace spanner_prod {

Status MvccStore::Apply(Timestamp commit_timestamp,
                        const std::vector<Mutation>& mutations) {
  if (commit_timestamp <= 0) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "commit timestamp must be positive");
  }
  // Validate the entire write batch before changing the store. Replicated
  // apply must be atomic: a conflict in the final mutation cannot leave the
  // earlier mutations visible on only some replicas.
  std::map<Cell, std::optional<std::string>> batch;
  for (const auto& mutation : mutations) {
    const auto [batch_it, inserted] =
        batch.emplace(mutation.cell, mutation.value);
    if (!inserted && batch_it->second != mutation.value) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "conflicting values for one cell in an MVCC batch");
    }
    const auto cell_it = versions_.find(mutation.cell);
    if (cell_it == versions_.end()) {
      continue;
    }
    const auto version_it = cell_it->second.find(commit_timestamp);
    if (version_it != cell_it->second.end() &&
        version_it->second != mutation.value) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "different values at the same MVCC timestamp");
    }
  }
  for (const auto& [cell, value] : batch) {
    versions_[cell].emplace(commit_timestamp, value);
  }
  return Status::Ok();
}

StatusOr<std::optional<std::string>> MvccStore::Read(
    const Cell& cell, Timestamp read_timestamp) const {
  if (read_timestamp < 0) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "read timestamp cannot be negative");
  }
  const auto cell_it = versions_.find(cell);
  if (cell_it == versions_.end()) {
    return std::optional<std::string>{};
  }
  const auto version_it = cell_it->second.upper_bound(read_timestamp);
  if (version_it == cell_it->second.begin()) {
    return std::optional<std::string>{};
  }
  return std::prev(version_it)->second;
}

std::vector<VersionedValue> MvccStore::Versions(const Cell& cell) const {
  std::vector<VersionedValue> result;
  if (const auto it = versions_.find(cell); it != versions_.end()) {
    for (const auto& [timestamp, value] : it->second) {
      result.push_back({timestamp, value});
    }
  }
  return result;
}

std::string MvccStore::Fingerprint() const {
  std::ostringstream out;
  for (const auto& [cell, versions] : versions_) {
    out << cell.ToString() << '{';
    for (const auto& [timestamp, value] : versions) {
      out << timestamp << '=';
      if (value.has_value()) {
        out << *value;
      } else {
        out << "<deleted>";
      }
      out << ';';
    }
    out << '}';
  }
  return out.str();
}

}  // namespace spanner_prod
