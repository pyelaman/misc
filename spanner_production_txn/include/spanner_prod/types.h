#pragma once

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace spanner_prod {

using Timestamp = std::int64_t;
using TransactionId = std::uint64_t;
using TransactionPriority = std::uint64_t;
using SplitId = std::uint32_t;
using Term = std::uint64_t;
using LogIndex = std::uint64_t;

enum class StatusCode {
  kOk,
  kInvalidArgument,
  kNotFound,
  kFailedPrecondition,
  kAborted,
  kUnavailable,
  kInDoubt,
  kCorruption,
  kClockUncertain,
  kInternal,
};

struct Status {
  StatusCode code{StatusCode::kOk};
  std::string message;

  [[nodiscard]] bool ok() const { return code == StatusCode::kOk; }
  static Status Ok() { return {}; }
  static Status Error(StatusCode code, std::string message) {
    return {code, std::move(message)};
  }
  bool operator==(const Status&) const = default;
};

template <typename T>
class StatusOr {
 public:
  StatusOr(Status status) : value_(std::move(status)) {}
  StatusOr(T value) : value_(std::move(value)) {}

  [[nodiscard]] bool ok() const { return std::holds_alternative<T>(value_); }
  [[nodiscard]] const Status& status() const {
    if (ok()) {
      static const Status kOk = Status::Ok();
      return kOk;
    }
    return std::get<Status>(value_);
  }
  [[nodiscard]] const T& value() const { return std::get<T>(value_); }
  [[nodiscard]] T& value() { return std::get<T>(value_); }

 private:
  std::variant<Status, T> value_;
};

struct Cell {
  std::string table;
  std::string row;
  std::string column;

  bool operator==(const Cell&) const = default;
  bool operator<(const Cell& other) const;
  [[nodiscard]] std::string RoutingKey() const;
  [[nodiscard]] std::string ToString() const;
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

struct LockRequest {
  Cell cell;
  LockMode mode{LockMode::kReaderShared};
  bool operator==(const LockRequest&) const = default;
};

struct Mutation {
  Cell cell;
  std::optional<std::string> value;
  bool operator==(const Mutation&) const = default;
};

struct TransactionDescriptor {
  TransactionId id{0};
  // Smaller values are older and have higher wound-wait priority.
  TransactionPriority priority{0};
  Timestamp deadline{0};
  SplitId coordinator_split{0};
  std::string tag;
  bool operator==(const TransactionDescriptor&) const = default;
};

struct PreparedTransaction {
  TransactionDescriptor transaction;
  Timestamp prepare_timestamp{0};
  std::vector<LockRequest> locks;
  std::vector<Mutation> mutations;
  bool operator==(const PreparedTransaction&) const = default;
};

enum class DecisionKind { kCommit, kAbort };

struct TransactionDecision {
  TransactionId transaction_id{0};
  DecisionKind kind{DecisionKind::kAbort};
  Timestamp commit_timestamp{0};
  std::vector<SplitId> participants;
  std::string reason;
  bool operator==(const TransactionDecision&) const = default;
};

struct PrepareCommand {
  PreparedTransaction prepared;
  bool operator==(const PrepareCommand&) const = default;
};

struct CommitCommand {
  TransactionId transaction_id{0};
  Timestamp commit_timestamp{0};
  bool operator==(const CommitCommand&) const = default;
};

struct AbortCommand {
  TransactionId transaction_id{0};
  std::string reason;
  bool operator==(const AbortCommand&) const = default;
};

struct RecordDecisionCommand {
  TransactionDecision decision;
  bool operator==(const RecordDecisionCommand&) const = default;
};

struct AdvanceSafeTimeCommand {
  Timestamp timestamp{0};
  bool operator==(const AdvanceSafeTimeCommand&) const = default;
};

using ReplicatedCommand =
    std::variant<PrepareCommand, CommitCommand, AbortCommand,
                 RecordDecisionCommand, AdvanceSafeTimeCommand>;

struct LogEntry {
  Term term{0};
  LogIndex index{0};
  std::uint64_t operation_id{0};
  ReplicatedCommand command;
  bool operator==(const LogEntry&) const = default;
};

std::ostream& operator<<(std::ostream& out, const Cell& cell);

}  // namespace spanner_prod

