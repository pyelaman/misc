#include "spanner_prod/true_time.h"

#include <algorithm>
#include <stdexcept>
#include <thread>

namespace spanner_prod {
namespace {

Timestamp SystemMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

SystemTimeSource::SystemTimeSource(
    std::string name, std::chrono::microseconds maximum_error)
    : name_(std::move(name)), maximum_error_(maximum_error) {
  if (maximum_error_.count() < 0) {
    throw std::invalid_argument("time-source error bound cannot be negative");
  }
}

TimeSample SystemTimeSource::Sample() const {
  const Timestamp now = SystemMicros();
  return {Status::Ok(),
          {now - maximum_error_.count(), now + maximum_error_.count()},
          name_};
}

QuorumTrueTime::QuorumTrueTime(
    std::vector<std::shared_ptr<ITimeSource>> sources, std::size_t quorum)
    : sources_(std::move(sources)), quorum_(quorum) {
  if (sources_.empty() || quorum_ == 0 || quorum_ > sources_.size()) {
    throw std::invalid_argument("invalid TrueTime source quorum");
  }
}

StatusOr<TimeInterval> QuorumTrueTime::Now() const {
  std::vector<TimeInterval> intervals;
  for (const auto& source : sources_) {
    if (source == nullptr) {
      continue;
    }
    const auto sample = source->Sample();
    if (sample.status.ok() &&
        sample.interval.earliest <= sample.interval.latest) {
      intervals.push_back(sample.interval);
    }
  }
  if (intervals.size() < quorum_) {
    return Status::Error(StatusCode::kClockUncertain,
                         "too few healthy time sources");
  }

  // Marzullo-style sweep: retain the full region covered by at least quorum
  // intervals. Under the configured source-fault assumptions that region
  // conservatively contains actual time.
  std::vector<Timestamp> points;
  for (const auto& interval : intervals) {
    points.push_back(interval.earliest);
    points.push_back(interval.latest);
  }
  std::sort(points.begin(), points.end());
  std::optional<Timestamp> earliest;
  std::optional<Timestamp> latest;
  for (const auto point : points) {
    const auto coverage = static_cast<std::size_t>(std::count_if(
        intervals.begin(), intervals.end(), [&](const TimeInterval& interval) {
          return interval.earliest <= point && point <= interval.latest;
        }));
    if (coverage >= quorum_) {
      earliest = earliest.has_value() ? std::min(*earliest, point) : point;
      latest = latest.has_value() ? std::max(*latest, point) : point;
    }
  }
  if (!earliest.has_value()) {
    return Status::Error(StatusCode::kClockUncertain,
                         "time-source intervals have no quorum overlap");
  }
  return TimeInterval{*earliest, *latest};
}

Status QuorumTrueTime::CommitWait(
    Timestamp timestamp, std::chrono::milliseconds timeout) const {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    auto now = Now();
    if (!now.ok()) {
      return now.status();
    }
    if (now.value().earliest > timestamp) {
      return Status::Ok();
    }
    const auto remaining = timestamp - now.value().earliest + 1;
    std::this_thread::sleep_for(
        std::chrono::microseconds(std::clamp<Timestamp>(remaining, 1, 1000)));
  }
  return Status::Error(StatusCode::kClockUncertain,
                       "commit wait exceeded its deadline");
}

}  // namespace spanner_prod

