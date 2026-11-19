#pragma once

#include "spanner_prod/split_group.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace spanner_prod {

struct SplitRange {
  std::string start_inclusive;
  std::optional<std::string> end_exclusive;
  std::shared_ptr<SplitGroup> group;
};

class RangeDirectory {
 public:
  Status Add(SplitRange range);
  [[nodiscard]] StatusOr<std::shared_ptr<SplitGroup>> Lookup(
      const Cell& cell) const;
  [[nodiscard]] std::vector<std::shared_ptr<SplitGroup>> groups() const;

 private:
  std::vector<SplitRange> ranges_;
};

}  // namespace spanner_prod

