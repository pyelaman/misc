#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spanner_lock {

using TransactionId = std::uint64_t;
using TransactionAge = std::uint64_t;
using LogIndex = std::uint64_t;
using LogicalTick = std::uint64_t;
using CommitTimestamp = std::int64_t;
using ShardId = std::uint32_t;

enum class StatusCode {
  kOk,
  kInvalidArgument,
  kNotFound,
  kFailedPrecondition,
  kAborted,
  kUnavailable,
  kInDoubt,
  kInternal,
};

struct Status {
  StatusCode code{StatusCode::kOk};
  std::string message;

  [[nodiscard]] bool ok() const { return code == StatusCode::kOk; }
  static Status Ok() { return {}; }
  static Status Error(StatusCode code, std::string message) {
    return Status{code, std::move(message)};
  }

  bool operator==(const Status&) const = default;
};

// A lockable Spanner-like data cell. Real Spanner also locks key ranges and
// internal row-existence cells; this reference core exposes an exact-cell key
// and leaves range-to-cell expansion/range indexing to the storage adapter.
struct Cell {
  std::string table;
  std::string row;
  std::string column;

  bool operator==(const Cell&) const = default;
  bool operator<(const Cell& other) const;
  [[nodiscard]] std::string ToString() const;
};

struct CellHash {
  std::size_t operator()(const Cell& cell) const noexcept;
};

enum class LockMode {
  kReaderShared,
  kWriterShared,
  kExclusive,
  kWriterSharedTimestamp,
};

std::string_view ToString(LockMode mode);
bool Compatible(LockMode held, LockMode requested);
std::optional<LockMode> CombineModes(LockMode held, LockMode requested);
bool ModeSubsumes(LockMode held, LockMode requested);

enum class TransactionState {
  kActive,
  kPreparing,
  kPrepared,
  kCommitting,
  kCommitted,
  kAborted,
};

std::string_view ToString(TransactionState state);

struct TransactionDescriptor {
  TransactionId id{0};
  // Smaller age means older and therefore higher wound-wait priority.
  TransactionAge age{0};
  LogicalTick deadline_tick{0};
  std::string tag;

  bool operator==(const TransactionDescriptor&) const = default;
};

struct CommandId {
  std::uint64_t client{0};
  std::uint64_t sequence{0};

  bool operator==(const CommandId&) const = default;
};

struct CommandIdHash {
  std::size_t operator()(const CommandId& id) const noexcept;
};

struct LockRequest {
  Cell cell;
  LockMode mode{LockMode::kReaderShared};
};

enum class AcquireDisposition { kGranted, kWaiting, kAborted };

struct GrantEvent {
  TransactionId transaction_id{0};
  Cell cell;
  LockMode mode{LockMode::kReaderShared};

  bool operator==(const GrantEvent&) const = default;
};

struct AcquireResult {
  AcquireDisposition disposition{AcquireDisposition::kGranted};
  std::vector<TransactionId> wounded_transactions;
  std::vector<GrantEvent> asynchronous_grants;
  std::size_t queue_position{0};
  std::string detail;

  bool operator==(const AcquireResult&) const = default;
};

struct LockMetrics {
  std::uint64_t acquire_requests{0};
  std::uint64_t immediate_grants{0};
  std::uint64_t queued_requests{0};
  std::uint64_t wounds{0};
  std::uint64_t releases{0};
  std::uint64_t expired_transactions{0};

  bool operator==(const LockMetrics&) const = default;
};

std::ostream& operator<<(std::ostream& out, const Cell& cell);

}  // namespace spanner_lock
