#pragma once

#include "spanner_prod/types.h"

#include <string>
#include <string_view>

namespace spanner_prod {

std::string EncodeLogEntry(const LogEntry& entry);
StatusOr<LogEntry> DecodeLogEntry(std::string_view bytes);
std::uint32_t Checksum(std::string_view bytes);

}  // namespace spanner_prod

