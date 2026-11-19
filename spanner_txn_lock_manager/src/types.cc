#include "spanner_lock/types.h"

#include <tuple>

namespace spanner_lock {
namespace {

void HashCombine(std::size_t& seed, std::size_t value) {
  seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
}

}  // namespace

bool Cell::operator<(const Cell& other) const {
  return std::tie(table, row, column) <
         std::tie(other.table, other.row, other.column);
}

std::string Cell::ToString() const {
  return table + "[" + row + "]." + column;
}

std::size_t CellHash::operator()(const Cell& cell) const noexcept {
  std::size_t seed = 0;
  HashCombine(seed, std::hash<std::string>{}(cell.table));
  HashCombine(seed, std::hash<std::string>{}(cell.row));
  HashCombine(seed, std::hash<std::string>{}(cell.column));
  return seed;
}

std::string_view ToString(LockMode mode) {
  switch (mode) {
    case LockMode::kReaderShared:
      return "ReaderShared";
    case LockMode::kWriterShared:
      return "WriterShared";
    case LockMode::kExclusive:
      return "Exclusive";
    case LockMode::kWriterSharedTimestamp:
      return "WriterSharedTimestamp";
  }
  return "Unknown";
}

bool Compatible(LockMode held, LockMode requested) {
  if (held == LockMode::kReaderShared &&
      requested == LockMode::kReaderShared) {
    return true;
  }
  if (held == LockMode::kWriterShared &&
      requested == LockMode::kWriterShared) {
    return true;
  }
  // WriterSharedTimestamp applies to a different insertion path. It conflicts
  // with itself and ReaderShared. Mixing it with WriterShared/Exclusive is an
  // invalid request at the storage adapter, not a compatible pair here.
  return false;
}

std::optional<LockMode> CombineModes(LockMode held, LockMode requested) {
  if (held == requested) {
    return held;
  }
  if (held == LockMode::kExclusive || requested == LockMode::kExclusive) {
    return LockMode::kExclusive;
  }
  if ((held == LockMode::kReaderShared &&
       requested == LockMode::kWriterShared) ||
      (held == LockMode::kWriterShared &&
       requested == LockMode::kReaderShared)) {
    return LockMode::kExclusive;
  }
  return std::nullopt;
}

bool ModeSubsumes(LockMode held, LockMode requested) {
  if (held == requested || held == LockMode::kExclusive) {
    return true;
  }
  return false;
}

std::string_view ToString(TransactionState state) {
  switch (state) {
    case TransactionState::kActive:
      return "Active";
    case TransactionState::kPreparing:
      return "Preparing";
    case TransactionState::kPrepared:
      return "Prepared";
    case TransactionState::kCommitting:
      return "Committing";
    case TransactionState::kCommitted:
      return "Committed";
    case TransactionState::kAborted:
      return "Aborted";
  }
  return "Unknown";
}

std::size_t CommandIdHash::operator()(const CommandId& id) const noexcept {
  std::size_t seed = std::hash<std::uint64_t>{}(id.client);
  HashCombine(seed, std::hash<std::uint64_t>{}(id.sequence));
  return seed;
}

std::ostream& operator<<(std::ostream& out, const Cell& cell) {
  return out << cell.ToString();
}

}  // namespace spanner_lock

