#include "spanner_prod/types.h"

#include <tuple>

namespace spanner_prod {

bool Cell::operator<(const Cell& other) const {
  return std::tie(table, row, column) <
         std::tie(other.table, other.row, other.column);
}

std::string Cell::RoutingKey() const { return table + "\x1f" + row; }

std::string Cell::ToString() const {
  return table + "/" + row + "/" + column;
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
  return (held == LockMode::kReaderShared &&
          requested == LockMode::kReaderShared) ||
         (held == LockMode::kWriterShared &&
          requested == LockMode::kWriterShared);
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
  return held == requested || held == LockMode::kExclusive;
}

std::ostream& operator<<(std::ostream& out, const Cell& cell) {
  return out << cell.ToString();
}

}  // namespace spanner_prod

