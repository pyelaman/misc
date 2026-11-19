#pragma once

#include "spanner_prod/types.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace spanner_prod {

struct VersionedValue {
  Timestamp timestamp{0};
  std::optional<std::string> value;
  bool operator==(const VersionedValue&) const = default;
};

class MvccStore {
 public:
  Status Apply(Timestamp commit_timestamp,
               const std::vector<Mutation>& mutations);
  [[nodiscard]] StatusOr<std::optional<std::string>> Read(
      const Cell& cell, Timestamp read_timestamp) const;
  [[nodiscard]] std::vector<VersionedValue> Versions(
      const Cell& cell) const;
  [[nodiscard]] std::string Fingerprint() const;

 private:
  std::map<Cell, std::map<Timestamp, std::optional<std::string>>> versions_;
};

}  // namespace spanner_prod

