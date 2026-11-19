#include "spanner_prod/range_directory.h"

#include <algorithm>

namespace spanner_prod {

Status RangeDirectory::Add(SplitRange range) {
  if (range.group == nullptr ||
      (range.end_exclusive.has_value() &&
       range.start_inclusive >= *range.end_exclusive)) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "invalid split range");
  }
  auto proposed = ranges_;
  proposed.push_back(std::move(range));
  std::sort(proposed.begin(), proposed.end(),
            [](const SplitRange& lhs, const SplitRange& rhs) {
              return lhs.start_inclusive < rhs.start_inclusive;
            });
  for (std::size_t i = 1; i < proposed.size(); ++i) {
    if (!proposed[i - 1].end_exclusive.has_value() ||
        proposed[i].start_inclusive < *proposed[i - 1].end_exclusive) {
      return Status::Error(StatusCode::kInvalidArgument,
                           "split ranges overlap");
    }
  }
  ranges_ = std::move(proposed);
  return Status::Ok();
}

StatusOr<std::shared_ptr<SplitGroup>> RangeDirectory::Lookup(
    const Cell& cell) const {
  const std::string key = cell.RoutingKey();
  for (const auto& range : ranges_) {
    if (range.start_inclusive <= key &&
        (!range.end_exclusive.has_value() || key < *range.end_exclusive)) {
      return range.group;
    }
  }
  return Status::Error(StatusCode::kNotFound,
                       "no split owns routing key " + key);
}

std::vector<std::shared_ptr<SplitGroup>> RangeDirectory::groups() const {
  std::vector<std::shared_ptr<SplitGroup>> result;
  for (const auto& range : ranges_) {
    if (std::find(result.begin(), result.end(), range.group) == result.end()) {
      result.push_back(range.group);
    }
  }
  return result;
}

}  // namespace spanner_prod
