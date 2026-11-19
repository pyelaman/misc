#pragma once

#include "spanner_prod/types.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace spanner_prod {

struct TimeInterval {
  Timestamp earliest{0};
  Timestamp latest{0};
};

struct TimeSample {
  Status status;
  TimeInterval interval;
  std::string source;
};

class ITimeSource {
 public:
  virtual ~ITimeSource() = default;
  [[nodiscard]] virtual TimeSample Sample() const = 0;
};

class SystemTimeSource final : public ITimeSource {
 public:
  SystemTimeSource(std::string name,
                   std::chrono::microseconds maximum_error);
  [[nodiscard]] TimeSample Sample() const override;

 private:
  std::string name_;
  std::chrono::microseconds maximum_error_;
};

class ITrueTime {
 public:
  virtual ~ITrueTime() = default;
  [[nodiscard]] virtual StatusOr<TimeInterval> Now() const = 0;
  virtual Status CommitWait(Timestamp timestamp,
                            std::chrono::milliseconds timeout) const = 0;
};

// Fault-aware interval clock. Sources stand in for independent GPS/atomic
// masters; production deployments must supply authenticated hardware-backed
// implementations and conservatively enforce their drift bounds.
class QuorumTrueTime final : public ITrueTime {
 public:
  QuorumTrueTime(std::vector<std::shared_ptr<ITimeSource>> sources,
                 std::size_t quorum);
  [[nodiscard]] StatusOr<TimeInterval> Now() const override;
  Status CommitWait(Timestamp timestamp,
                    std::chrono::milliseconds timeout) const override;

 private:
  std::vector<std::shared_ptr<ITimeSource>> sources_;
  std::size_t quorum_;
};

}  // namespace spanner_prod

